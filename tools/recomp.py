"""ARM/Thumb -> C static recompiler (code generator).

Reads functions.json produced by analyze.py and writes C sources that
implement each guest function as a C function operating on the runtime ABI
in runtime/gba.h + runtime/recomp.h.

Model
-----
* Guest registers live in C locals inside a function.  They are loaded from
  the global `cpu` at entry (only registers live on entry), stored before
  calls / at exits (registers the function writes), and reloaded after calls
  (only registers live afterwards).
* Flags are four 0/1 locals n z c v.  A compare immediately followed by a
  conditional branch in the same block fuses into one C comparison; unused
  flag computations are removed by the C compiler.
* Control flow: in-function branches are gotos.  Every function remembers the
  link register at entry (`ret`); any indirect transfer whose target equals
  `ret` is a return.  Other indirect targets go through the function's
  dispatch block: local jump-table/resume labels, else gba_jump() which runs a
  known function deeper, else the function unwinds to its caller with
  cpu.next_pc = target so a frame further up can continue there.
* cpu.next_pc always holds the guest address a returning function went to;
  callers verify it against the expected link value.
"""
import json
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
from armdecode import Rom, CodeSpace, COND_AL, CONDS  # noqa: E402

from recomp_consts import (FLAGS, ALLREGS, LOGICAL, TESTS, COND_FLAGS,  # noqa: E402
                           bit, regs_of, use_def_basic)
from abi import Summaries  # noqa: E402


# assignments to the temporaries a fused compare-and-branch reads
LAZY_SUB_WRITE = re.compile(r'\b(fa|fb|fr)\s*=(?!=)')
LAZY_FR_WRITE = re.compile(r'\bfr\s*=(?!=)')


def fname(addr, thumb):
    return ('T_%08X' if thumb else 'A_%08X') % addr


def hx(v):
    return '0x%08Xu' % (v & 0xffffffff)


# ------------------------------------------------------------ hooks

# --trace: every function starts with 'if (TRACE_ENTER(addr)) return;' (a
# no-op unless built with -DRECOMP_TRACE and a gba_trace_enter(), which can
# log entries or hand functions to the interpreter; runtime/recomp.h)
TRACE = '--trace' in sys.argv

# pc -> C function name; filled from game.toml
OVERRIDE_LOAD = {}      # value loaded at pc is passed through func(value)
HOOK_BEFORE = {}        # func(const uint32_t regs[15]) runs before pc
NATIVE = {}             # (addr, thumb) -> C function replacing the routine


# ------------------------------------------------------------ function

class FuncGen:
    def __init__(self, cs, fd, funcs_by_key, opts):
        self.cs = cs
        self.addr = fd['addr']
        self.thumb = fd['thumb']
        self.T = 1 if self.thumb else 0
        self.opts = opts
        self.funcs = funcs_by_key
        self.addrs = list(fd['insns'])
        self.addrset = set(self.addrs)
        self.labels = set(fd['labels'])
        self.jumptables = {int(k): v for k, v in fd.get('jumptables', {}).items()}
        self.const_branch = {int(k): v for k, v in fd.get('const_branch', {}).items()}
        self.ins = {a: cs.decode(a, self.thumb) for a in self.addrs}
        self.lines = []
        self.resume = set()   # labels reachable from the dispatch block

    # -------------------------------------------- classification
    def classify(self, i):
        """Control-transfer kind of instruction i."""
        a = i.addr
        op = i.op
        if op == 'bl':
            return 'call'
        if op == 'swi':
            return 'swi'
        if op == 'msr' and not i.spsr and not (i.mask & 1):
            return 'normal'          # flags-only write
        if op in ('mrs', 'msr'):
            return 'psr'
        if op == 'b':
            if i.target in self.labels and i.target in self.addrset:
                return 'branch'
            return 'tail'
        if a in self.const_branch:
            t, tt, is_call = self.const_branch[a]
            return 'ccall' if is_call else 'ctail'
        if op == 'bx' or i.writes_pc():
            nxt = i.next
            prev = self.ins.get(a - (2 if self.thumb else 4))
            if (prev is not None and prev.op == 'dp' and prev.alu == 'mov' and prev.rd == 14
                    and prev.op2[0] == 'reg' and prev.op2[1] == 15 and prev.pcval == nxt):
                return 'icall'
            return 'indirect'
        return 'normal'

    # -------------------------------------------- CFG + liveness
    def build(self):
        """Classify instructions, build successors, resume labels and the set
        of registers this function writes."""
        self.kind = {}
        self.succ = {}
        written = 0
        wflags = 0
        for a in self.addrs:
            i = self.ins[a]
            k = self.classify(i)
            self.kind[a] = k
            u, d = use_def_basic(i)
            if k in ('call', 'icall', 'ccall'):
                written |= bit(14)
            elif k == 'swi' or (k == 'psr' and i.op == 'msr'):
                pass
            else:
                written |= d & ALLREGS
                wflags |= d & FLAGS
        self.written = written
        self.wflags = wflags
        for a in self.addrs:
            k = self.kind[a]
            i = self.ins[a]
            if k in ('call', 'icall', 'ccall', 'swi'):
                if i.next in self.addrset:
                    self.resume.add(i.next)
        for tl in self.jumptables.values():
            for t in tl:
                if t in self.addrset:
                    self.resume.add(t)
        if self.addr in self.addrset:
            self.resume.add(self.addr)
        self.labels |= self.resume
        for a in self.addrs:
            i = self.ins[a]
            k = self.kind[a]
            s = []
            cond = i.cond != COND_AL
            nxt = i.next
            if k == 'branch':
                s.append(i.target)
                if cond:
                    s.append(nxt)
            elif k in ('tail', 'ctail', 'indirect'):
                s.append('D')
                if cond:
                    s.append(nxt)
            else:
                s.append(nxt)
            self.succ[a] = [x for x in s if x == 'D' or x in self.addrset]

    def falls_off_end(self):
        if not self.addrs:
            return True
        for a in self.addrs:
            i = self.ins[a]
            if self.falls_through(i) and i.next not in self.addrset:
                return True
        return False

    def set_summaries(self, summ):
        """Per-instruction liveness under the converged callee summaries."""
        self.summ = summ
        key = (self.addr, self.thumb)
        self.MOD = summ.MOD[key]
        self.IN = summ.IN[key]
        unknown = self.MOD == (ALLREGS | FLAGS)
        _, self.live, self.dlive = summ.live_in(self, self.MOD, summ.saved[key], unknown)
        self.synced = 0

    def live_out(self, a):
        out = 0
        for s in self.succ[a]:
            out |= self.dlive if s == 'D' else self.live[s]
        return out

    # -------------------------------------------- emission helpers
    def e(self, s):
        self.lines.append(s)

    def r(self, n, i=None):
        if n == 15:
            return hx(i.pcval)
        return 'r%d' % n

    def sync_out(self, mask=ALLREGS | FLAGS, extra=0):
        """Store registers in mask that this function may have changed."""
        regs = regs_of((mask & self.written) | extra)
        for r in regs:
            self.synced |= bit(r)
        s = ' '.join('cpu.r[%d] = r%d;' % (r, r) for r in regs)
        if mask & self.wflags:
            s += ' cpu.n = n; cpu.z = z; cpu.c = c; cpu.v = v;'
        return s

    def sync_in(self, mask):
        regs = regs_of(mask & ALLREGS)
        s = ' '.join('r%d = cpu.r[%d];' % (r, r) for r in regs)
        if mask & FLAGS:
            s += ' n = cpu.n; z = cpu.z; c = cpu.c; v = cpu.v;'
        return s

    def cond_expr(self, cond):
        lz = self.lazy
        if lz is not None:
            k = lz
            if k == 'sub':
                a, b = 'fa', 'fb'
                m = {0: '%s == %s' % (a, b), 1: '%s != %s' % (a, b),
                     2: '%s >= %s' % (a, b), 3: '%s < %s' % (a, b),
                     4: '(int32_t)fr < 0', 5: '(int32_t)fr >= 0',
                     8: '%s > %s' % (a, b), 9: '%s <= %s' % (a, b),
                     10: '(int32_t)%s >= (int32_t)%s' % (a, b),
                     11: '(int32_t)%s < (int32_t)%s' % (a, b),
                     12: '(int32_t)%s > (int32_t)%s' % (a, b),
                     13: '(int32_t)%s <= (int32_t)%s' % (a, b)}
                if cond in m:
                    return m[cond]
            elif k == 'logic':
                m = {0: 'fr == 0', 1: 'fr != 0', 4: '(int32_t)fr < 0', 5: '(int32_t)fr >= 0'}
                if cond in m:
                    return m[cond]
        return {0: 'z', 1: '!z', 2: 'c', 3: '!c', 4: 'n', 5: '!n', 6: 'v', 7: '!v',
                8: '(c && !z)', 9: '(!c || z)', 10: '(n == v)', 11: '(n != v)',
                12: '(!z && n == v)', 13: '(z || n != v)'}[cond]

    def op2(self, o, i, want_carry):
        """Return (expr, pre_statements) for operand 2; sets c if want_carry."""
        pre = []
        if o[0] == 'imm':
            if want_carry and o[2] is not None:
                pre.append('c = %d;' % o[2])
            return hx(o[1]), pre
        if o[0] == 'reg':
            rm = self.r(o[1], i)
            st, amt = o[2], o[3]
            if st == 'lsl':
                if amt == 0:
                    return rm, pre
                if want_carry:
                    pre.append('c = (%s >> %d) & 1;' % (rm, 32 - amt))
                return '(%s << %d)' % (rm, amt), pre
            if st == 'lsr':
                if want_carry:
                    pre.append('c = (%s >> %d) & 1;' % (rm, amt - 1))
                return ('0u' if amt == 32 else '(%s >> %d)' % (rm, amt)), pre
            if st == 'asr':
                if want_carry:
                    pre.append('c = (%s >> %d) & 1;' % (rm, min(amt - 1, 31)))
                return '(uint32_t)((int32_t)%s >> %d)' % (rm, min(amt, 31)), pre
            if st == 'ror':
                if want_carry:
                    pre.append('c = (%s >> %d) & 1;' % (rm, amt - 1))
                return '((%s >> %d) | (%s << %d))' % (rm, amt, rm, 32 - amt), pre
            if st == 'rrx':
                # carry-in must be read before c is overwritten
                pre.append('ft = (c << 31) | (%s >> 1);' % rm)
                if want_carry:
                    pre.append('c = %s & 1;' % rm)
                return 'ft', pre
        # shift by register
        rm = self.r(o[1], i)
        rs = self.r(o[3], i)
        st = o[2]
        if want_carry:
            pre.append('ft = sh_%s_c(%s, %s, &c);' % (st, rm, rs))
            return 'ft', pre
        return 'sh_%s(%s, %s)' % (st, rm, rs), pre

    # -------------------------------------------- jumps
    def jump(self, expr, mode):
        """Indirect transfer to the address in C expression expr.
        mode: 'bx' (bit0 selects state), 'same' (stay in current state)."""
        if mode == 'bx':
            return 't = %s; goto L_dispatch;' % expr
        if self.thumb:
            return 't = (%s) | 1u; goto L_dispatch;' % expr
        return 't = (%s) & ~3u; goto L_dispatch;' % expr

    def call_known(self, target, tthumb, link, live):
        key = (target, tthumb)
        cin, cmod = self.summ.callee_summary(key)
        stmts = ['r14 = %s;' % hx(link), self.sync_out(cin, bit(14))]
        if key in self.funcs:
            stmts.append('%s();' % fname(target, tthumb))
        else:
            stmts.append('gba_call(%s);' % hx(target | (1 if tthumb else 0)))
        stmts.append('if (unlikely(cpu.next_pc != %s)) goto L_resync;' % hx(link))
        stmts.append(self.sync_in(live & cmod))
        return stmts

    def tail_known(self, target, tthumb):
        key = (target, tthumb)
        stmts = [self.sync_out()]
        if key in self.funcs:
            stmts.append('%s();' % fname(target, tthumb))
            stmts.append('t = cpu.next_pc; if (((t ^ ret) >> 1) == 0) return;')
            stmts.append('goto L_resync;')
        else:
            stmts.append('t = %s; goto L_dispatch;' % hx(target | (1 if tthumb else 0)))
        return stmts

    # -------------------------------------------- instruction emission
    def emit_ins(self, i):
        out = []
        if i.addr in HOOK_BEFORE:
            out.append('{ const uint32_t hr[15] = { r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, '
                       'r11, r12, r13, r14 }; %s(hr); }' % HOOK_BEFORE[i.addr])
        body = self.emit_ins_core(i)
        if i.addr in OVERRIDE_LOAD and i.op == 'mem' and i.load and i.rd != 15:
            body = body + ['r%d = %s(r%d);' % (i.rd, OVERRIDE_LOAD[i.addr], i.rd)]
        return out + body

    def emit_ins_core(self, i):
        a = i.addr
        k = self.kind[a]
        out = []
        op = i.op

        if k == 'branch':
            if i.cond == COND_AL:
                return ['goto L_%08X;' % i.target]
            return ['if (%s) goto L_%08X;' % (self.cond_expr(i.cond), i.target)]
        if k == 'tail':
            body = self.tail_known(i.target, self.thumb)
            return self.wrap_cond(i, body)
        if k == 'call':
            return self.wrap_cond(i, self.call_known(i.target, self.thumb, i.next | self.T,
                                                     self.live_out(a)))
        if k in ('ccall', 'ctail'):
            t, tt, is_call = self.const_branch[a]
            if k == 'ccall':
                return self.wrap_cond(i, self.call_known(t, tt, i.next | self.T, self.live_out(a)))
            return self.wrap_cond(i, self.tail_known(t, tt))
        if k == 'swi':
            num = i.num
            live = self.live_out(a)
            return self.wrap_cond(i, [self.sync_out(0xF | bit(13)), 'gba_swi(0x%02X, %s, %d);' % (num, hx(i.next), self.T),
                                      self.sync_in(live & (0xF | bit(12) | FLAGS))])
        if k == 'psr':
            live = self.live_out(a)
            if op == 'mrs':
                body = ['cpu.n = n; cpu.z = z; cpu.c = c; cpu.v = v; r%d = gba_mrs(%d);'
                        % (i.rd, 1 if i.spsr else 0)]
                return self.wrap_cond(i, body)
            val, pre = self.op2(i.op2, i, False)
            body = pre + [self.sync_out() + ' cpu.n = n; cpu.z = z; cpu.c = c; cpu.v = v;',
                          'gba_msr(%s, %d, %d);' % (val, i.mask, 1 if i.spsr else 0),
                          self.sync_in(ALLREGS | FLAGS)]
            return self.wrap_cond(i, body)
        if k == 'icall':
            # mov lr, pc ; bx rX / ldr pc,[..]   (lr already set by the mov)
            if op == 'bx':
                tgt = self.r(i.rm, i)
            else:
                tgt = None
            body = []
            if tgt is None:
                body += self.emit_plain(i, pc_target_var='ft2')
                tgt = 'ft2'
            body += [self.sync_out(self.summ.ind_in, bit(14)), 'gba_call(%s);' % tgt,
                     'if (unlikely(cpu.next_pc != r14)) goto L_resync;',
                     self.sync_in(self.live_out(a) & self.summ.ind_mod)]
            return self.wrap_cond(i, body)
        if k == 'indirect':
            if op == 'bx':
                body = [self.jump(self.r(i.rm, i), 'bx')]
            else:
                body = self.emit_plain(i, pc_target_var='ft2')
                if op == 'dp' and i.s:
                    # movs pc, lr / subs pc, lr, #4: exception return
                    body = body + ['%s cpu.next_pc = ft2; gba_exc_return(); return;' % self.sync_out()]
                elif op == 'ldm' and i.user:
                    body = body + ['%s cpu.next_pc = ft2; gba_exc_return(); return;' % self.sync_out()]
                else:
                    body.append(self.jump('ft2', 'same'))
            return self.wrap_cond(i, body)
        return self.wrap_cond(i, self.emit_plain(i))

    def wrap_cond(self, i, body):
        if i.cond == COND_AL:
            return body
        return ['if (%s) {' % self.cond_expr(i.cond)] + ['  ' + b for b in body] + ['}']

    def emit_plain(self, i, pc_target_var=None):
        """Emit a non-control instruction (or the data part of a PC write,
        storing the new PC in pc_target_var)."""
        op = i.op
        out = []
        if op == 'dp':
            alu = i.alu
            logical = alu in LOGICAL
            want_c = i.s and logical
            b, pre = self.op2(i.op2, i, want_c)
            out += pre
            rn = self.r(i.rn, i) if alu not in ('mov', 'mvn') else None
            dst = pc_target_var if i.rd == 15 else 'r%d' % i.rd
            if logical:
                expr = {'and': '%s & %s', 'eor': '%s ^ %s', 'tst': '%s & %s', 'teq': '%s ^ %s',
                        'orr': '%s | %s', 'bic': '%s & ~%s'}.get(alu)
                if alu == 'mov':
                    ex = b
                elif alu == 'mvn':
                    ex = '~%s' % b
                else:
                    ex = expr % (rn, b)
                if i.s:
                    out.append('fr = %s;' % ex)
                    if alu not in TESTS:
                        out.append('%s = fr;' % dst)
                    out.append('n = fr >> 31; z = fr == 0;')
                elif alu not in TESTS:
                    out.append('%s = %s;' % (dst, ex))
                return out
            # arithmetic
            if alu in ('rsb', 'rsc'):
                x, y = b, rn
            else:
                x, y = rn, b
            if alu in ('sub', 'rsb', 'cmp'):
                if i.s:
                    out.append('fa = %s; fb = %s; fr = fa - fb;' % (x, y))
                    out.append('n = fr >> 31; z = fr == 0; c = fa >= fb; v = SUB_V(fa, fb, fr);')
                    if alu != 'cmp':
                        out.append('%s = fr;' % dst)
                else:
                    out.append('%s = %s - %s;' % (dst, x, y))
            elif alu in ('add', 'cmn'):
                if i.s:
                    out.append('fa = %s; fb = %s; fr = fa + fb;' % (x, y))
                    out.append('n = fr >> 31; z = fr == 0; c = fr < fa; v = ADD_V(fa, fb, fr);')
                    if alu != 'cmn':
                        out.append('%s = fr;' % dst)
                else:
                    out.append('%s = %s + %s;' % (dst, x, y))
            elif alu == 'adc':
                if i.s:
                    out.append('fa = %s; fb = %s; { uint64_t s64 = (uint64_t)fa + fb + c; fr = (uint32_t)s64;'
                               ' n = fr >> 31; z = fr == 0; v = ADD_V(fa, fb, fr); c = (uint32_t)(s64 >> 32); }' % (x, y))
                    out.append('%s = fr;' % dst)
                else:
                    out.append('%s = %s + %s + c;' % (dst, x, y))
            elif alu in ('sbc', 'rsc'):
                if i.s:
                    out.append('fa = %s; fb = %s; fr = fa - fb - (c ^ 1);' % (x, y))
                    out.append('n = fr >> 31; z = fr == 0; v = SUB_V(fa, fb, fr);'
                               ' c = (uint64_t)fa >= (uint64_t)fb + (c ^ 1);')
                    out.append('%s = fr;' % dst)
                else:
                    out.append('%s = %s - %s - (c ^ 1);' % (dst, x, y))
            return out
        if op == 'mul':
            ex = 'r%d * r%d' % (i.rm, i.rs)
            if i.acc:
                ex += ' + r%d' % i.rn
            out.append('r%d = %s;' % (i.rd, ex))
            if i.s:
                out.append('n = r%d >> 31; z = r%d == 0;' % (i.rd, i.rd))
            return out
        if op == 'mull':
            if i.sgn:
                ex = '(uint64_t)((int64_t)(int32_t)r%d * (int64_t)(int32_t)r%d)' % (i.rm, i.rs)
            else:
                ex = '(uint64_t)r%d * (uint64_t)r%d' % (i.rm, i.rs)
            if i.acc:
                ex += ' + (((uint64_t)r%d << 32) | r%d)' % (i.rdhi, i.rd)
            out.append('{ uint64_t m64 = %s; r%d = (uint32_t)m64; r%d = (uint32_t)(m64 >> 32);' % (ex, i.rd, i.rdhi))
            if i.s:
                out.append('  n = r%d >> 31; z = m64 == 0;' % i.rdhi)
            out.append('}')
            return out
        if op == 'msr':
            val, pre = self.op2(i.op2, i, False)
            return pre + ['ft = %s; n = ft >> 31; z = (ft >> 30) & 1; c = (ft >> 29) & 1; v = (ft >> 28) & 1;'
                          % val]
        if op == 'swp':
            if i.msize == 1:
                out.append('ft = rd8(r%d); wr8(r%d, r%d); r%d = ft;' % (i.rn, i.rn, i.rm, i.rd))
            else:
                out.append('ft = rd32(r%d); wr32(r%d, r%d); r%d = ft;' % (i.rn, i.rn, i.rm, i.rd))
            return out
        if op == 'mem':
            return self.emit_mem(i, pc_target_var)
        if op == 'ldm':
            return self.emit_ldm(i, pc_target_var)
        if op == 'undef' or op in ('blprefix', 'blh'):
            return ['%s gba_undefined(%s, %s);' % (self.sync_out(), hx(i.addr), hx(i.raw))]
        return ['/* unhandled %s */ gba_undefined(%s, %s);' % (op, hx(i.addr), hx(i.raw))]

    def const_rom(self, addr, size):
        if size == 4 and addr & 3:
            return None
        p = self.cs.phys(addr)
        if p is None or not self.cs.rom.in_rom(p + size - 1):
            return None
        if size == 4:
            return self.cs.u32(addr)
        v = self.cs.u16(addr & ~1)
        if size == 1:
            return (v >> (8 * (addr & 1))) & 0xff
        return v

    def emit_mem(self, i, pc_target_var):
        out = []
        size = i.msize
        # address
        if i.off[0] == 'imm':
            offv = i.off[1]
            off_expr = None
        else:
            offv = None
            o = i.off
            rm = self.r(o[1], i)
            if o[2] == 'lsl':
                off_expr = rm if o[3] == 0 else '(%s << %d)' % (rm, o[3])
            elif o[2] == 'lsr':
                off_expr = '0u' if o[3] == 32 else '(%s >> %d)' % (rm, o[3])
            elif o[2] == 'asr':
                off_expr = '(uint32_t)((int32_t)%s >> %d)' % (rm, min(o[3], 31))
            elif o[2] == 'ror':
                off_expr = '((%s >> %d) | (%s << %d))' % (rm, o[3], rm, 32 - o[3])
            else:
                off_expr = '((c << 31) | (%s >> 1))' % rm
        sign = '+' if i.up else '-'
        const_addr = None
        if i.rn == 15:
            base = i.pcval
            if off_expr is None and i.pre:
                const_addr = (base + (offv if i.up else -offv)) & 0xffffffff
            base_e = hx(base)
        else:
            base_e = 'r%d' % i.rn
            kb = self.kconst.get(i.rn)
            if kb is not None:
                ko = offv
                if off_expr is not None:
                    ko = None
                    o = i.off
                    if o[2] == 'lsl' and o[1] in self.kconst:
                        ko = (self.kconst[o[1]] << o[3]) & 0xffffffff
                if ko is not None:
                    const_addr = ((kb + (ko if i.up else -ko)) if i.pre else kb) & 0xffffffff
        if off_expr is None:
            ea = base_e if offv == 0 else '%s %s %du' % (base_e, sign, offv)
        else:
            ea = '%s %s %s' % (base_e, sign, off_expr)
        addr_e = ea if i.pre else base_e
        on_stack = i.rn == 13 and size == 4
        dst = (pc_target_var if i.rd == 15 else 'r%d' % i.rd)
        if i.load:
            if const_addr is not None:
                v = self.const_rom(const_addr, size)
                if v is not None:
                    if i.signed:
                        v = (v ^ (0x80 if size == 1 else 0x8000)) - (0x80 if size == 1 else 0x8000)
                    out.append('%s = %s;' % (dst, hx(v)))
                    if i.wb and i.rn != 15 and i.rn != i.rd:
                        out.append('r%d = %s;' % (i.rn, ea))
                    return out
                ld = self.const_load(const_addr, size, i.signed)
                if ld is not None:
                    if i.wb and i.rn != 15 and i.rn != i.rd:
                        out.append('ft = %s;' % ld)
                        out.append('r%d = %s;' % (i.rn, ea))
                        out.append('%s = ft;' % dst)
                    else:
                        out.append('%s = %s;' % (dst, ld))
                    return out
            if on_stack:
                ld = 'sp_rd32(%s)' % addr_e
            elif size == 4:
                if i.pre and i.up and off_expr is None and 0 < offv < 0x1000:
                    ld = 'rd32o(%s, %du)' % (base_e, offv)
                else:
                    ld = 'rd32(%s)' % addr_e
            elif size == 2:
                ld = 'rd16s(%s)' % addr_e if i.signed else 'rd16(%s)' % addr_e
            else:
                ld = '(uint32_t)(int32_t)(int8_t)rd8(%s)' % addr_e if i.signed else 'rd8(%s)' % addr_e
            if i.signed and size == 2:
                ld = '(uint32_t)' + ld
            if i.wb and i.rn != 15 and i.rn != i.rd:
                out.append('ft = %s;' % ld)
                out.append('r%d = %s;' % (i.rn, ea))
                out.append('%s = ft;' % dst)
            else:
                out.append('%s = %s;' % (dst, ld))
            return out
        # store
        if i.rd == 15:
            val = hx(i.pcval + (4 if not self.thumb else 0))
        else:
            val = 'r%d' % i.rd
        st = self.const_store(const_addr, size, val) if const_addr is not None else None
        if st is not None:
            pass
        elif on_stack:
            st = 'sp_wr32(%s, %s);' % (addr_e, val)
        else:
            if size == 4 and i.pre and i.up and off_expr is None and 0 < offv < 0x1000:
                st = 'wr32o(%s, %du, %s);' % (base_e, offv, val)
            else:
                st = {4: 'wr32', 2: 'wr16', 1: 'wr8'}[size] + '(%s, %s);' % (addr_e, val)
        out.append(st)
        if i.wb and i.rn != 15:
            out.append('r%d = %s;' % (i.rn, ea))
        return out

    def const_load(self, a, size, signed):
        """C expression loading a compile-time-known guest address, or None."""
        if a & (size - 1):
            return None
        r = a >> 24
        if r == 0x03:
            e = {4: 'IW32(%s)', 2: 'IW16(%s)', 1: 'IW8(%s)'}[size] % hx(a)
        elif r == 0x02:
            e = {4: 'EW32(%s)', 2: 'EW16(%s)', 1: 'EW8(%s)'}[size] % hx(a)
        elif r == 0x04 and (a & 0xFFFFFF) < 0x400:
            e = {4: 'gba_io_read32(%s)', 2: 'gba_io_read16(%s)', 1: 'gba_io_read8(%s)'}[size] % hx(a & 0xFFFFFF)
        else:
            return None
        if signed:
            e = '(uint32_t)(int32_t)(%s)%s' % ('int16_t' if size == 2 else 'int8_t', e)
        return e

    def const_store(self, a, size, val):
        if a & (size - 1):
            return None
        r = a >> 24
        if r == 0x03:
            return {4: 'IW32(%s) = %s;', 2: 'IW16(%s) = (uint16_t)%s;', 1: 'IW8(%s) = (uint8_t)%s;'}[size] % (hx(a), val)
        if r == 0x02:
            return {4: 'EW32(%s) = %s;', 2: 'EW16(%s) = (uint16_t)%s;', 1: 'EW8(%s) = (uint8_t)%s;'}[size] % (hx(a), val)
        if r == 0x04 and (a & 0xFFFFFF) < 0x400:
            return {4: 'gba_io_write32(%s, %s);', 2: 'gba_io_write16(%s, %s);',
                    1: 'gba_io_write8(%s, %s);'}[size] % (hx(a & 0xFFFFFF), val)
        return None

    def kconst_update(self, i):
        """Track registers holding compile-time constants within a block."""
        k = self.kind[i.addr]
        if k != 'normal':
            self.kconst.clear()
            return
        kc = self.kconst
        newv = None
        if i.cond == COND_AL:
            if i.op == 'mem' and i.load and i.rn == 15 and i.off[0] == 'imm' and i.pre and i.msize == 4:
                newv = self.const_rom((i.pcval + (i.off[1] if i.up else -i.off[1])) & 0xffffffff, 4)
            elif i.op == 'dp' and i.rd < 15 and i.alu not in TESTS:
                newv = self.const_dp(i, kc)
        u, d = use_def_basic(i)
        for r in list(kc):
            if d & bit(r):
                del kc[r]
        if newv is not None and i.op in ('mem', 'dp'):
            kc[i.rd] = newv & 0xffffffff

    @staticmethod
    def const_dp(i, kc):
        o = i.op2
        if o[0] == 'imm':
            b = o[1]
        elif o[0] == 'reg' and o[1] in kc and o[2] in ('lsl', 'lsr', 'asr'):
            x, n = kc[o[1]], o[3]
            if o[2] == 'lsl':
                b = (x << n) & 0xffffffff
            elif o[2] == 'lsr':
                b = 0 if n >= 32 else x >> n
            else:
                b = ((x - (1 << 32) if x & 0x80000000 else x) >> min(n, 31)) & 0xffffffff
        else:
            return None
        if i.alu == 'mov':
            return b
        if i.alu == 'mvn':
            return ~b & 0xffffffff
        if i.rn == 15:
            a = i.pcval
        elif i.rn in kc:
            a = kc[i.rn]
        else:
            return None
        return {'add': a + b, 'sub': a - b, 'rsb': b - a, 'orr': a | b, 'and': a & b,
                'eor': a ^ b, 'bic': a & ~b}.get(i.alu)

    def emit_ldm(self, i, pc_target_var):
        out = []
        regs = list(i.regs)
        n = len(regs)
        rn = 'r%d' % i.rn
        if n == 0:
            return ['/* empty ldm/stm */']
        if i.up:
            start = '%s + 4u' % rn if i.pre else rn
            newb = '%s + %du' % (rn, 4 * n)
        else:
            start = '%s - %du' % (rn, 4 * n) if i.pre else '%s - %du' % (rn, 4 * n - 4)
            newb = '%s - %du' % (rn, 4 * n)
        stack = i.rn == 13
        rdf = 'sp_rd32' if stack else 'rd32'
        wrf = 'sp_wr32' if stack else 'wr32'
        out.append('fa = %s;' % start)
        if i.user and not (i.load and 15 in regs):
            # user-bank transfer
            mask = sum(1 << r for r in regs)
            out.append('%s %s(fa, 0x%04X);' % (self.sync_out(),
                                               'gba_ldm_user' if i.load else 'gba_stm_user', mask))
            if i.load:
                out.append(self.sync_in(ALLREGS))
            if i.wb:
                out.append('%s = %s;' % (rn, newb))
            return out
        if i.load:
            wb = i.wb and i.rn not in regs
            if wb:
                out.append('%s = %s;' % (rn, newb))
            for k, r in enumerate(regs):
                ea = 'fa' if k == 0 else 'fa + %du' % (4 * k)
                if r == 15:
                    out.append('%s = %s(%s);' % (pc_target_var or 'ft2', rdf, ea))
                else:
                    out.append('r%d = %s(%s);' % (r, rdf, ea))
            return out
        for k, r in enumerate(regs):
            ea = 'fa' if k == 0 else 'fa + %du' % (4 * k)
            if r == 15:
                val = hx(i.pcval + (4 if not self.thumb else 0))
            elif r == i.rn and i.wb and k != 0:
                val = '(%s)' % newb
            else:
                val = 'r%d' % r
            out.append('%s(%s, %s);' % (wrf, ea, val))
        if i.wb:
            out.append('%s = %s;' % (rn, newb))
        return out

    # -------------------------------------------- whole function
    def generate(self):
        name = fname(self.addr, self.thumb)
        self.e('void %s(void) {' % name)
        self.e('  uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14;')
        self.e('  uint32_t n, z, c, v, fa, fb, fr, ft, ft2, t;')
        self.e('  const uint32_t ret = cpu.r[14];')
        if TRACE:
            self.e('  if (TRACE_ENTER(%s)) return;' % hx(self.addr | self.T))
        if getattr(self, 'warn_entry', False):
            self.e('  gba_unexpected(%s);' % hx(self.addr | self.T))
        entry_live = self.live[self.addr]
        self.e('  ' + self.sync_in(entry_live))
        unl = [r for r in range(15) if not entry_live & bit(r)]
        if unl:
            self.e('  ' + ' = '.join('r%d' % r for r in unl) + ' = 0;')
        self.e('  (void)fa; (void)fb; (void)fr; (void)ft; (void)ft2; (void)n; (void)z; (void)c; (void)v;')
        if not (entry_live & FLAGS):
            self.e('  n = z = c = v = 0;')
        # registers that are never loaded still need a defined value for
        # sync_out paths that store them (they are in written, so loaded).
        # The body is emitted in address order: when code below the entry
        # belongs to the function (a shared tail reached by a branch), jump
        # to the entry first, unless the entry is an unconditional branch to
        # the first instruction anyway.
        first = self.addrs[0] if self.addrs else self.addr
        if first != self.addr:
            ei = self.ins[self.addr]
            if not (self.kind[self.addr] == 'branch' and ei.cond == COND_AL and ei.target == first):
                self.e('  goto L_%08X;' % self.addr)
        prev_falls = True
        self.lazy = None
        self.kconst = {}
        prev_addr = None
        for a in self.addrs:
            i = self.ins[a]
            if prev_addr is not None and self.ins[prev_addr].next != a and prev_falls:
                nx = self.ins[prev_addr].next
                if nx in self.addrset and nx in self.labels:
                    self.e('  goto L_%08X;' % nx)
                else:
                    self.e('  t = %s; goto L_dispatch;' % hx(nx | self.T))
            if a in self.labels:
                self.e('L_%08X:' % a)
                self.lazy = None
                self.kconst = {}
            self.e('  /* %08X: %s */' % (a, self.dis(i)))
            body = self.emit_ins(i)
            for b in body:
                self.e('  ' + b)
            self.update_lazy(i, body)
            self.kconst_update(i)
            prev_falls = self.falls_through(i)
            prev_addr = a
        if prev_addr is not None and prev_falls:
            nxt = self.ins[prev_addr].next
            self.e('  t = %s; goto L_dispatch;' % hx(nxt | self.T))
        # dispatch block
        self.e('L_dispatch:')
        self.e('  if (((t ^ ret) >> 1) == 0) goto L_exit;')
        cases = sorted(self.resume)
        if cases:
            self.e('  switch (t) {')
            for ca in cases:
                self.e('  case %s: goto L_%08X;' % (hx(ca | self.T), ca))
            self.e('  default: break;')
            self.e('  }')
        self.e('  ' + self.sync_out())
        self.e('  if (!gba_jump(t)) { cpu.next_pc = t; return; }')
        self.e('L_resync:')
        self.e('  t = cpu.next_pc;')
        self.e('  if (((t ^ ret) >> 1) == 0) return;')
        self.e('  ' + self.sync_in(ALLREGS | FLAGS))
        self.e('  goto L_dispatch;')
        self.e('L_exit:')
        self.e('  ' + self.sync_out((self.MOD | self.synced) & (ALLREGS | FLAGS)))
        self.e('  cpu.next_pc = ret;')
        self.e('}')
        # labels referenced but never emitted would break compilation; the
        # entry label always exists because it is in labels
        text = '\n'.join(self.lines)
        return text

    def falls_through(self, i):
        k = self.kind[i.addr]
        if i.cond != COND_AL:
            return True
        if k in ('branch', 'tail', 'ctail', 'indirect'):
            return False
        return True

    def update_lazy(self, i, body=()):
        k = self.kind[i.addr]
        if k != 'normal':
            self.lazy = None
            return
        if self.lazy is not None and not getattr(i, 's', False):
            # the temporaries a fused condition reads ('sub': fa, fb, fr;
            # 'logic': fr) were reused as scratch, e.g. by the address
            # temporary of an LDM between 'cmp' and 'bne': later conditions
            # must read the flags
            pat = LAZY_SUB_WRITE if self.lazy == 'sub' else LAZY_FR_WRITE
            if any(pat.search(b) for b in body):
                self.lazy = None
                return
        if i.op == 'dp' and i.s:
            if i.cond != COND_AL:
                self.lazy = None
            elif i.alu in ('sub', 'rsb', 'cmp'):
                self.lazy = 'sub'
            elif i.alu in LOGICAL:
                self.lazy = 'logic'
            else:
                self.lazy = None
        elif i.op in ('mul', 'mull') and i.s:
            self.lazy = None

    def dis(self, i):
        from armdecode import fmt
        return fmt(i).replace('*/', '* /')


def game_toml(argv):
    """--config PATH, or --game NAME for games/NAME/game.toml (default log2)."""
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(repo, 'games', 'log2', 'game.toml')
    for k, a in enumerate(argv):
        if a == '--game' and k + 1 < len(argv):
            path = os.path.join(repo, 'games', argv[k + 1], 'game.toml')
        elif a == '--config' and k + 1 < len(argv):
            path = argv[k + 1]
    return path


def main():
    """recomp.py ROM functions.json OUTDIR [SHARDS] [--game NAME | --config TOML] [--dump-abi] [--trace]"""
    args, k = [], 1
    while k < len(sys.argv):
        if sys.argv[k] in ('--game', '--config'):
            k += 2
            continue
        if not sys.argv[k].startswith('--'):
            args.append(sys.argv[k])
        k += 1
    rom_path, fjson, outdir = args[:3]
    shards = int(args[3]) if len(args) > 3 else 16
    import tomllib
    cfg = tomllib.load(open(game_toml(sys.argv), 'rb'))
    data = json.load(open(fjson))
    rom = Rom(rom_path)
    cs = CodeSpace(rom)
    for dst, size, src in data['overlays']:
        cs.add_overlay(dst, size, src)
    funcs = data['functions']
    keys = {(f['addr'], f['thumb']) for f in funcs}
    os.makedirs(outdir, exist_ok=True)
    gens = {}
    for f in funcs:
        g = FuncGen(cs, f, keys, {})
        g.build()
        gens[(f['addr'], f['thumb'])] = g
    # Address-taken functions are the possible targets of indirect calls.  The
    # IRQ code is only entered by the runtime (with a full context save), so it
    # does not constrain the indirect-call summary (game.toml [recomp]).
    # Entries seeded as computed-jump targets inside a function (toml
    # [[extra_func]] resume = true) are not called indirectly either, and
    # neither is code only gap fill found: no BL, pointer, vtable or code
    # constant anywhere refers to it (dead code, or data that decodes as code
    # and falls into a real function).
    runtime_only = set(cfg.get('recomp', {}).get('runtime_only', []))
    not_taken = ('call', 'tail', 'reset', 'toml-resume', 'gap-fill', 'gap-fill-arm', None)
    taken = set()
    for f in funcs:
        if f.get('origin') not in not_taken and f['addr'] not in runtime_only:
            taken.add((f['addr'], f['thumb']))
    noreturn = {(e['addr'], e.get('mode', 'thumb') == 'thumb') for e in cfg.get('noreturn', [])}
    for e in cfg.get('override_load', []):
        OVERRIDE_LOAD[e['pc']] = e['func']
    for e in cfg.get('hook_before', []):
        HOOK_BEFORE[e['pc']] = e['func']
    for e in cfg.get('native', []):
        NATIVE[(e['addr'], e.get('mode', 'thumb') == 'thumb')] = e['func']
    unreachable = {(e['addr'], e.get('mode', 'thumb') == 'thumb') for e in cfg.get('assume_unreachable', [])}
    noreturn |= unreachable
    taken -= noreturn
    summ = Summaries(gens, taken, noreturn)
    iters = summ.run()
    print('indirect-call summary: IN=%05x MOD=%05x (widened in %d rounds)'
          % (summ.ind_in, summ.ind_mod, summ.widened))
    for k, kin, kmod in getattr(summ, 'nonconforming', [])[:20]:
        print('  non-conforming address-taken function %s IN=%05x MOD=%05x'
              % (fname(*k), kin, kmod))
    nabi = sum(1 for k in gens if summ.MOD[k] != (ALLREGS | FLAGS))
    print('ABI summaries converged after %d passes; %d/%d functions with precise summaries'
          % (iters, nabi, len(gens)))
    if '--dump-abi' in sys.argv:
        with open(os.path.join(outdir, 'abi.txt'), 'w') as fh:
            for k in sorted(gens):
                fh.write('%s IN=%05x MOD=%05x saved=%03x\n' % (fname(*k), summ.IN[k], summ.MOD[k], summ.saved[k]))
    bodies = []
    for f in funcs:
        g = gens[(f['addr'], f['thumb'])]
        g.set_summaries(summ)
        g.warn_entry = (f['addr'], f['thumb']) in unreachable
        key = (f['addr'], f['thumb'])
        if key in NATIVE:
            # replaced by native C: same register contract (callers use the
            # summary computed from the original code); it returns to the
            # entry lr even if it leaves another value in r14, as the
            # original's pop {pc} would
            bodies.append((f, 'void %s(void) {\n  const uint32_t ret = cpu.r[14];\n  %s();\n'
                              '  cpu.next_pc = ret;\n}' % (fname(*key), NATIVE[key])))
            continue
        bodies.append((f, g.generate()))
    # header with prototypes
    with open(os.path.join(outdir, 'gen_funcs.h'), 'w') as h:
        h.write('#ifndef GEN_FUNCS_H\n#define GEN_FUNCS_H\n#include "recomp.h"\n')
        h.write('int gba_jump(uint32_t target);\n')
        for fn in sorted(set(OVERRIDE_LOAD.values())):
            h.write('uint32_t %s(uint32_t v);\n' % fn)
        for fn in sorted(set(HOOK_BEFORE.values())):
            h.write('void %s(const uint32_t *regs);\n' % fn)
        for fn in sorted(set(NATIVE.values())):
            h.write('void %s(void);\n' % fn)
        for f, _ in bodies:
            h.write('void %s(void);\n' % fname(f['addr'], f['thumb']))
        h.write('#endif\n')
    per = (len(bodies) + shards - 1) // shards
    for s in range(shards):
        chunk = bodies[s * per:(s + 1) * per]
        with open(os.path.join(outdir, 'gen_%02d.c' % s), 'w') as fh:
            fh.write('/* generated by tools/recomp.py - do not edit */\n#include "gen_funcs.h"\n\n')
            for f, text in chunk:
                fh.write(text + '\n\n')
    with open(os.path.join(outdir, 'gen_table.c'), 'w') as fh:
        fh.write('#include "gen_funcs.h"\n#include "dispatch.h"\n')
        fh.write('const gba_func_entry_t gba_func_table[] = {\n')
        for f, _ in sorted(bodies, key=lambda x: (x[0]['addr'], x[0]['thumb'])):
            fh.write('  { %s, %s },\n' % (hx(f['addr'] | (1 if f['thumb'] else 0)), fname(f['addr'], f['thumb'])))
        fh.write('};\nconst unsigned gba_func_count = %d;\n' % len(bodies))
        # what each function may change (its ABI summary): the interpreter
        # and the top-level loop put back the rest after recompiled code
        # returns (runtime/dispatch.h)
        fh.write('const uint32_t gba_func_mod[] = {\n')
        for f, _ in sorted(bodies, key=lambda x: (x[0]['addr'], x[0]['thumb'])):
            fh.write('  0x%05X,\n' % (summ.MOD[(f['addr'], f['thumb'])] & (ALLREGS | FLAGS)))
        fh.write('};\n')
    # ROM cache boot preload list (game.toml [rom] preload, 16KB pages)
    pre = cfg.get('rom', {}).get('preload', [])
    with open(os.path.join(outdir, 'gen_rom_preload.c'), 'w') as fh:
        fh.write('/* generated by tools/recomp.py from game.toml [rom] preload */\n#include <stdint.h>\n')
        fh.write('const uint16_t gba_rom_preload[] = { %s };\n' % (', '.join('%d' % p for p in pre) or '0'))
        fh.write('const int gba_rom_preload_n = %d;\n' % len(pre))
    print('generated %d functions in %d shards' % (len(bodies), shards))


if __name__ == '__main__':
    main()

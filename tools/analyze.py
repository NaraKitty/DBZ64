"""Code discovery for the Webfoot DBZ GBA games (games/<game>/game.toml).

Features:
  * game.toml seed loader (extra_func / jump_table / code_copy)
  * per-function literal-pool / ADR tracking (byte classification)
  * Thumb PIC constant tracking (ldr rX,=k ; add rX,pc)
  * relative C++ vtables (target = vptr + [vptr+4k]); bases = vptr literals
    (absolute words into [layout] rodata), contiguous slots up to
    the next base / first non-code entry
  * Thumb switch tables: cmp #N; bcs; adr; ldrb|ldrh; lsls #1; add pc,rT
  * absolute pointer harvesting (whole ROM) with trial-walk validation
  * ARM this-adjust thunk pattern scan (ldr ip,[pc,#8]; sub r0,r0,#n;
    add ip,pc,ip; bx ip) in rodata
  * linear AOT gap filling of the executable text + IWRAM image (ARM first)
  * fixpoint passes: re-run from scratch with all vtable bases known and
    with speculative entries that conflict (overlap literals / start inside
    instructions / coincidental pointers into walked bodies) blacklisted
  * 'b' to a push{..,lr} is a tail call (shared-body functions)
  * classification of every indirect control transfer
  * byte coverage statistics (code / literal / table / padding / data)
  * toml [[extra_func]] seeds that are BL return addresses (reference-runtime
    resume points) are listed but not used as entries unless WITH_RET_SITES=1

Usage: python tools/analyze.py ROM games/GAME/game.toml OUT_DIR [extra_entries.txt]
Output: OUT_DIR/functions.json (analyze.py schema + origin/indirect/lits keys,
        top-level callvia/vtables/summary), summary.json, coverage.txt,
        indirect.txt, jumptables.txt, vtables.txt, features.txt,
        arm_and_veneers.txt, overlaps.txt, functions.txt, rejected.txt
"""
import json
import os
import struct
import sys
import tomllib
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from armdecode import Rom, CodeSpace, fmt  # noqa: E402

IWRAM_LO, IWRAM_HI = 0x03000000, 0x03008000
EXIT_OPS = ('undef', 'blprefix')


def written_regs(ins):
    op = ins.op
    if op == 'dp':
        if ins.alu in ('tst', 'teq', 'cmp', 'cmn'):
            return ()
        return (ins.rd,)
    if op == 'mem':
        r = []
        if ins.load:
            r.append(ins.rd)
        if ins.wb:
            r.append(ins.rn)
        return r
    if op == 'ldm':
        r = list(ins.regs) if ins.load else []
        if ins.wb:
            r.append(ins.rn)
        return r
    if op == 'mul':
        return (ins.rd,)
    if op == 'mull':
        return (ins.rd, ins.rdhi)
    if op in ('mrs', 'swp'):
        return (ins.rd,)
    if op in ('bl', 'blh'):
        return (14,)
    return ()


# --------------------------------------------------------------- seeds
def load_toml(path):
    """Parse the reference game.toml into plain seed records.

    Returns dict with:
      funcs:  [(addr, thumb, source_addr|None, resume, note)]
      tables: [(addr, stride, count, fmt, entries_mode, name)]
      copies: [(runtime_start, source_start, size, name)]
      false:  [(addr, thumb, note)] ([[false_entry]]: never a function entry)
      program: the [program] table
    """
    t = tomllib.load(open(path, 'rb'))
    funcs = []
    for e in t.get('extra_func', []):
        funcs.append((e['addr'], e.get('mode', 'thumb') == 'thumb',
                      e.get('source_addr'), bool(e.get('resume', False)),
                      e.get('note', '')))
    tables = []
    for e in t.get('jump_table', []):
        tables.append((e['addr'], e.get('stride', 4), e['count'], e.get('format', 'abs32'),
                       e.get('entries_mode', 'auto'), e.get('name', '')))
    copies = []
    for e in t.get('code_copy', []):
        copies.append((e['runtime_start'], e['source_start'], e['size'], e.get('name', '')))
    false = [(e['addr'], e.get('mode', 'thumb') == 'thumb', e.get('note', ''))
             for e in t.get('false_entry', [])]
    return {'funcs': funcs, 'tables': tables, 'copies': copies, 'false': false,
            'program': t.get('program', {})}


def expand_table(cs, addr, stride, count, fmt_, mode):
    """Expand a [[jump_table]] into [(slot_addr, target, thumb)].

    pcrel_thumb: target = slot_addr + u32(slot_addr).  Verified against the
    constructor runner at 0x08021D74 (ldr r0,[r4]; adds r0,r0,r4; bl
    _call_via_r0) and destructor runner 0x08021D48 (same, walking down).
    abs32: target = u32(slot_addr).
    """
    out = []
    for k in range(count):
        a = addr + k * stride
        v = cs.u32(a)
        if fmt_ == 'pcrel_thumb':
            raw = (a + v) & 0xffffffff
        else:
            raw = v
        if mode == 'thumb':
            th = True
        elif mode == 'arm':
            th = False
        else:
            th = bool(raw & 1)
        out.append((a, raw & ~1, th))
    return out


# --------------------------------------------------------------- function
class Func:
    __slots__ = ('addr', 'thumb', 'insns', 'labels', 'calls', 'tails', 'jumptables',
                 'const_branch', 'bad', 'lits', 'adrs', 'consts_seen', 'indirect',
                 'vcall_slots', 'blh', 'veneers', 'callvia', 'spec_reject', 'jt_data',
                 'const_stores', 'swis')

    def __init__(self, addr, thumb):
        self.addr = addr
        self.thumb = thumb
        self.insns = {}          # addr -> Insn
        self.labels = {addr}
        self.calls = set()       # (target, thumb)
        self.tails = set()       # (target, thumb)
        self.jumptables = {}     # insn addr -> [targets]
        self.const_branch = {}   # insn addr -> (target, thumb, is_call)
        self.bad = []            # (addr, reason)
        self.lits = {}           # literal addr -> size
        self.adrs = set()        # addresses formed by ADR / PIC add
        self.consts_seen = set() # every constant materialised in a register
        self.indirect = []       # (addr, kind, detail)
        self.vcall_slots = set()
        self.blh = []
        self.veneers = []        # (addr, target, thumb)
        self.callvia = []        # (addr, reg, resolved target or None)
        self.spec_reject = None
        self.jt_data = {}        # insn addr -> (table addr, byte size) inline switch tables
        self.const_stores = []   # (insn addr, target addr, nbytes) stores through known base
        self.swis = []           # (insn addr, num, r0, r1, r2) with known-constant args


class Analyzer:
    def __init__(self, rom_path, toml=None):
        self.rom = Rom(rom_path)
        self.cs = CodeSpace(self.rom)
        self.toml = toml
        self.funcs = {}
        self.entries = {}        # (addr, thumb) -> origin string
        self.worklist = []
        self.dcache = {}
        self.log = []
        self.callvia_regs = {}   # helper addr -> reg  (Thumb 'bx rN' stubs)
        self.data_ranges = []    # (lo, hi, name) never code
        self.scan_lo = 0x080000C0
        self.scan_hi = 0x08030000
        self.rejected = {}       # (addr, thumb) -> reason

    # ---------------------------------------------------- helpers
    def dec(self, a, thumb):
        k = (a, thumb)
        i = self.dcache.get(k)
        if i is None:
            i = self.cs.decode(a, thumb)
            self.dcache[k] = i
        return i

    def in_data(self, a):
        for lo, hi, _ in self.data_ranges:
            if lo <= a < hi:
                return True
        return False

    def is_code_addr(self, a):
        """Plausible destination of a call/branch."""
        if self.scan_lo <= a < self.scan_hi:
            return not self.in_data(a)
        for dst, size, _src in self.cs.overlays:
            if dst <= a < dst + size:
                return True
        return False

    def add_entry(self, addr, thumb, origin='?'):
        addr &= ~1 if thumb else ~3
        key = (addr, thumb)
        if key in self.entries:
            return False
        if not self.cs.valid(addr, 2 if thumb else 4):
            return False
        self.entries[key] = origin
        self.worklist.append(key)
        return True

    # ---------------------------------------------------- per-function walk
    def walk(self, addr, thumb, spec=False):
        f = Func(addr, thumb)
        isz = 2 if thumb else 4
        todo = [addr]
        seen = set()
        while todo:
            a = todo.pop()
            consts = {}
            lr_link = None
            lr_age = 0
            prev = None
            while True:
                if a in seen:
                    break
                if not self.cs.valid(a, isz):
                    f.bad.append((a, 'unmapped'))
                    break
                if spec and self.in_data(a):
                    f.bad.append((a, 'data-range'))
                    break
                seen.add(a)
                ins = self.dec(a, thumb)
                f.insns[a] = ins
                nxt = ins.next
                op = ins.op
                if op in EXIT_OPS:
                    f.bad.append((a, op))
                    break
                uncond = ins.cond == 14
                # ---- literal / adr bookkeeping
                if op == 'mem' and ins.rn == 15 and ins.off[0] == 'imm':
                    la = ins.pcval + (ins.off[1] if ins.up else -ins.off[1])
                    if ins.load:
                        f.lits[la] = max(f.lits.get(la, 0), ins.msize)
                    else:
                        f.bad.append((a, 'str-to-pc-rel'))
                if op == 'mem' and not ins.load and ins.rn in consts and ins.off[0] == 'imm':
                    ta = (consts[ins.rn] + ((ins.off[1] if ins.up else -ins.off[1]) if ins.pre else 0)) & 0xffffffff
                    f.const_stores.append((a, ta, ins.msize))
                elif op == 'ldm' and not ins.load and ins.rn in consts:
                    f.const_stores.append((a, consts[ins.rn], 4 * len(ins.regs)))
                elif op == 'swp' and ins.rn in consts:
                    f.const_stores.append((a, consts[ins.rn], ins.msize))
                if op == 'swi':
                    f.swis.append((a, ins.num, consts.get(0), consts.get(1), consts.get(2)))
                    for r in (0, 1, 2, 3):
                        consts.pop(r, None)
                self._track_consts(ins, consts, f)
                if op == 'bl':
                    t = ins.target
                    if self.cs.valid(t):
                        f.calls.add((t, thumb))
                        reg = self.callvia_regs.get(t) if thumb else None
                        if reg is not None:
                            res = consts.get(reg)
                            f.callvia.append((a, reg, res))
                            if res is not None:
                                f.const_branch[a] = (res & ~1, bool(res & 1), True)
                                f.calls.add((res & ~1, bool(res & 1)))
                    else:
                        f.bad.append((a, 'bl-unmapped %08x' % t))
                    for r in (0, 1, 2, 3, 12, 14):
                        consts.pop(r, None)
                    prev = ins
                    a = nxt
                    continue
                if op == 'blh':
                    # lone BL suffix: pc = lr + off; lr = next|1
                    f.blh.append(a)
                    src = 'lr=%s' % ('%08x' % consts[14] if 14 in consts else '?')
                    f.indirect.append((a, 'blh_call', src))
                    for r in (0, 1, 2, 3, 12, 14):
                        consts.pop(r, None)
                    a = nxt
                    continue
                if op == 'b':
                    t = ins.target
                    if not self.cs.valid(t):
                        f.bad.append((a, 'b-unmapped %08x' % t))
                        break
                    if (t, thumb) in self.entries and t != addr:
                        f.tails.add((t, thumb))
                    elif thumb and t != addr and (self.cs.u16(t) & 0xff00) == 0xb500:
                        # a branch never targets push {..,lr} inside its own
                        # function: this is a tail call into another function
                        f.tails.add((t, thumb))
                    else:
                        f.labels.add(t)
                        todo.append(t)
                    if uncond:
                        break
                    f.labels.add(nxt)
                    prev = ins
                    a = nxt
                    continue
                if op == 'bx':
                    rm = ins.rm
                    if rm == 15:
                        t = (ins.pcval & ~3) if thumb else ins.pcval
                        f.const_branch[a] = (t, False, False)
                        f.tails.add((t, False))
                        f.veneers.append((a, t, False))
                        if uncond:
                            break
                        a = nxt
                        continue
                    if rm in consts:
                        t = consts[rm]
                        tt = bool(t & 1)
                        is_call = lr_link == nxt
                        f.const_branch[a] = (t & ~1, tt, is_call)
                        (f.calls if is_call else f.tails).add((t & ~1, tt))
                        if not is_call:
                            f.veneers.append((a, t & ~1, tt))
                        if is_call or not uncond:
                            f.labels.add(nxt)
                            a = nxt
                            continue
                        break
                    kind = self._classify_bx(f, ins, prev, lr_link)
                    det = 'bx r%d' % rm
                    if lr_link is not None and lr_link != nxt and kind != 'return':
                        kind = 'computed_call'
                        det += ' lr=%08x' % lr_link
                        f.calls.add((lr_link & ~1, bool(lr_link & 1)))
                    f.indirect.append((a, kind, det))
                    if lr_link == nxt or not uncond:
                        f.labels.add(nxt)
                        a = nxt
                        continue
                    break
                if ins.writes_pc():
                    kind = None
                    jt = self._jumptable(f, ins, consts)
                    if jt is not None:
                        f.jumptables[a] = jt
                        for t in jt:
                            f.labels.add(t)
                            todo.append(t)
                        f.indirect.append((a, 'jumptable', '%d entries' % len(jt)))
                        if uncond:
                            break
                        a = nxt
                        continue
                    if op == 'mem' and ins.rn == 15 and ins.off[0] == 'imm' and ins.pre:
                        la = ins.pcval + (ins.off[1] if ins.up else -ins.off[1])
                        if self.cs.valid(la, 4):
                            t = self.cs.u32(la)
                            tt = thumb   # ARMv4: ldr pc does not interwork
                            is_call = lr_link == nxt
                            f.const_branch[a] = (t & ~(1 if tt else 3), tt, is_call)
                            (f.calls if is_call else f.tails).add((t & ~(1 if tt else 3), tt))
                            f.veneers.append((a, t, tt))
                            if is_call or not uncond:
                                f.labels.add(nxt)
                                a = nxt
                                continue
                            break
                    if op == 'dp' and ins.alu == 'mov' and ins.op2[0] == 'reg' and ins.op2[1] in consts \
                            and ins.op2[3] == 0 and ins.op2[2] == 'lsl':
                        t = consts[ins.op2[1]]
                        is_call = lr_link == nxt
                        f.const_branch[a] = (t & ~1, thumb, is_call)
                        (f.calls if is_call else f.tails).add((t & ~1, thumb))
                        if is_call or not uncond:
                            f.labels.add(nxt)
                            a = nxt
                            continue
                        break
                    kind = self._classify_pcwrite(f, ins, lr_link)
                    f.indirect.append((a, kind, fmt(ins)))
                    if lr_link == nxt or not uncond:
                        f.labels.add(nxt)
                        a = nxt
                        continue
                    break
                if op == 'dp' and ins.rd == 14 and ins.alu in ('mov', 'add') and \
                        (ins.rn == 15 or (ins.op2[0] == 'reg' and ins.op2[1] == 15)):
                    if ins.alu == 'mov':
                        lr_link = ins.pcval
                    elif ins.op2[0] == 'imm':
                        lr_link = ins.pcval + ins.op2[1]
                    lr_age = 0
                else:
                    if lr_link is not None:
                        lr_age += 1
                        if lr_age > 4 or 14 in written_regs(ins):
                            lr_link = None
                prev = ins
                a = nxt
        return f

    def _classify_bx(self, f, ins, prev, lr_link):
        rm = ins.rm
        if rm == 14:
            return 'return'
        if lr_link == ins.next:
            return 'computed_call'
        if ins.addr == f.addr and f.thumb:
            return 'call_via_r%d' % rm
        # pop {rX} ; bx rX  (interworking return)  / ldmfd sp!,{..rX}
        if prev is not None and prev.op == 'ldm' and prev.load and prev.rn == 13 and rm in prev.regs:
            return 'return'
        # pop {r3}; ... look back a few insns
        p = ins.addr
        for _ in range(4):
            p -= ins.size
            q = f.insns.get(p)
            if q is None:
                break
            if q.op == 'ldm' and q.load and q.rn == 13 and rm in q.regs:
                return 'return'
            if q.op == 'dp' and q.rd == rm and q.op2 and q.op2[0] == 'reg' and q.op2[1] == 14:
                return 'return'  # mov rX, lr ... bx rX
            if rm in written_regs(q):
                break
        return 'computed_jump'

    def _classify_pcwrite(self, f, ins, lr_link):
        op = ins.op
        if op == 'ldm':
            if ins.user:
                return 'exception_return'
            if ins.rn == 13:
                return 'return'
            return 'computed_jump'
        if op == 'dp':
            if ins.op2[0] == 'reg' and ins.op2[1] == 14 and ins.alu == 'mov':
                return 'exception_return' if ins.s else 'return'
            if ins.op2[0] == 'reg' and ins.op2[1] == 12 and ins.alu == 'mov' and not f.thumb:
                return 'return_via_ip'
            if ins.alu == 'sub' and ins.rn == 14 and ins.s:
                return 'exception_return'
            if lr_link == ins.next:
                return 'computed_call'
            return 'computed_jump'
        if op == 'mem':
            if ins.rn == 13:
                return 'return'
            if lr_link == ins.next:
                return 'computed_call'
            return 'computed_jump'
        return 'computed_jump'

    def _track_consts(self, ins, consts, f):
        op = ins.op
        if op == 'mem' and ins.load and ins.rn == 15 and ins.off[0] == 'imm' and ins.msize == 4 and ins.pre:
            la = ins.pcval + (ins.off[1] if ins.up else -ins.off[1])
            if self.cs.valid(la, 4):
                v = self.cs.u32(la)
                consts[ins.rd] = v
                f.consts_seen.add(v)
                return
        if op == 'mem' and ins.load and ins.msize == 4 and ins.rn in consts and ins.off[0] == 'imm' \
                and ins.pre and not ins.wb:
            base = consts[ins.rn]
            la = (base + (ins.off[1] if ins.up else -ins.off[1])) & 0xffffffff
            # loads from ROM constant tables (e.g. vptr+slot)
            consts.pop(ins.rd, None)
            if 0x08000000 <= la < 0x08000000 + len(self.rom.data) - 3:
                pass
            return
        if op == 'dp':
            o = ins.op2
            alu = ins.alu
            val = None
            if alu == 'mov' and o[0] == 'imm':
                val = o[1]
            elif alu == 'mvn' and o[0] == 'imm':
                val = (~o[1]) & 0xffffffff
            elif alu == 'mov' and o[0] == 'reg' and o[1] in consts and o[2] in ('lsl', 'lsr') and o[3] < 32:
                v = consts[o[1]]
                val = ((v << o[3]) if o[2] == 'lsl' else (v >> o[3])) & 0xffffffff
            elif alu == 'mov' and o[0] == 'reg' and o[1] == 15 and o[3] == 0 and o[2] == 'lsl':
                val = ins.pcval
            elif alu in ('add', 'sub', 'orr', 'bic', 'and', 'eor'):
                if ins.rn == 15:
                    a = ins.pcval
                elif ins.rn in consts:
                    a = consts[ins.rn]
                else:
                    a = None
                if o[0] == 'imm':
                    b = o[1]
                elif o[0] == 'reg' and o[3] == 0 and o[2] == 'lsl':
                    b = ins.pcval if o[1] == 15 else consts.get(o[1])
                else:
                    b = None
                if a is not None and b is not None:
                    val = {'add': a + b, 'sub': a - b, 'orr': a | b, 'bic': a & ~b,
                           'and': a & b, 'eor': a ^ b}[alu] & 0xffffffff
                    if alu == 'add' and (ins.rn == 15 or (o[0] == 'reg' and o[1] == 15)):
                        f.adrs.add(val)
            if val is not None:
                consts[ins.rd] = val
                f.consts_seen.add(val)
                return
        for r in written_regs(ins):
            consts.pop(r, None)

    def _jumptable(self, f, ins, consts):
        """ARM-style switch tables (only kinds seen on GBA GCC/ARM code)."""
        thumb = f.thumb
        a = ins.addr
        prev = []
        p = a
        for _ in range(12):
            p -= 2 if thumb else 4
            if p not in f.insns:
                break
            prev.append(f.insns[p])
        bound = None
        for q in prev:
            if q.op == 'dp' and q.alu == 'cmp' and q.op2[0] == 'imm':
                bound = q.op2[1] + 1
                br = f.insns.get(q.next)
                if br is not None and br.op == 'b' and br.cond == 2:   # cmp #N ; bcs default
                    bound = q.op2[1]
                break
        if thumb and ins.op == 'dp' and ins.alu == 'add' and ins.rd == 15 and \
                ins.op2[0] == 'reg' and bound:
            # agbcc/GCC byte/halfword switch:
            #   cmp rI,#N ; bcs def ; adr rT,tbl ; [adds rT,rI] ; ldrb/ldrh rT,[rT,rI] ;
            #   lsls rT,#1 ; add pc,rT        target = addr+4 + 2*tbl[i]
            table = esz = None
            for q in prev:
                if q.op == 'mem' and q.load and q.rn != 15 and q.msize in (1, 2) and esz is None:
                    esz = q.msize
                if q.op == 'dp' and q.alu == 'add' and q.rn == 15 and q.op2[0] == 'imm':
                    table = (q.pcval + q.op2[1]) & 0xffffffff
                    break
            if table is None or esz is None or bound > 1024:
                return None
            out = []
            for k in range(bound):
                v = self.cs.u16(table + 2 * k) if esz == 2 else self.cs.rom.data[
                    self.cs.phys(table + k) - 0x08000000]
                out.append((ins.pcval + 2 * v) & 0xffffffff)
            f.jt_data[a] = (table, bound * esz)
            return out
        if not thumb:
            if ins.op == 'mem' and ins.rn == 15 and ins.off[0] == 'reg' and bound:
                base = ins.pcval
                return [self.cs.u32(base + 4 * k) & ~1 for k in range(bound)
                        if self.cs.valid(base + 4 * k, 4)]
            if ins.op == 'dp' and ins.alu == 'add' and ins.rn == 15 and ins.op2[0] == 'reg' and bound:
                return [ins.pcval + 4 * k for k in range(bound)]
            return None
        # Thumb GCC: lsl rX,#2 ; ldr rY,=table ; add rX,rY ; ldr rX,[rX] ; mov pc,rX
        if ins.op == 'dp' and ins.alu == 'mov' and bound:
            table = None
            for q in prev:
                if q.op == 'mem' and q.rn == 15 and q.load and q.msize == 4:
                    table = self.cs.u32(q.pcval + q.off[1])
                    break
            if table is None or bound > 1024:
                return None
            out = []
            for k in range(bound):
                if not self.cs.valid(table + 4 * k, 4):
                    return None
                out.append(self.cs.u32(table + 4 * k) & ~1)
            return out
        return None

    # ---------------------------------------------------- validation
    def plausible(self, f, strict=True):
        """Decide whether a speculative walk looks like real code."""
        if f.bad:
            return 'bad:%s@%08x' % (f.bad[0][1], f.bad[0][0])
        if f.blh:
            return 'lone-bl-suffix'
        for (t, th) in f.calls | f.tails:
            if not self.is_code_addr(t):
                return 'call-out-of-code %08x' % t
        for a, ins in f.insns.items():
            # instruction overlaps a known literal or the middle of known code
            for b in range(a, a + ins.size, 2):
                if b in self.lit_bytes:
                    return 'overlaps-literal %08x' % b
            if a in self.mid_bytes:
                return 'starts-inside-insn %08x' % a
            if a not in self.code_bytes:
                for b in range(a + 2, a + ins.size, 2):
                    if b in self.code_bytes:
                        return 'straddles-insn %08x' % b
        if strict:
            if self.looks_like_string(f.addr):
                return 'ascii-string'
            first = f.insns.get(f.addr)
            if first is not None and first.thumb and first.raw == 0:
                return 'starts-with-zero'
            # every literal it loads must be inside ROM and 4-aligned
            for la, sz in f.lits.items():
                if not self.cs.valid(la, sz):
                    return 'lit-unmapped'
            # must contain at least one terminator
            if not any(k in ('return', 'exception_return') for _, k, _ in f.indirect) and \
                    not f.tails and not any(ins.op == 'bx' or ins.writes_pc() for ins in f.insns.values()):
                return 'no-terminator'
        return None

    def looks_like_string(self, a, minlen=6):
        p = self.cs.phys(a)
        if p is None:
            return False
        d = self.rom.data
        o = p - 0x08000000
        n = 0
        while o + n < len(d) and n < 256:
            c = d[o + n]
            if c == 0:
                return n >= minlen
            if not (0x20 <= c < 0x7f or c in (9, 10, 13)):
                return False
            n += 1
        return False

    # ---------------------------------------------------- driver
    def commit(self, f):
        key = (f.addr, f.thumb)
        self.funcs[key] = f
        for (t, th) in f.calls:
            self.add_entry(t, th, 'call')
        for (t, th) in f.tails:
            self.add_entry(t, th, 'tail')
        for la, sz in f.lits.items():
            for b in range(la, la + max(sz, 2), 2):
                self.lit_bytes.add(b)
        for ta, tsz in f.jt_data.values():
            for b in range(ta & ~1, ta + tsz, 2):
                self.lit_bytes.add(b)
        for a, ins in f.insns.items():
            for b in range(a + 2, a + ins.size, 2):
                self.mid_bytes.add(b)
            self.code_bytes.add(a)

    def run(self):
        while self.worklist:
            key = self.worklist.pop()
            if key in self.funcs:
                continue
            f = self.walk(*key)
            self.commit(f)

    def try_seed(self, addr, thumb, origin, strict=True):
        addr &= ~1 if thumb else ~3
        key = (addr, thumb)
        if key in self.entries:
            return 'known'
        if not self.cs.valid(addr, 2 if thumb else 4):
            return 'unmapped'
        f = self.walk(addr, thumb, spec=True)
        why = self.plausible(f, strict)
        if why:
            self.rejected[key] = (origin, why)
            return why
        self.entries[key] = origin
        self.commit(f)
        self.run()
        return None

    def setup(self):
        self.lit_bytes = set()
        self.mid_bytes = set()
        self.code_bytes = set()




# ===================================================================== driver
# Section layout of the cartridge image, from the game toml's [layout] table
# (games/<game>/game.toml; verified by hand from crt0, the .ctors/.dtors
# runners and the veneer block):
#   text        .text [lo, hi) (crt0 ends at text[0])
#   ctors/dtors constructor / destructor tables (never code)
#   veneers     Thumb/ARM interworking veneers
#   rodata      relative vtables + ARM this-adjust thunks (never gap-filled)
#   text2       start of the second text block (it ends at the last code)
#   iwram_code  ARM code + pools of the crt0 IWRAM image
#   iwram_data  the image's .data (reported only)
LAYOUT_KEYS = ('text', 'ctors', 'dtors', 'veneers', 'rodata', 'text2', 'iwram_code', 'iwram_data')
LAYOUT = {}


def load_layout(path):
    t = tomllib.load(open(path, 'rb')).get('layout', {})
    missing = [k for k in LAYOUT_KEYS if k not in t]
    if missing:
        sys.exit('%s: [layout] needs %s' % (path, ', '.join(missing)))
    for k, v in t.items():
        LAYOUT[k] = tuple(v) if isinstance(v, list) else v
    global STATIC_DATA_RANGES, RODATA, IWRAM_CODE, TEXT2
    STATIC_DATA_RANGES = [
        (0x08000004, 0x080000C0, 'cartridge header'),
        (LAYOUT['ctors'][0], LAYOUT['ctors'][1], '.ctors (pcrel_thumb)'),
        (LAYOUT['dtors'][0], LAYOUT['dtors'][1], '.dtors (pcrel_thumb)'),
    ]
    RODATA = (LAYOUT['rodata'][0], LAYOUT['rodata'][1], 'rodata: relative vtables + ARM this-adjust thunks')
    IWRAM_CODE = LAYOUT['iwram_code']
    TEXT2 = LAYOUT['text2']


STATIC_DATA_RANGES = RODATA = IWRAM_CODE = TEXT2 = None
# ldr ip,[pc,#8]; sub r0,r0,#n (or sub r1,r1,#n); add ip,pc,ip; bx ip; .word rel
THUNK_W0, THUNK_W2, THUNK_W3 = 0xE59FC008, 0xE08FC00C, 0xE12FFF1C



SPEC_ORIGINS = ('vtable', 'rom-ptr', 'gap-fill', 'gap-fill-arm', 'code-const', 'iwram-const')
ORIGIN_RANK = {'reset': 0, 'toml': 0, 'toml-resume': 0, 'toml-table': 0, 'extra': 0,
               'call': 1, 'tail': 1, 'thunk-pattern': 1,
               'vtable': 2, 'code-const': 2, 'iwram-const': 2, 'rom-ptr': 3,
               'gap-fill': 4, 'gap-fill-arm': 4}


class Driver:
    def __init__(self, rom_path, toml_path, extra=None, verbose=False, use_toml_funcs=True,
                 use_toml_tables=True, include_ret_sites=False):
        self.rom_path = rom_path
        load_layout(toml_path)
        self.T = load_toml(toml_path)
        self.include_ret_sites = include_ret_sites
        self.ret_sites = []          # toml seeds that are BL return addresses
        if not use_toml_funcs:
            self.T['funcs'] = []
        if not use_toml_tables:
            self.T['tables'] = []
        self.extra = extra
        self.verbose = verbose
        self.known_bases = set()
        self.blacklist = {}          # (addr, thumb) -> reason
        for addr, th, note in self.T['false']:
            # e.g. a data word that happens to point into the middle of a
            # function the pointer harvest cannot tell from an entry
            self.blacklist[(addr, th)] = 'toml false_entry: ' + note
        self.history = []

    # ------------------------------------------------ setup
    def new_analyzer(self):
        an = Analyzer(self.rom_path)
        an.setup()
        prog = self.T['program']
        an.scan_lo = prog.get('aot_scan_start', 0x080000C0)
        an.scan_hi = prog.get('aot_scan_end', 0x08030000)
        for rs, ss, size, _ in self.T['copies']:
            an.cs.add_overlay(rs, size, ss)
        an.data_ranges = list(STATIC_DATA_RANGES)
        for k, why in self.blacklist.items():
            an.rejected[k] = ('blacklist', why)
        self.an = an
        self.origin_count = Counter()
        self.table_slots = {}        # slot addr -> (target, thumb, kind)
        self.vt_bases = set(self.known_bases)
        self.vt_extent = {}          # base -> number of valid slots
        self.vt_slots = set()        # slot offsets seen at virtual call sites
        self.vcall_sites = []
        self.rodata = RODATA[:2]
        self.stats = {}
        self.thunks = []
        self.rom_ptr_sites = {}
        return an

    def find_callvia(self):
        """_call_via_rN stubs: BL targets whose first insn is 'bx rN'."""
        an = self.an
        lo, hi = an.scan_lo, an.scan_hi
        for a in range(lo, hi - 2, 2):
            h, h2 = an.rom.u16(a), an.rom.u16(a + 2)
            if h >> 11 == 0x1e and h2 >> 11 == 0x1f:
                off = ((((h & 0x7ff) ^ 0x400) - 0x400) << 12) + ((h2 & 0x7ff) << 1)
                t = (a + 4 + off) & 0xffffffff
                if lo <= t < hi:
                    ht = an.rom.u16(t)
                    if (ht & 0xff87) == 0x4700 and ((ht >> 3) & 15) not in (14, 15):
                        an.callvia_regs[t] = (ht >> 3) & 15
        return an.callvia_regs

    # ------------------------------------------------ step 1: seeds
    def seed(self):
        an = self.an
        an.add_entry(0x08000000, False, 'reset')
        self.ret_sites = []
        for addr, th, src, resume, note in self.T['funcs']:
            if src is not None and an.cs.phys(addr) != src:
                an.log.append('toml source_addr mismatch %08x: %s vs %s' % (addr, src, an.cs.phys(addr)))
            if th and an.cs.valid(addr - 4, 4) and (an.cs.u16(addr) & 0xff00) != 0xb500:
                pb = an.cs.decode(addr - 4, True)
                if pb.op == 'bl' and pb.size == 4:
                    # return address of a BL: only a resume point for runtimes
                    # that return through the dispatcher (reference recomp)
                    self.ret_sites.append((addr, pb.addr, pb.target, note))
                    if not self.include_ret_sites:
                        continue
            an.add_entry(addr, th, 'toml-resume' if resume else 'toml')
        for addr, stride, count, f_, mode, name in self.T['tables']:
            for slot, t, th in expand_table(an.cs, addr, stride, count, f_, mode):
                self.table_slots[slot] = (t, th, 'toml-table:' + (name or '%08x' % addr))
                an.add_entry(t, th, 'toml-table')
        if self.extra and os.path.exists(self.extra):
            for line in open(self.extra):
                line = line.split('#')[0].strip()
                if line:
                    a, mode = line.split()[:2]
                    an.add_entry(int(a, 16), mode.upper() == 'T', 'extra')
        an.run()

    # ------------------------------------------------ step 2: harvesting
    def literal_words(self):
        s = set()
        for f in self.an.funcs.values():
            for la, sz in f.lits.items():
                if sz == 4 and self.an.cs.valid(la, 4):
                    s.add(self.an.cs.u32(la))
        return s

    def all_consts(self):
        s = self.literal_words()
        for f in self.an.funcs.values():
            s |= f.consts_seen
            s |= f.adrs
        return s

    def _spec(self, t, th, origin, require_gap=False):
        an = self.an
        key = (t, th)
        if key in an.entries or key in an.rejected:
            return False
        if require_gap and (t in an.code_bytes or t in an.mid_bytes):
            an.rejected[key] = (origin, 'inside-known-code')
            return False
        why = an.try_seed(t, th, origin)
        if why is None:
            self.origin_count[origin] += 1
            return True
        return False

    def harvest_consts(self):
        an = self.an
        n = 0
        for v in sorted(self.all_consts()):
            t = v & ~1
            if (v & 1) and an.scan_lo <= t < an.scan_hi and not an.in_data(t):
                n += self._spec(t, True, 'code-const', require_gap=True)
            elif IWRAM_LO <= v < IWRAM_HI and an.cs.phys(t) is not None:
                n += self._spec(t, bool(v & 1), 'iwram-const')
        return n

    def collect_vcall_slots(self):
        """Virtual call sites: ldr rA,[rB,#s] ; add rC,rA,rB -> slot s."""
        an = self.an
        self.vcall_sites = []
        for f in an.funcs.values():
            if not f.thumb:
                continue
            addrs = sorted(f.insns)
            for idx, a in enumerate(addrs):
                i = f.insns[a]
                if not (i.op == 'mem' and i.load and i.msize == 4 and i.off[0] == 'imm'
                        and i.rn not in (13, 15) and i.rd != i.rn):
                    continue
                for j in addrs[idx + 1: idx + 4]:
                    q = f.insns[j]
                    if q.op == 'dp' and q.alu == 'add' and q.op2[0] == 'reg' and q.op2[3] == 0 and \
                            {q.rn, q.op2[1]} == {i.rd, i.rn}:
                        self.vt_slots.add(i.off[1])
                        f.vcall_slots.add(i.off[1])
                        self.vcall_sites.append((a, i.off[1]))
                        break
                    if i.rd in written_regs(q) or i.rn in written_regs(q):
                        break

    def vt_entry(self, b, off):
        """Decode relative vtable slot; returns (target, thumb) or None."""
        an = self.an
        lo, hi = self.rodata
        v = an.cs.u32(b + off)
        raw = (b + v) & 0xffffffff
        t = raw & ~1
        if raw & 1:
            if an.scan_lo <= t < an.scan_hi and not an.in_data(t) and not (lo <= t < hi):
                return t, True
            return None
        if lo <= t < hi and an.cs.valid(t, 16) and an.cs.u32(t) == THUNK_W0:
            return t, False
        return None

    def harvest_vtables(self):
        """vptr literals in rodata are vtable bases; slots are contiguous
        relative entries (target = base + [base+4k]) up to the next base
        or the first entry that is not a code offset."""
        an = self.an
        lo, hi = self.rodata
        self.vt_bases |= {v for v in self.literal_words() if lo <= v < hi and not v & 3}
        allb = sorted(self.vt_bases)
        n = 0
        for k, b in enumerate(allb):
            end = allb[k + 1] if k + 1 < len(allb) else hi
            off = 0
            while b + off + 4 <= end:
                e = self.vt_entry(b, off)
                if e is None:
                    break
                t, th = e
                self.table_slots.setdefault(b + off, (t, th, 'vtable %08x+%x' % (b, off)))
                if th:
                    n += self._spec(t, True, 'vtable', require_gap=True)
                off += 4
            self.vt_extent[b] = off // 4
        return n

    def scan_thunks(self):
        an = self.an
        n = 0
        for a in range(an.scan_lo & ~3, an.scan_hi - 16, 4):
            if an.cs.u32(a) != THUNK_W0:
                continue
            w1, w2, w3 = an.cs.u32(a + 4), an.cs.u32(a + 8), an.cs.u32(a + 12)
            # sub r0,r0,#n (this in r0) or sub r1,r1,#n (struct-returning
            # methods: this in r1)
            if w2 == THUNK_W2 and w3 == THUNK_W3 and (w1 & 0xFFFFF000) in (0xE2400000, 0xE2411000):
                rel = an.cs.u32(a + 16)
                tgt = (a + 16 + rel) & 0xffffffff
                adj = ror32_imm(w1 & 0xfff)
                self.thunks.append((a, adj, tgt))
                if (a, False) not in an.entries:
                    # ARM code inside rodata: commit without the data-range check
                    an.entries[(a, False)] = 'thunk-pattern'
                    f = an.walk(a, False)
                    an.commit(f)
                    an.run()
                    self.origin_count['thunk-pattern'] += 1
                    n += 1
        return n

    def harvest_rom_pointers(self):
        """Whole-ROM absolute pointer scan (odd words into the scan window)."""
        an = self.an
        data = an.rom.data
        cnt = len(data) // 4
        words = struct.unpack_from('<%dI' % cnt, data, 0)
        sites = defaultdict(list)
        for i, v in enumerate(words):
            if (v & 1) and an.scan_lo <= v < an.scan_hi:
                sites[v & ~1].append(0x08000000 + 4 * i)
        self.rom_ptr_sites = sites
        n = 0
        for t in sorted(sites):
            if an.in_data(t):
                continue
            n += self._spec(t, True, 'rom-ptr', require_gap=True)
        return n

    # ------------------------------------------------ step 3: gap fill
    def covered(self):
        an = self.an
        cov = {}
        for f in an.funcs.values():
            for a, ins in f.insns.items():
                for b in range(a, a + ins.size, 2):
                    cov[b] = 'code'
        for f in an.funcs.values():
            for la, sz in f.lits.items():
                for b in range(la & ~1, la + max(sz, 2), 2):
                    cov.setdefault(b, 'literal')
            for ta, tsz in f.jt_data.values():
                for b in range(ta & ~1, ta + tsz, 2):
                    cov.setdefault(b, 'table')
            for ja, tgts in f.jumptables.items():
                ins = f.insns[ja]
                if not f.thumb and ins.op == 'mem':
                    for k in range(len(tgts)):
                        for b in (ins.pcval + 4 * k, ins.pcval + 4 * k + 2):
                            cov.setdefault(b, 'table')
        for slot, (t, th, kind) in self.table_slots.items():
            for b in (slot, slot + 2):
                cov.setdefault(b, 'table')
        return cov

    @staticmethod
    def gaps(lo, hi, cov):
        out = []
        a = lo
        while a < hi:
            if a in cov:
                a += 2
                continue
            s = a
            while a < hi and a not in cov:
                a += 2
            out.append((s, a))
        return out

    def gap_fill(self, ranges):
        an = self.an
        total = 0
        while True:
            cov = self.covered()
            n = 0
            for lo, hi in ranges:
                for s, e in self.gaps(lo, hi, cov):
                    a = s
                    while a < e and an.rom.u16(a) in (0x0000, 0x46C0):
                        a += 2
                    if a >= e or an.in_data(a):
                        continue
                    if (a, True) in an.entries:
                        continue
                    if self._spec(a, True, 'gap-fill'):
                        n += 1
                        continue
                    if a % 4 == 0 and (an.rom.u32(a) >> 28) == 0xE:
                        n += self._spec(a, False, 'gap-fill-arm')
            total += n
            if n == 0:
                return total

    def gap_fill_arm(self, rng):
        """ARM-first gap fill (IWRAM image)."""
        an = self.an
        while True:
            cov = self.covered()
            n = 0
            for s, e in self.gaps(rng[0], rng[1], cov):
                a = (s + 3) & ~3
                while a < e and an.cs.u32(a) == 0:
                    a += 4
                if a >= e:
                    continue
                if (an.cs.u32(a) >> 28) == 0xE:
                    n += self._spec(a, False, 'gap-fill-arm')
                elif not self._spec(a, True, 'gap-fill'):
                    pass
            if n == 0:
                return

    # ------------------------------------------------ validation
    def conflicts(self):
        """Speculative functions whose instructions overlap a literal of, or
        start inside an instruction of, another function."""
        an = self.an
        lit_owner = defaultdict(set)
        mid_owner = defaultdict(set)
        for key, f in an.funcs.items():
            for la, sz in f.lits.items():
                for b in range(la & ~1, la + max(sz, 2), 2):
                    lit_owner[b].add(key)
            for ta, tsz in f.jt_data.values():
                for b in range(ta & ~1, ta + tsz, 2):
                    lit_owner[b].add(key)
            for a, ins in f.insns.items():
                for b in range(a + 2, a + ins.size, 2):
                    mid_owner[b].add(key)
        bad = {}
        # weak pointer-derived entries that turned out to sit inside a body
        # walked from an earlier, independent entry are coincidental words
        # (a literal in code outranks gap fill, though: data that decodes as
        # code may run into a real function)
        insn_owner = defaultdict(set)
        for key, f in an.funcs.items():
            for a in f.insns:
                if a != key[0]:
                    insn_owner[(a, key[1])].add(key)
        for key, f in an.funcs.items():
            origin = an.entries.get(key)
            if origin not in ('rom-ptr', 'code-const'):
                continue
            weaker = ('rom-ptr', 'code-const') + (('gap-fill', 'gap-fill-arm') if origin == 'code-const' else ())
            outer = [o for o in insn_owner.get(key, ()) if o[0] < key[0] and
                     an.entries.get(o) not in weaker]
            if outer:
                bad[key] = (key[0], set(outer))
        for key, f in an.funcs.items():
            if an.entries.get(key) not in SPEC_ORIGINS or key in bad:
                continue
            for a, ins in f.insns.items():
                others = set()
                for b in range(a, a + ins.size, 2):
                    others |= lit_owner.get(b, set()) - {key}
                if a in mid_owner:
                    others |= mid_owner[a] - {key}
                if others:
                    bad[key] = (a, others)
                    break
        # remove only those whose accuser is not itself accused
        out = {}
        for key, (a, others) in bad.items():
            clean = [o for o in others if o not in bad]
            if clean or all(ORIGIN_RANK.get(an.entries.get(o), 1) < ORIGIN_RANK.get(an.entries.get(key), 4)
                            for o in others):
                out[key] = 'post-validate: %08x conflicts with %s' % (
                    a, ','.join('%08x' % o[0] for o in sorted(others)))
        return out

    # ------------------------------------------------ main
    def run_once(self):
        an = self.new_analyzer()
        self.find_callvia()
        self.seed()
        self.stats['1_seeds'] = (len(an.funcs), len(self.unique_insns()))
        an.data_ranges.append(RODATA)
        self.scan_thunks()
        for rnd in range(10):
            before = len(an.funcs)
            self.harvest_consts()
            self.harvest_vtables()
            if len(an.funcs) == before:
                break
        self.stats['2_consts_vtables'] = (len(an.funcs), len(self.unique_insns()))
        self.harvest_rom_pointers()
        self.stats['3_rom_pointers'] = (len(an.funcs), len(self.unique_insns()))
        self.text_ranges = [LAYOUT['text'], (TEXT2, self.code_end())]
        self.gap_fill_arm(IWRAM_CODE)
        for rnd in range(10):
            before = len(an.funcs)
            self.gap_fill(self.text_ranges)
            self.harvest_consts()
            self.harvest_vtables()
            self.text_ranges[1] = (TEXT2, self.code_end())
            if len(an.funcs) == before:
                break
        self.stats['4_gap_fill'] = (len(an.funcs), len(self.unique_insns()))

    def run(self):
        for it in range(8):
            self.run_once()
            newb = self.vt_bases - self.known_bases
            bad = self.conflicts()
            self.history.append((it, len(self.an.funcs), len(newb), len(bad)))
            if self.verbose:
                print('pass', it, self.stats, 'new bases', len(newb), 'conflicts', len(bad))
            if not newb and not bad:
                break
            self.known_bases |= self.vt_bases
            self.blacklist.update(bad)
        self.collect_vcall_slots()
        self.final_walk()
        self.stats['5_final'] = (len(self.an.funcs), len(self.unique_insns()))

    def code_end(self):
        """End of the second text block ([layout] text2..): last insn + its pool."""
        an = self.an
        hi = 0
        for f in an.funcs.values():
            for a, ins in f.insns.items():
                if TEXT2 <= a < an.scan_hi:
                    hi = max(hi, a + ins.size)
        for f in an.funcs.values():
            for la, sz in f.lits.items():
                if TEXT2 <= la < an.scan_hi and la < hi + 0x400:
                    hi = max(hi, la + sz)
        return (hi + 3) & ~3

    def final_walk(self):
        """Re-walk every function with the complete entry set so that
        branches to other entries are recorded as tail calls."""
        an = self.an
        newf = {}
        for key in sorted(an.funcs):
            newf[key] = an.walk(*key)
        an.funcs = newf
        an.lit_bytes, an.mid_bytes, an.code_bytes = set(), set(), set()
        for f in newf.values():
            for la, sz in f.lits.items():
                for b in range(la, la + max(sz, 2), 2):
                    an.lit_bytes.add(b)
            for ta, tsz in f.jt_data.values():
                for b in range(ta & ~1, ta + tsz, 2):
                    an.lit_bytes.add(b)
            for a, ins in f.insns.items():
                an.code_bytes.add(a)
                for b in range(a + 2, a + ins.size, 2):
                    an.mid_bytes.add(b)

    def unique_insns(self):
        u = set()
        for (a, th), f in self.an.funcs.items():
            for x in f.insns:
                u.add((x, th))
        return u


def ror32_imm(imm12):
    rot = (imm12 >> 8) * 2
    v = imm12 & 0xff
    return ((v >> rot) | (v << (32 - rot))) & 0xffffffff if rot else v


# ===================================================================== reports
def op_kind(i):
    """Fine-grained op label for feature statistics."""
    op = i.op
    c = '' if i.cond == 14 else '.cond'
    if op == 'dp':
        o = i.op2
        sh = ''
        if o and o[0] == 'reg' and not (o[2] == 'lsl' and o[3] == 0):
            sh = '+shift_imm'
        elif o and o[0] == 'regsh':
            sh = '+shift_reg'
        pc = ''
        if i.rd == 15 and i.alu not in ('tst', 'teq', 'cmp', 'cmn'):
            pc = '->pc'
        elif i.rn == 15 or (o and o[0] in ('reg', 'regsh') and o[1] == 15):
            pc = '(reads pc)'
        return 'dp.%s%s%s%s%s' % (i.alu, 's' if i.s else '', sh, pc, c)
    if op == 'mem':
        nm = ('ldr' if i.load else 'str') + {1: 'b', 2: 'h', 4: ''}[i.msize] + ('s' if i.signed else '')
        mode = 'pcrel' if i.rn == 15 else ('sp' if i.rn == 13 else ('regoff' if i.off[0] == 'reg' else 'imm'))
        if i.off[0] == 'reg' and not (i.off[2] == 'lsl' and i.off[3] == 0):
            mode += '+shift'
        wb = ''
        if not i.pre:
            wb = '.post'
        elif i.wb:
            wb = '.pre!'
        if i.user and not i.thumb:
            wb += '.T'
        pc = '->pc' if (i.load and i.rd == 15) else ('(stores pc)' if (not i.load and i.rd == 15) else '')
        return 'mem.%s.%s%s%s%s' % (nm, mode, wb, pc, c)
    if op == 'ldm':
        nm = 'pop' if (i.thumb and i.rn == 13 and i.load) else 'push' if (i.thumb and i.rn == 13) else \
            ('ldm' if i.load else 'stm')
        if not i.thumb:
            nm += {(False, False): 'da', (False, True): 'ia', (True, False): 'db', (True, True): 'ib'}[(i.pre, i.up)]
            if i.rn == 13:
                nm += '(sp)'
        return 'blk.%s%s%s%s%s' % (nm, '!' if i.wb and not (i.thumb and i.rn == 13) else '',
                                   '^S' if i.user else '', '+pc' if (i.load and 15 in i.regs) else '', c)
    if op == 'mul':
        return 'mul.%s%s%s' % ('mla' if i.acc else 'mul', 's' if i.s else '', c)
    if op == 'mull':
        return 'mull.%s%s%s%s' % ('s' if i.sgn else 'u', 'mlal' if i.acc else 'mull', 's' if i.s else '', c)
    if op == 'bx':
        return 'bx.%s%s' % ('lr' if i.rm == 14 else 'pc' if i.rm == 15 else 'reg', c)
    if op == 'b':
        return 'b%s' % c
    if op in ('mrs', 'msr'):
        return '%s.%s%s' % (op, 'spsr' if i.spsr else 'cpsr', c)
    return op + c


def write_reports(d, outdir):
    an = d.an
    funcs = an.funcs
    os.makedirs(outdir, exist_ok=True)
    W = lambda name: open(os.path.join(outdir, name), 'w')
    uniq = {}
    for (fa, th), f in funcs.items():
        for a, i in f.insns.items():
            uniq[(a, th)] = i
    summary = {}
    summary['stats_by_phase'] = d.stats
    summary['functions'] = len(funcs)
    summary['functions_thumb'] = sum(1 for k in funcs if k[1])
    summary['functions_arm'] = sum(1 for k in funcs if not k[1])
    summary['unique_insns'] = len(uniq)
    summary['unique_insns_thumb'] = sum(1 for k in uniq if k[1])
    summary['unique_insns_arm'] = sum(1 for k in uniq if not k[1])
    summary['origins'] = dict(Counter(an.entries.get(k, '?') for k in funcs))
    summary['passes'] = d.history
    summary['blacklisted'] = len(d.blacklist)

    # ---------------------------------------------------------- coverage
    cov = d.covered()
    Lt, Lc, Ld, Lv, Lr, Li, Lid = (LAYOUT[k] for k in ('text', 'ctors', 'dtors', 'veneers', 'rodata',
                                                       'iwram_code', 'iwram_data'))
    regions = [
        ('crt0+header 08000000-%08X' % Lt[0], 0x08000000, Lt[0]),
        ('.text %08X-%08X' % Lt, Lt[0], Lt[1]),
        ('ctors/dtors %08X-%08X' % (Lc[0], Ld[1]), Lc[0], Ld[1]),
        ('veneers %08X-%08X' % Lv, Lv[0], Lv[1]),
        ('rodata vtables %08X-%08X' % Lr, Lr[0], Lr[1]),
        ('.text2 %08X-%08X' % (TEXT2, d.code_end()), TEXT2, d.code_end()),
        ('tail data %08X-%08X' % (d.code_end(), an.scan_hi), d.code_end(), an.scan_hi),
        ('IWRAM code %08X-%08X' % Li, Li[0], Li[1]),
        ('IWRAM .data %08X-%08X' % Lid, Lid[0], Lid[1]),
    ]
    consts = d.all_consts()
    unknown_runs = []
    covlines = []
    totals = Counter()
    for name, lo, hi in regions:
        c = Counter()
        for a in range(lo, hi, 2):
            if a in cov:
                c[cov[a]] += 2
        for s, e in d.gaps(lo, hi, cov):
            if not an.cs.valid(s, e - s):
                # declared .data outside the code-copy overlay (not mapped)
                c['data(declared)'] += e - s
                continue
            vals = [an.cs.u16(a) for a in range(s, e, 2)]
            if all(v in (0, 0x46C0) for v in vals):
                c['padding'] += e - s
                continue
            if lo == 0x08000000 and s < 0x080000C0:
                c['header'] += e - s
                continue
            if an.in_data(s) or (Lid[0] <= s < Lid[1]):
                c['data(declared)'] += e - s
                continue
            raw = bytes(an.rom.data[an.cs.phys(s) - 0x08000000: an.cs.phys(s) - 0x08000000 + (e - s)])
            printable = sum(1 for ch in raw if 0x20 <= ch < 0x7f or ch in (0, 9, 10))
            if printable >= 0.85 * len(raw):
                kind = 'string'
            elif any(s <= v < e for v in consts):
                kind = 'data(referenced)'
            elif lo >= TEXT2 and s >= d.code_end():
                kind = 'data(after code)'
            else:
                kind = 'unknown'
            c[kind] += e - s
            unknown_runs.append((s, e, kind, raw[:48]))
        size = hi - lo
        codeish = c['code'] + c['literal'] + c['table'] + c['padding']
        covlines.append('%-36s size=%6d  code=%6d lit=%5d table=%5d pad=%4d string=%4d dataref=%4d data=%5d unknown=%5d  ->'
                        ' code+lit+table+pad=%.2f%%' % (
                            name, size, c['code'], c['literal'], c['table'], c['padding'], c['string'],
                            c['data(referenced)'], c['data(declared)'] + c['data(after code)'] + c['header'],
                            c['unknown'], 100.0 * codeish / max(size, 1)))
        for k, v in c.items():
            totals[k] += v
    # aggregated over the executable text (crt0 + .text + veneers + .text2)
    text_lo_hi = [(0x080000C0, Lt[1]), Lv, (TEXT2, d.code_end())]
    tc = Counter()
    for lo, hi in text_lo_hi:
        for a in range(lo, hi, 2):
            tc[cov.get(a, 'gap')] += 2
    tsize = sum(hi - lo for lo, hi in text_lo_hi)
    gapb = tc['gap']
    strb = sum(e - s for s, e, k, _ in unknown_runs if k in ('string', 'data(referenced)') and
               any(lo <= s < hi for lo, hi in text_lo_hi))
    unkb = sum(e - s for s, e, k, _ in unknown_runs if k == 'unknown' and any(lo <= s < hi for lo, hi in text_lo_hi))
    padb = gapb - strb - unkb
    summary['text_bytes'] = tsize
    summary['text_code_bytes'] = tc['code']
    summary['text_literal_bytes'] = tc['literal']
    summary['text_table_bytes'] = tc['table']
    summary['text_padding_bytes'] = padb
    summary['text_string_data_bytes'] = strb
    summary['text_unknown_bytes'] = unkb
    summary['text_code_pct'] = round(100.0 * tc['code'] / tsize, 2)
    summary['text_explained_pct'] = round(100.0 * (tsize - unkb) / tsize, 3)
    with W('coverage.txt') as fh:
        fh.write('\n'.join(covlines) + '\n\n')
        fh.write('executable text (%s): %d bytes\n' % (
            ', '.join('%08X-%08X' % r for r in text_lo_hi), tsize))
        fh.write('  code %d (%.2f%%), literal %d, inline tables %d, padding %d, strings/data %d, unknown %d\n\n' % (
            tc['code'], 100.0 * tc['code'] / tsize, tc['literal'], tc['table'], padb, strb, unkb))
        fh.write('non-code runs inside the scan window (start end kind first-bytes):\n')
        for s, e, k, raw in unknown_runs:
            fh.write('%08x %08x %5d %-18s %r\n' % (s, e, e - s, k, raw))

    # ---------------------------------------------------------- indirect transfers
    ind = {}
    for (fa, th), f in funcs.items():
        for ia, kind, det in f.indirect:
            ind.setdefault((ia, th), (kind, det, fa))
    callvia_sites = {}
    for (fa, th), f in funcs.items():
        for ba, reg, res in f.callvia:
            callvia_sites[ba] = (reg, res, fa)
    summary['indirect_by_kind'] = dict(Counter('%s:%s' % ('T' if th else 'A', k)
                                               for (ia, th), (k, det, fa) in ind.items()))
    summary['call_via_bl_sites'] = len(callvia_sites)
    summary['call_via_bl_sites_resolved'] = sum(1 for v in callvia_sites.values() if v[1] is not None)
    summary['call_via_by_reg'] = dict(Counter('r%d' % v[0] for v in callvia_sites.values()))
    with W('indirect.txt') as fh:
        fh.write('# every indirect control transfer (addr mode kind detail function)\n')
        for (ia, th), (k, det, fa) in sorted(ind.items()):
            fh.write('%08x %s %-16s %-30s %08x\n' % (ia, 'T' if th else 'A', k, det, fa))
        fh.write('\n# BL _call_via_rN sites (computed calls; reg resolved-target function)\n')
        for ba, (reg, res, fa) in sorted(callvia_sites.items()):
            fh.write('%08x r%d %s %08x\n' % (ba, reg, '%08x' % res if res is not None else '-', fa))
    with W('jumptables.txt') as fh:
        fh.write('# insn addr, function, table addr, bytes, count, targets\n')
        for (fa, th), f in sorted(funcs.items()):
            for ja, tg in sorted(f.jumptables.items()):
                ta, tsz = f.jt_data.get(ja, (0, 0))
                fh.write('%08x %08x table=%08x size=%d count=%d -> %s\n' % (
                    ja, fa, ta, tsz, len(tg), ' '.join('%08x' % t for t in tg)))
        fh.write('\n# data tables of code pointers (toml [[jump_table]] + vtables)\n')
        for addr, stride, count, f_, mode, name in d.T['tables']:
            fh.write('toml %08x stride=%d count=%d %s %s %s\n' % (addr, stride, count, f_, mode, name))
    summary['switch_tables'] = sum(len(f.jumptables) for f in funcs.values())

    # ---------------------------------------------------------- vtables
    with W('vtables.txt') as fh:
        fh.write('# relative vtables: target = base + u32(base + 4*k)  (odd=Thumb, even=ARM thunk)\n')
        bases = sorted(d.vt_bases)
        for b in bases:
            n = d.vt_extent.get(b, 0)
            ents = []
            for k in range(n):
                e = d.vt_entry(b, 4 * k)
                ents.append('%08x%s' % (e[0], 'T' if e[1] else 'A') if e else '?')
            fh.write('%08x slots=%d  %s\n' % (b, n, ' '.join(ents)))
        fh.write('\n# virtual-call slot offsets seen at call sites: %s\n' % sorted(d.vt_slots))
        fh.write('\n# ARM this-adjusting thunks (addr, this -= n, target)\n')
        for a, adj, tgt in d.thunks:
            fh.write('%08x sub r0,r0,#0x%x -> %08x%s\n' % (a, adj, tgt & ~1, 'T' if tgt & 1 else 'A'))
    summary['vtables'] = len(d.vt_bases)
    summary['vtable_slots'] = sum(d.vt_extent.values())
    summary['arm_thunks'] = len(d.thunks)

    # ---------------------------------------------------------- features
    feat = {True: Counter(), False: Counter()}
    for (a, th), i in uniq.items():
        feat[th][op_kind(i)] += 1
    special = []
    swis = Counter()
    swi_sites = defaultdict(list)
    for (a, th), i in sorted(uniq.items()):
        if i.op in ('mrs', 'msr', 'swp', 'mull', 'blh', 'blprefix', 'undef'):
            special.append('%08x %s %s %s' % (a, 'T' if th else 'A', i.op, fmt(i)))
        elif i.op == 'ldm' and i.user:
            special.append('%08x %s ldm/stm-S %s' % (a, 'T' if th else 'A', fmt(i)))
        elif i.op == 'mem' and i.rd == 15:
            special.append('%08x %s %s-pc %s' % (a, 'T' if th else 'A', 'ldr' if i.load else 'str', fmt(i)))
        elif i.op == 'mem' and i.user and not th:
            special.append('%08x A ldrt/strt %s' % (a, fmt(i)))
        if i.op == 'swi':
            num = i.num if th else (i.raw & 0xffffff)
            swis[(th, num)] += 1
            swi_sites[(th, num)].append(a)
    iw_writes = []
    for (fa, th), f in funcs.items():
        for ia, ta, nb in f.const_stores:
            if IWRAM_CODE[0] <= ta < IWRAM_CODE[1]:
                iw_writes.append((ia, ta, nb, fa))
    swi_args = []
    for (fa, th), f in funcs.items():
        for sa, num, r0, r1, r2 in f.swis:
            if th and num in (0x0B, 0x0C, 0x11, 0x12, 0x14, 0x15) and r1 is not None:
                swi_args.append((sa, num, r0, r1, r2))
    with W('features.txt') as fh:
        for th in (True, False):
            fh.write('==== %s: %d unique insns\n' % ('THUMB' if th else 'ARM', sum(feat[th].values())))
            for k, v in sorted(feat[th].items(), key=lambda x: -x[1]):
                fh.write('  %6d %s\n' % (v, k))
        fh.write('\n==== SWI numbers (mode, number, count, sites)\n')
        for (th, num), v in sorted(swis.items()):
            fh.write('  %s 0x%x x%d  %s\n' % ('T' if th else 'A', num, v,
                                            ' '.join('%08x' % s for s in swi_sites[(th, num)][:24])))
        fh.write('\n==== special instructions (MRS/MSR, SWP, long multiply, LDM/STM ^, ldr/str pc, lone BL halves)\n')
        fh.write('\n'.join(special) + '\n')
        fh.write('\n==== static stores into IWRAM code %08X-%08X (insn, target, bytes, function)\n' % IWRAM_CODE)
        for r in sorted(iw_writes):
            fh.write('  %08x -> %08x (%d) in %08x\n' % r)
        fh.write('\n==== memory SWIs with constant r1 (addr, swi, r0, r1, r2)\n')
        for sa, num, r0, r1, r2 in sorted(swi_args):
            fh.write('  %08x swi 0x%x r0=%s r1=%s r2=%s\n' % (sa, num, *(('%08x' % x) if x is not None else '?' for x in (r0, r1, r2))))
    summary['swi'] = {'%s:0x%x' % ('T' if th else 'A', n): v for (th, n), v in sorted(swis.items())}
    summary['special_insns'] = len(special)
    summary['iwram_code_static_stores'] = len(iw_writes)

    # ---------------------------------------------------------- ARM code, veneers, overlaps
    with W('arm_and_veneers.txt') as fh:
        fh.write('# ARM functions (entry, origin, insns, extent)\n')
        for (a, th), f in sorted(funcs.items()):
            if th:
                continue
            lo = min(f.insns)
            hi = max(x + f.insns[x].size for x in f.insns)
            fh.write('%08x %-14s n=%4d %08x-%08x\n' % (a, an.entries.get((a, th)), len(f.insns), lo, hi))
        fh.write('\n# mode-switch / constant-target veneers (site, function, target)\n')
        for (fa, th), f in sorted(funcs.items()):
            for va, t, tt in f.veneers:
                fh.write('%08x %s in %08x -> %08x %s\n' % (va, 'T' if th else 'A', fa, t & ~1,
                                                          'Thumb' if tt else 'ARM'))
    entries = set(funcs)
    shared = []
    for (fa, th), f in sorted(funcs.items()):
        inner = sorted(x for x in f.insns if x != fa and (x, th) in entries)
        if inner:
            shared.append((fa, th, inner))
    tails_to_mid = []
    for (fa, th), f in funcs.items():
        for x, i in f.insns.items():
            if i.op == 'b' and (i.target, th) not in entries:
                for (ga, gth), g in funcs.items():
                    pass
    ov_kind = Counter()
    with W('overlaps.txt') as fh:
        fh.write('# functions whose body contains other function entries\n')
        fh.write('# kind: fall = reached by sequential fallthrough, br = reached by a branch inside the outer function\n')
        for fa, th, inner in shared:
            f = funcs[(fa, th)]
            parts = []
            for x in inner:
                prevs = [q for q in (x - 2, x - 4) if q in f.insns and f.insns[q].next == x]
                pi = f.insns[prevs[0]] if prevs else None
                fall = pi is not None and not (pi.op == 'b' and pi.cond == 14) and not (
                    pi.op == 'bx' and pi.cond == 14) and not (pi.writes_pc() and pi.cond == 14)
                kind = 'fall' if fall else 'br'
                ov_kind[kind] += 1
                parts.append('%08x(%s,%s)' % (x, kind, an.entries.get((x, th))))
            fh.write('%08x %s %-10s contains %s\n' % (fa, 'T' if th else 'A', an.entries.get((fa, th)),
                                                     ' '.join(parts)))
        fh.write('\n# toml [[extra_func]] seeds that are BL return addresses (%d); %s\n' % (
            len(d.ret_sites), 'included' if d.include_ret_sites else 'NOT used as entries'))
        for addr, bla, tgt, note in d.ret_sites:
            fh.write('%08x  after bl@%08x -> %08x   %s\n' % (addr, bla, tgt, note[:70]))
    summary['overlap_kinds'] = dict(ov_kind)
    summary['toml_return_sites'] = len(d.ret_sites)
    summary['functions_containing_other_entries'] = len(shared)
    summary['shared_entry_points'] = len({x for _, _, inner in shared for x in inner})

    with W('rejected.txt') as fh:
        for (t, th), (o, why) in sorted(an.rejected.items()):
            fh.write('%08x %s %-12s %s\n' % (t, 'T' if th else 'A', o, why))
    with W('functions.txt') as fh:
        for (a, th), f in sorted(funcs.items()):
            lo = min(f.insns)
            hi = max(x + f.insns[x].size for x in f.insns)
            fh.write('%08x %s %-13s insns=%4d extent=%08x-%08x calls=%d tails=%d\n' % (
                a, 'T' if th else 'A', an.entries.get((a, th), '?'), len(f.insns), lo, hi,
                len(f.calls), len(f.tails)))
    out = []
    for (a, th), f in sorted(funcs.items()):
        out.append({
            'addr': a, 'thumb': th, 'origin': an.entries.get((a, th), '?'),
            'labels': sorted(f.labels), 'insns': sorted(f.insns),
            'jumptables': {str(k): v for k, v in f.jumptables.items()},
            'const_branch': {str(k): list(v) for k, v in f.const_branch.items()},
            'indirect': [[x, k, det] for x, k, det in f.indirect],
            'lits': sorted(f.lits), 'bad': [list(b) for b in f.bad],
        })
    json.dump({'functions': out, 'overlays': an.cs.overlays,
               'callvia': {str(k): v for k, v in an.callvia_regs.items()},
               'vtables': {str(b): d.vt_extent.get(b, 0) for b in sorted(d.vt_bases)},
               'summary': summary},
              open(os.path.join(outdir, 'functions.json'), 'w'))
    json.dump(summary, open(os.path.join(outdir, 'summary.json'), 'w'), indent=1)
    return summary


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    rom_path, toml_path, outdir = sys.argv[1:4]
    extra = sys.argv[4] if len(sys.argv) > 4 else None
    d = Driver(rom_path, toml_path, extra, verbose=True,
               include_ret_sites=os.environ.get('WITH_RET_SITES') == '1',
               use_toml_funcs=os.environ.get('NO_TOML_FUNCS') != '1')
    d.run()
    s = write_reports(d, outdir)
    print(json.dumps(s, indent=1))


if __name__ == '__main__':
    main()

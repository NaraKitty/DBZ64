"""Interprocedural register summaries for the recompiler.

For every function F we compute
  IN(F)  - registers/flags whose value at entry may be read
  MOD(F) - registers/flags whose value at return may differ from entry
so call sites only store IN(callee) and reload MOD(callee).

Forward pass: value numbering of entry values.  val[r] = the entry register
whose original value r currently holds (or -1).  The stack pointer is tracked
as an offset from its entry value (also through frame-pointer registers), and
stack slots remember which entry value was stored there.  This recognises the
push {r4-r7,lr} ... pop {r4-r7} / pop {r0}; bx r0 pattern: r4-r7 end up
holding their own entry values (not modified) and the final transfer goes to
the entry lr (a return).  Any indirect transfer that is not provably a return
is an unknown exit and makes the summary conservative.
"""
from recomp_consts import ALLREGS, FLAGS, bit, COND_AL, COND_FLAGS, use_def_basic

NREG = 15


class State:
    __slots__ = ('sp', 'val', 'fl', 'slots', 'cst')

    def __init__(self, sp=0, val=None, fl=FLAGS, slots=None, cst=None):
        self.sp = sp                                   # sp - sp_entry or None
        self.val = list(range(NREG)) if val is None else val
        self.fl = fl                                   # flags still at entry value
        self.slots = {} if slots is None else slots    # sp offset -> entry reg id
        self.cst = {} if cst is None else cst          # reg -> known constant

    def copy(self):
        return State(self.sp, list(self.val), self.fl, dict(self.slots), dict(self.cst))

    def merge(self, o):
        sp = self.sp if self.sp == o.sp else None
        val = [a if a == b else -1 for a, b in zip(self.val, o.val)]
        fl = self.fl & o.fl
        slots = {k: v for k, v in self.slots.items() if o.slots.get(k) == v}
        cst = {k: v for k, v in self.cst.items() if o.cst.get(k) == v}
        ns = State(sp, val, fl, slots, cst)
        ch = (sp != self.sp or val != self.val or fl != self.fl or slots != self.slots
              or cst != self.cst)
        return ns, ch

    def modified(self):
        m = 0
        for r in range(NREG):
            # sp is tracked as an offset (self.sp), not through val[]
            if r != 13 and self.val[r] != r:
                m |= bit(r)
        if self.sp != 0:
            m |= bit(13)
        return m | (FLAGS & ~self.fl)


# value ids >= 100 encode "sp_entry + offset" for frame pointers
SPV = 1000000


def is_spv(v):
    return v is not None and v >= SPV // 2


# Calling-convention summary assumed for indirect calls (virtual calls,
# function pointers).  It is verified after the fixpoint: if any
# address-taken function reads or modifies more, the summary is widened.
IND_IN = 0xF | (1 << 12) | (1 << 13) | (1 << 14) | FLAGS
IND_MOD = 0xF | (1 << 12) | (1 << 14) | FLAGS


class Summaries:
    def __init__(self, gens, addr_taken=(), noreturn=()):
        self.gens = gens
        self.noreturn = set(noreturn)
        self.addr_taken = [k for k in addr_taken if k in gens]
        self.ind_in = IND_IN
        self.ind_mod = IND_MOD
        self.IN = {k: 0 for k in gens}
        self.MOD = {k: 0 for k in gens}
        self.saved = {k: 0 for k in gens}
        self.returns = {k: set() for k in gens}   # insn addrs proven to be returns

    def callee_summary(self, key):
        if key in self.noreturn:
            # control never comes back: nothing is modified on return paths
            return (self.IN[key] if key in self.gens else 0xF), 0
        if key in self.gens:
            return self.IN[key], self.MOD[key]
        return ALLREGS | FLAGS, ALLREGS | FLAGS

    def run(self, max_rounds=20):
        """Fixpoint over all functions, then verify the indirect-call summary
        against every address-taken function and widen it if needed."""
        total = 0
        for rnd in range(max_rounds):
            total += self.run_inner()
            ni, nm = self.ind_in, self.ind_mod
            bad = []
            for k in self.addr_taken:
                # A register the target restores before returning is only read
                # to be saved; its stale value cannot escape (see the note in
                # recomp.py on cpu.r[] staleness), so it is not a real input.
                real_in = self.IN[k] & self.MOD[k]
                vi = real_in & ~ni
                vm = self.MOD[k] & ~nm
                if vi or vm:
                    bad.append(k)
                    ni |= vi
                    nm |= vm
            if ni == self.ind_in and nm == self.ind_mod:
                self.widened = rnd
                return total
            if rnd == 0:
                self.nonconforming = [(k, self.IN[k], self.MOD[k]) for k in bad]
            self.ind_in, self.ind_mod = ni, nm
        raise RuntimeError('indirect summary did not converge')

    def run_inner(self, max_iter=100):
        for it in range(max_iter):
            changed = False
            for key, g in self.gens.items():
                inn, mod, saved, rets = self.analyze(g)
                ni = self.IN[key] | inn
                nm = self.MOD[key] | mod
                if ni != self.IN[key] or nm != self.MOD[key] or saved != self.saved[key] \
                        or rets != self.returns[key]:
                    self.IN[key] = ni
                    self.MOD[key] = nm
                    self.saved[key] = saved
                    self.returns[key] = rets
                    changed = True
            if not changed:
                return it
        raise RuntimeError('ABI summaries did not converge')

    def call_effect(self, g, i):
        k = g.kind[i.addr]
        if k == 'call':
            return self.callee_summary((i.target, g.thumb))
        if k == 'ccall':
            t, tt, _ = g.const_branch[i.addr]
            return self.callee_summary((t, tt))
        if k == 'icall':
            return self.ind_in, self.ind_mod
        if k == 'swi':
            # sp too: IRQ handlers run inside Halt on the interrupted stack
            return 0xF | bit(13), 0xF | bit(12) | FLAGS
        if k == 'psr' and i.op == 'msr':
            return ALLREGS | FLAGS, ALLREGS | FLAGS
        return None

    # ------------------------------------------------------------------
    def analyze(self, g):
        ins = g.ins
        entry = g.addr
        st_in = {entry: State()}
        work = [entry]
        mod = 0
        unknown = False
        other_loads = 0
        rets = set()
        visits = 0
        exec_st = {}      # state after a conditional insn on its taken path
        skip_st = {}      # state when the whole same-condition chain is skipped
        while work:
            a = work.pop()
            visits += 1
            if visits > 400000:
                return ALLREGS | FLAGS, ALLREGS | FLAGS, 0, set()
            i = ins[a]
            k = g.kind[a]
            st = st_in[a].copy()
            cond = i.cond != COND_AL
            if cond:
                # 'ldmmi sp!,{..lr}; bxmi lr': evaluate a chain of insns with
                # the same condition (no flag writes between) on one path
                p = ins.get(a - i.size)
                if (p is not None and p.cond == i.cond and p.addr in exec_st
                        and not (use_def_basic(p)[1] & FLAGS)):
                    est = exec_st[p.addr].copy()
                else:
                    est = st.copy()
            else:
                est = st
            ce = self.call_effect(g, i)
            target_val = None
            if ce is not None:
                _, cmod = ce
                if k in ('call', 'ccall', 'icall'):
                    # a call that returns here balanced the stack and left our
                    # frame alone (codegen still reloads sp if the summary says
                    # so); without this one unknown callee would make every
                    # caller's return unprovable
                    keep_sp, keep_slots = est.sp, est.slots
                    self.clobber(est, cmod)
                    est.sp, est.slots = keep_sp, keep_slots
                    est.val[14] = -1
                else:
                    self.clobber(est, cmod)
            else:
                target_val, ol = self.step_consts(est, i, g)
                other_loads |= ol
            if cond:
                exec_st[a] = est.copy()
                st, _ = st.merge(est)
            if k == 'indirect':
                if not g.jumptables.get(a):
                    if target_val == 14 and est.sp == 0:
                        rets.add(a)
                        mod |= est.modified()
                    elif est.val[14] == 14:
                        # indirect tail call (e.g. _call_via_rN): returns to
                        # our caller through the callee
                        mod |= est.modified() | self.ind_mod
                    else:
                        unknown = True
            elif k in ('tail', 'ctail'):
                if k == 'tail':
                    tkey = (i.target, g.thumb)
                else:
                    t, tt, _ = g.const_branch[a]
                    tkey = (t, tt)
                tin, tmod = self.callee_summary(tkey)
                if est.val[14] != 14 and tkey not in self.noreturn:
                    unknown = True     # callee will not return to our caller
                mod |= est.modified() | tmod
            if cond:
                p = ins.get(a - i.size)
                if (p is not None and p.cond == i.cond and p.addr in skip_st
                        and not (use_def_basic(p)[1] & FLAGS)):
                    skip_st[a] = skip_st[p.addr]
                else:
                    skip_st[a] = st_in[a].copy()
            for s in g.succ[a]:
                if s == 'D':
                    continue
                out = st
                if cond and k in ('indirect', 'tail', 'ctail') and s == i.next:
                    # condition false: nothing in the same-condition chain ran
                    out = skip_st[a]
                elif cond and k == 'branch' and s == i.target and s != i.next:
                    out = est
                if s not in st_in:
                    st_in[s] = out.copy()
                    work.append(s)
                else:
                    ns, ch = st_in[s].merge(out)
                    if ch:
                        st_in[s] = ns
                        work.append(s)
        if g.falls_off_end():
            unknown = True
        if unknown:
            mod = ALLREGS | FLAGS
        saved = 0
        for r in range(4, 12):
            if not (mod & bit(r)) and not (other_loads & bit(r)):
                saved |= bit(r)
        inn = self.live_in(g, mod, saved, unknown)[0]
        return inn, mod, saved, rets

    def clobber(self, st, mask):
        for r in range(NREG):
            if mask & bit(r):
                st.val[r] = -1
        if mask & bit(13):
            st.sp = None
        st.fl &= ~(mask & FLAGS)

    def setreg(self, st, r, v):
        if r == 13:
            if is_spv(v):
                st.sp = v - SPV
                st.val[13] = -1
            else:
                st.sp = None
                st.val[13] = -1
            return
        st.val[r] = v

    def regval(self, st, r):
        if r == 13:
            return None if st.sp is None else SPV + st.sp
        return st.val[r]

    def step_consts(self, st, i, g):
        """step() plus constant tracking for literal loads / immediate moves
        (needed for 'ldr r3,=-0x800; add sp, r3' style frames)."""
        pre_cst = dict(st.cst)
        res = self.step(st, i)
        u, d = use_def_basic(i)
        for r in list(st.cst):
            if d & bit(r):
                del st.cst[r]
        if i.op == 'mem' and i.load and i.rn == 15 and i.off[0] == 'imm' and i.pre \
                and i.msize == 4 and i.rd < 13:
            v = g.const_rom((i.pcval + (i.off[1] if i.up else -i.off[1])) & 0xffffffff, 4)
            if v is not None:
                st.cst[i.rd] = v
        elif i.op == 'dp' and i.rd < 13 and i.cond == COND_AL:
            v = self.const_dp(i, pre_cst)
            if v is not None:
                st.cst[i.rd] = v
        return res

    @staticmethod
    def const_dp(i, cst):
        """Fold mov/shift/add/sub/neg of known constants (32-bit)."""
        o = i.op2
        if o[0] == 'imm':
            b = o[1]
        elif o[0] == 'reg' and o[1] in cst and o[2] in ('lsl', 'lsr', 'asr'):
            x, n = cst[o[1]], o[3]
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
        if i.rn not in cst:
            return None
        a = cst[i.rn]
        if i.alu == 'add':
            return (a + b) & 0xffffffff
        if i.alu == 'sub':
            return (a - b) & 0xffffffff
        if i.alu == 'rsb':
            return (b - a) & 0xffffffff
        if i.alu == 'orr':
            return a | b
        if i.alu == 'and':
            return a & b
        return None

    def step(self, st, i):
        """Forward transfer. Returns (target value id for PC writes, mask of
        saved-slot values loaded into other registers)."""
        op = i.op
        other = 0
        tval = None
        if op == 'ldm':
            n = len(i.regs)
            if i.rn == 13 and st.sp is not None:
                if i.up:
                    base = st.sp + (4 if i.pre else 0)
                else:
                    base = st.sp - 4 * n + (0 if i.pre else 4)
                for idx, r in enumerate(i.regs):
                    off = base + 4 * idx
                    if i.load:
                        src = st.slots.get(off, -1)
                        if r == 15:
                            tval = src
                            continue
                        if src != r and 0 <= src < NREG:
                            other |= bit(src)
                        self.setreg(st, r, src if src >= 0 else -1)
                    else:
                        v = self.regval(st, r) if r < 15 else -1
                        if v is not None and 0 <= v < NREG:
                            st.slots[off] = v
                        else:
                            st.slots.pop(off, None)
                if i.wb and not (i.load and 13 in i.regs):
                    st.sp = st.sp + (4 * n if i.up else -4 * n)
                return tval, other
            if i.load:
                for r in i.regs:
                    if r < 15:
                        self.setreg(st, r, -1)
            if i.wb:
                self.setreg(st, i.rn, -1)
            return tval, other
        if op == 'mem':
            base = self.regval(st, i.rn) if i.rn != 15 else None
            if is_spv(base) and i.off[0] == 'imm' and i.msize == 4:
                sp_off = base - SPV
                ea = sp_off + (i.off[1] if i.up else -i.off[1]) if i.pre else sp_off
                if i.load:
                    src = st.slots.get(ea, -1)
                    if i.rd == 15:
                        tval = src
                    else:
                        if src != i.rd and 0 <= src < NREG:
                            other |= bit(src)
                        self.setreg(st, i.rd, src if src >= 0 else -1)
                else:
                    v = self.regval(st, i.rd) if i.rd < 15 else -1
                    if v is not None and 0 <= v < NREG:
                        st.slots[ea] = v
                    else:
                        st.slots.pop(ea, None)
                if i.wb and i.rn != 15:
                    nb = base + (i.off[1] if i.up else -i.off[1])
                    if not (i.load and i.rd == i.rn):
                        self.setreg(st, i.rn, nb)
                return tval, other
            if i.load:
                if i.rd == 15:
                    tval = -1
                else:
                    self.setreg(st, i.rd, -1)
            if i.wb and i.rn != 15:
                self.setreg(st, i.rn, -1)
            return tval, other
        if op == 'dp':
            alu = i.alu
            u, d = use_def_basic(i)
            st.fl &= ~(d & FLAGS)
            if alu in ('tst', 'teq', 'cmp', 'cmn'):
                return tval, other
            newv = -1
            o2 = i.op2
            simple_reg = o2[0] == 'reg' and o2[2] == 'lsl' and o2[3] == 0
            if alu == 'mov' and simple_reg and o2[1] != 15:
                newv = self.regval(st, o2[1])
                if newv is None:
                    newv = -1
            elif alu in ('add', 'sub') and o2[0] == 'imm' and i.rn != 15:
                bv = self.regval(st, i.rn)
                if is_spv(bv):
                    newv = bv + (o2[1] if alu == 'add' else -o2[1])
            elif alu in ('add', 'sub') and simple_reg and o2[1] in st.cst and i.rn != 15:
                bv = self.regval(st, i.rn)
                k = st.cst[o2[1]]
                if k & 0x80000000:
                    k -= 1 << 32
                if is_spv(bv):
                    newv = bv + (k if alu == 'add' else -k)
            if i.rd == 15:
                return newv, other
            self.setreg(st, i.rd, newv)
            return tval, other
        if op == 'bx':
            return (self.regval(st, i.rm) if i.rm != 15 else -1), other
        u, d = use_def_basic(i)
        for r in range(NREG):
            if d & bit(r):
                self.setreg(st, r, -1)
        st.fl &= ~(d & FLAGS)
        return tval, other

    # ------------------------------------------------------------------
    def live_in(self, g, mod, saved, unknown):
        ins = g.ins
        addrs = g.addrs
        exit_use = ALLREGS | FLAGS if unknown else mod
        live = {a: 0 for a in addrs}
        use_tab = {}
        for a in addrs:
            i = ins[a]
            k = g.kind[a]
            ce = self.call_effect(g, i)
            if ce is not None:
                cin, cmod = ce
                u = cin | COND_FLAGS[i.cond]
                d = cmod
                if k in ('call', 'ccall'):
                    u &= ~bit(14)
                    d |= bit(14)
                elif k == 'icall':
                    u |= bit(14)
            else:
                u, d = use_def_basic(i)
                if not (i.op == 'ldm' and i.load) and i.op == 'ldm' and i.rn == 13:
                    u &= ~saved
                elif i.op == 'mem' and not i.load and i.rn == 13 and i.rd < 15:
                    u &= ~(saved & bit(i.rd))
            if k in ('tail', 'ctail'):
                if k == 'tail':
                    tkey = (i.target, g.thumb)
                else:
                    t, tt, _ = g.const_branch[a]
                    tkey = (t, tt)
                tin, tmod = self.callee_summary(tkey)
                u |= tin | (exit_use & ~tmod) | bit(14)
            elif k == 'indirect' and not g.jumptables.get(a):
                u |= self.ind_in
            use_tab[a] = (u, d)
        dlive = exit_use | bit(14)
        order = list(reversed(addrs))
        changed = True
        while changed:
            changed = False
            nd = exit_use | bit(14)
            for r in g.resume:
                nd |= live.get(r, 0)
            if nd != dlive:
                dlive = nd
                changed = True
            for a in order:
                out = 0
                for s in g.succ[a]:
                    out |= dlive if s == 'D' else live[s]
                u, d = use_tab[a]
                if ins[a].cond != COND_AL:
                    n = out | u
                else:
                    n = (out & ~d) | u
                if n != live[a]:
                    live[a] = n
                    changed = True
        return live.get(g.addr, 0), live, dlive

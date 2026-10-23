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
    __slots__ = ('sp', 'val', 'fl', 'slots')

    def __init__(self, sp=0, val=None, fl=FLAGS, slots=None):
        self.sp = sp                                   # sp - sp_entry or None
        self.val = list(range(NREG)) if val is None else val
        self.fl = fl                                   # flags still at entry value
        self.slots = {} if slots is None else slots    # sp offset -> entry reg id

    def copy(self):
        return State(self.sp, list(self.val), self.fl, dict(self.slots))

    def merge(self, o):
        sp = self.sp if self.sp == o.sp else None
        val = [a if a == b else -1 for a, b in zip(self.val, o.val)]
        fl = self.fl & o.fl
        slots = {k: v for k, v in self.slots.items() if o.slots.get(k) == v}
        ns = State(sp, val, fl, slots)
        ch = (sp != self.sp or val != self.val or fl != self.fl or slots != self.slots)
        return ns, ch

    def modified(self):
        m = 0
        for r in range(NREG):
            if self.val[r] != r:
                m |= bit(r)
        if self.sp != 0:
            m |= bit(13)
        return m | (FLAGS & ~self.fl)


# value ids >= 100 encode "sp_entry + offset" for frame pointers
SPV = 1000000


def is_spv(v):
    return v is not None and v >= SPV // 2


class Summaries:
    def __init__(self, gens):
        self.gens = gens
        self.IN = {k: 0 for k in gens}
        self.MOD = {k: 0 for k in gens}
        self.saved = {k: 0 for k in gens}
        self.returns = {k: set() for k in gens}   # insn addrs proven to be returns

    def callee_summary(self, key):
        if key in self.gens:
            return self.IN[key], self.MOD[key]
        return ALLREGS | FLAGS, ALLREGS | FLAGS

    def run(self, max_iter=100):
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
            return ALLREGS | FLAGS, ALLREGS | FLAGS
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
        while work:
            a = work.pop()
            visits += 1
            if visits > 400000:
                return ALLREGS | FLAGS, ALLREGS | FLAGS, 0, set()
            i = ins[a]
            k = g.kind[a]
            st = st_in[a].copy()
            before = st.copy() if i.cond != COND_AL else None
            ce = self.call_effect(g, i)
            target_val = None
            if ce is not None:
                _, cmod = ce
                self.clobber(st, cmod)
                if k in ('call', 'ccall', 'icall'):
                    st.val[14] = -1
            else:
                target_val, ol = self.step(st, i)
                other_loads |= ol
            if before is not None:
                st, _ = st.merge(before)
            if k == 'indirect':
                if not g.jumptables.get(a):
                    if target_val == 14 and st.sp == 0:
                        rets.add(a)
                        mod |= st.modified()
                    else:
                        unknown = True
            elif k in ('tail', 'ctail'):
                if k == 'tail':
                    tkey = (i.target, g.thumb)
                else:
                    t, tt, _ = g.const_branch[a]
                    tkey = (t, tt)
                tin, tmod = self.callee_summary(tkey)
                if st.val[14] != 14:
                    unknown = True     # callee will not return to our caller
                mod |= st.modified() | tmod
            for s in g.succ[a]:
                if s == 'D':
                    continue
                if s not in st_in:
                    st_in[s] = st.copy()
                    work.append(s)
                else:
                    ns, ch = st_in[s].merge(st)
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
        inn = self.live_in(g, mod, saved, unknown)
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
        return live.get(g.addr, 0)

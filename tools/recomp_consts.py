"""Register/flag masks and per-instruction use/def sets shared by the
recompiler passes."""
from armdecode import COND_AL  # noqa: F401  (re-exported)

FLAG_N, FLAG_Z, FLAG_C, FLAG_V = 1 << 16, 1 << 17, 1 << 18, 1 << 19
FLAGS = FLAG_N | FLAG_Z | FLAG_C | FLAG_V
ALLREGS = (1 << 15) - 1          # r0..r14
LOGICAL = {'and', 'eor', 'tst', 'teq', 'orr', 'mov', 'bic', 'mvn'}
TESTS = {'tst', 'teq', 'cmp', 'cmn'}

COND_FLAGS = {
    0: FLAG_Z, 1: FLAG_Z, 2: FLAG_C, 3: FLAG_C, 4: FLAG_N, 5: FLAG_N,
    6: FLAG_V, 7: FLAG_V, 8: FLAG_C | FLAG_Z, 9: FLAG_C | FLAG_Z,
    10: FLAG_N | FLAG_V, 11: FLAG_N | FLAG_V, 12: FLAG_Z | FLAG_N | FLAG_V,
    13: FLAG_Z | FLAG_N | FLAG_V, 14: 0, 15: 0,
}


def bit(r):
    return 1 << r


def regs_of(mask):
    return [r for r in range(15) if mask & bit(r)]


def op2_uses(o):
    if o is None or o[0] == 'imm':
        return 0
    m = bit(o[1]) if o[1] != 15 else 0
    if o[0] == 'regsh' and o[3] != 15:
        m |= bit(o[3])
    return m


def shifter_carry_defined(o):
    if o[0] == 'imm':
        return o[2] is not None
    if o[0] == 'reg':
        return not (o[2] == 'lsl' and o[3] == 0)
    return True


def use_def_basic(ins):
    """(use, def) masks of one instruction ignoring call semantics."""
    use = COND_FLAGS[ins.cond]
    d = 0
    op = ins.op
    if op == 'dp':
        alu = ins.alu
        if alu not in ('mov', 'mvn') and ins.rn != 15:
            use |= bit(ins.rn)
        use |= op2_uses(ins.op2)
        if alu in ('adc', 'sbc', 'rsc'):
            use |= FLAG_C
        if ins.op2[0] == 'reg' and ins.op2[2] == 'rrx':
            use |= FLAG_C
        if ins.s:
            if alu in LOGICAL:
                d |= FLAG_N | FLAG_Z
                if shifter_carry_defined(ins.op2):
                    d |= FLAG_C
                    if ins.op2[0] == 'regsh':
                        use |= FLAG_C
            else:
                d |= FLAGS
        if alu not in TESTS and ins.rd != 15:
            d |= bit(ins.rd)
    elif op == 'mul':
        use |= bit(ins.rm) | bit(ins.rs)
        if ins.acc:
            use |= bit(ins.rn)
        d |= bit(ins.rd)
        if ins.s:
            d |= FLAG_N | FLAG_Z
    elif op == 'mull':
        use |= bit(ins.rm) | bit(ins.rs)
        if ins.acc:
            use |= bit(ins.rd) | bit(ins.rdhi)
        d |= bit(ins.rd) | bit(ins.rdhi)
        if ins.s:
            d |= FLAG_N | FLAG_Z
    elif op == 'mem':
        if ins.rn != 15:
            use |= bit(ins.rn)
        if ins.off[0] == 'reg':
            use |= bit(ins.off[1])
            if ins.off[2] == 'rrx':
                use |= FLAG_C
        if ins.load:
            if ins.rd != 15:
                d |= bit(ins.rd)
        elif ins.rd != 15:
            use |= bit(ins.rd)
        if ins.wb and ins.rn != 15:
            d |= bit(ins.rn)
    elif op == 'ldm':
        use |= bit(ins.rn)
        if ins.load:
            for r in ins.regs:
                if r != 15:
                    d |= bit(r)
        else:
            for r in ins.regs:
                if r != 15:
                    use |= bit(r)
        if ins.wb:
            d |= bit(ins.rn)
    elif op == 'bx':
        if ins.rm != 15:
            use |= bit(ins.rm)
    elif op == 'swp':
        use |= bit(ins.rn) | bit(ins.rm)
        d |= bit(ins.rd)
    elif op == 'mrs':
        use |= FLAGS
        d |= bit(ins.rd)
    elif op == 'msr':
        use |= op2_uses(ins.op2)
        if ins.mask & 8 and not ins.spsr:
            d |= FLAGS
    return use, d

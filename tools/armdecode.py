"""ARMv4T (ARM7TDMI) decoder for ARM and Thumb code.

Both instruction sets decode to one small IR (class Insn) so the analyzer and
the C code generator only deal with ARM semantics.  Thumb instructions are
expressed as their ARM equivalents (e.g. Thumb `lsl r0, r1, #2` becomes
MOVS r0, r1 LSL #2).
"""
import struct

COND_AL = 14
ALU = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc',
       'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn']
SHIFT = ['lsl', 'lsr', 'asr', 'ror']
CONDS = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc',
         'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', 'nv']


class Insn:
    """One decoded instruction.

    op is one of: dp mul mull mem ldm b bl bx swi mrs msr swp undef blh
    Operand2 / offsets are tuples:
      ('imm', value, carry)          carry: None (unchanged) or 0/1
      ('reg', rm, shtype, amount)    shtype in lsl lsr asr ror rrx, amount 0..32
      ('regsh', rm, shtype, rs)      shift by register
    """
    __slots__ = ('addr', 'size', 'thumb', 'cond', 'op', 'alu', 's', 'rd', 'rn',
                 'op2', 'off', 'pre', 'up', 'wb', 'msize', 'signed', 'load',
                 'regs', 'user', 'target', 'rm', 'rs', 'num', 'spsr', 'mask',
                 'acc', 'sgn', 'rdhi', 'pcval', 'raw')

    def __init__(self, addr, size, thumb):
        self.addr = addr
        self.size = size
        self.thumb = thumb
        self.cond = COND_AL
        self.op = 'undef'
        self.s = False
        self.rd = self.rn = self.rm = self.rs = None
        self.op2 = self.off = None
        self.pre = self.up = self.wb = False
        self.msize = 4
        self.signed = False
        self.load = False
        self.regs = ()
        self.user = False
        self.target = None
        self.num = 0
        self.spsr = False
        self.mask = 0
        self.acc = False
        self.sgn = False
        self.rdhi = None
        # value read from r15 by this instruction
        self.pcval = addr + (4 if thumb else 8)
        self.raw = 0

    @property
    def next(self):
        return self.addr + self.size

    def writes_pc(self):
        if self.op == 'dp':
            return self.rd == 15 and self.alu not in ('tst', 'teq', 'cmp', 'cmn')
        if self.op == 'mem':
            return self.load and self.rd == 15
        if self.op == 'ldm':
            return self.load and 15 in self.regs
        return self.op in ('b', 'bx')

    def __repr__(self):
        return '<%08x %s%s %s>' % (self.addr, 'T' if self.thumb else 'A',
                                   self.size, fmt(self))


def _op2s(o):
    if o is None:
        return ''
    if o[0] == 'imm':
        return '#0x%x' % o[1]
    if o[0] == 'reg':
        if o[2] == 'lsl' and o[3] == 0:
            return 'r%d' % o[1]
        if o[2] == 'rrx':
            return 'r%d, rrx' % o[1]
        return 'r%d, %s #%d' % (o[1], o[2], o[3])
    return 'r%d, %s r%d' % (o[1], o[2], o[3])


def fmt(i):
    c = CONDS[i.cond]
    if i.op == 'dp':
        return '%s%s%s r%s, r%s, %s' % (i.alu, c, 's' if i.s else '', i.rd, i.rn, _op2s(i.op2))
    if i.op == 'mem':
        nm = ('ldr' if i.load else 'str') + {1: 'b', 2: 'h', 4: ''}[i.msize] + ('s' if i.signed else '')
        return '%s%s r%d, [r%d %s%s%s]%s' % (nm, c, i.rd, i.rn, '' if i.up else '-', _op2s(i.off),
                                            '' if i.pre else ' post', '!' if i.wb else '')
    if i.op == 'ldm':
        return '%s%s r%d%s {%s}%s' % ('ldm' if i.load else 'stm', c, i.rn, '!' if i.wb else '',
                                     ','.join('r%d' % r for r in i.regs), '^' if i.user else '')
    if i.op in ('b', 'bl'):
        return '%s%s 0x%x' % (i.op, c, i.target)
    if i.op == 'bx':
        return 'bx%s r%d' % (c, i.rm)
    if i.op == 'swi':
        return 'swi 0x%x' % i.num
    return i.op


def ror32(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xffffffff if n else v


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


# ---------------------------------------------------------------- ARM

def decode_arm(word, addr):
    i = Insn(addr, 4, False)
    i.raw = word
    cond = word >> 28
    i.cond = cond
    if cond == 15:
        return i  # undefined on ARMv4
    b27_25 = (word >> 25) & 7

    if (word & 0x0ffffff0) == 0x012fff10:
        i.op = 'bx'
        i.rm = word & 15
        return i
    if b27_25 == 0 and (word & 0x90) == 0x90:
        # multiply / swap / halfword transfer
        op = (word >> 5) & 3
        if op == 0:
            if (word >> 23) & 0x1f == 0:  # MUL/MLA
                i.op = 'mul'
                i.acc = bool(word & (1 << 21))
                i.s = bool(word & (1 << 20))
                i.rd = (word >> 16) & 15
                i.rn = (word >> 12) & 15
                i.rs = (word >> 8) & 15
                i.rm = word & 15
                return i
            if (word >> 23) & 0x1f == 1:  # long multiply
                i.op = 'mull'
                i.sgn = bool(word & (1 << 22))
                i.acc = bool(word & (1 << 21))
                i.s = bool(word & (1 << 20))
                i.rdhi = (word >> 16) & 15
                i.rd = (word >> 12) & 15  # rdlo
                i.rs = (word >> 8) & 15
                i.rm = word & 15
                return i
            if (word & 0x0fb00ff0) == 0x01000090:
                i.op = 'swp'
                i.msize = 1 if word & (1 << 22) else 4
                i.rn = (word >> 16) & 15
                i.rd = (word >> 12) & 15
                i.rm = word & 15
                return i
            return i
        # halfword / signed transfers
        i.op = 'mem'
        i.pre = bool(word & (1 << 24))
        i.up = bool(word & (1 << 23))
        immf = bool(word & (1 << 22))
        i.wb = bool(word & (1 << 21)) or not i.pre
        i.load = bool(word & (1 << 20))
        i.rn = (word >> 16) & 15
        i.rd = (word >> 12) & 15
        if immf:
            i.off = ('imm', ((word >> 4) & 0xf0) | (word & 15), None)
        else:
            i.off = ('reg', word & 15, 'lsl', 0)
        if op == 1:
            i.msize = 2
        elif op == 2:
            i.msize, i.signed = 1, True
        else:
            i.msize, i.signed = 2, True
        if not i.load and op != 1:
            i.op = 'undef'
        return i
    if b27_25 in (0, 1):
        opc = (word >> 21) & 15
        s = bool(word & (1 << 20))
        if not s and 8 <= opc <= 11:
            # PSR transfer
            spsr = bool(word & (1 << 22))
            if (word & 0x0fbf0fff) == 0x010f0000:
                i.op = 'mrs'
                i.spsr = spsr
                i.rd = (word >> 12) & 15
                return i
            if (word & 0x0db0f000) == 0x0120f000:
                i.op = 'msr'
                i.spsr = spsr
                i.mask = (word >> 16) & 15
                if word & (1 << 25):
                    i.op2 = ('imm', ror32(word & 0xff, ((word >> 8) & 15) * 2), None)
                else:
                    i.op2 = ('reg', word & 15, 'lsl', 0)
                return i
            return i
        i.op = 'dp'
        i.alu = ALU[opc]
        i.s = s
        i.rn = (word >> 16) & 15
        i.rd = (word >> 12) & 15
        if word & (1 << 25):
            rot = ((word >> 8) & 15) * 2
            v = ror32(word & 0xff, rot)
            i.op2 = ('imm', v, (v >> 31) if rot else None)
        else:
            rm = word & 15
            st = SHIFT[(word >> 5) & 3]
            if word & 0x10:
                i.op2 = ('regsh', rm, st, (word >> 8) & 15)
                i.pcval = addr + 12
            else:
                amt = (word >> 7) & 31
                if amt == 0:
                    if st in ('lsr', 'asr'):
                        amt = 32
                    elif st == 'ror':
                        st = 'rrx'
                        amt = 1
                i.op2 = ('reg', rm, st, amt)
        return i
    if b27_25 in (2, 3):
        if b27_25 == 3 and word & 0x10:
            return i  # undefined
        i.op = 'mem'
        i.pre = bool(word & (1 << 24))
        i.up = bool(word & (1 << 23))
        i.msize = 1 if word & (1 << 22) else 4
        w = bool(word & (1 << 21))
        i.wb = w or not i.pre
        if not i.pre and w:
            i.user = True  # ldrt/strt; treated as normal access
        i.load = bool(word & (1 << 20))
        i.rn = (word >> 16) & 15
        i.rd = (word >> 12) & 15
        if b27_25 == 2:
            i.off = ('imm', word & 0xfff, None)
        else:
            rm = word & 15
            st = SHIFT[(word >> 5) & 3]
            amt = (word >> 7) & 31
            if amt == 0:
                if st in ('lsr', 'asr'):
                    amt = 32
                elif st == 'ror':
                    st, amt = 'rrx', 1
            i.off = ('reg', rm, st, amt)
        return i
    if b27_25 == 4:
        i.op = 'ldm'
        i.pre = bool(word & (1 << 24))
        i.up = bool(word & (1 << 23))
        i.user = bool(word & (1 << 22))
        i.wb = bool(word & (1 << 21))
        i.load = bool(word & (1 << 20))
        i.rn = (word >> 16) & 15
        i.regs = tuple(r for r in range(16) if word & (1 << r))
        return i
    if b27_25 == 5:
        i.op = 'bl' if word & (1 << 24) else 'b'
        i.target = (addr + 8 + (sext(word & 0xffffff, 24) << 2)) & 0xffffffff
        return i
    if b27_25 == 7 and (word >> 24) & 15 == 15:
        i.op = 'swi'
        i.num = (word >> 16) & 0xff
        i.raw = word
        return i
    return i


# ---------------------------------------------------------------- Thumb

def _dp(i, alu, s, rd, rn, op2):
    i.op = 'dp'
    i.alu = alu
    i.s = s
    i.rd = rd
    i.rn = rn
    i.op2 = op2
    return i


def _reg(r):
    return ('reg', r, 'lsl', 0)


def _imm(v):
    return ('imm', v, None)


def decode_thumb(h, addr, h2=None):
    """Decode a Thumb halfword.  h2 is the following halfword (for BL pairs)."""
    i = Insn(addr, 2, True)
    i.raw = h
    top = h >> 13
    if top == 0:
        op = (h >> 11) & 3
        rs = (h >> 3) & 7
        rd = h & 7
        if op < 3:
            amt = (h >> 6) & 31
            st = SHIFT[op]
            if amt == 0 and op != 0:
                amt = 32
            return _dp(i, 'mov', True, rd, 0, ('reg', rs, st, amt))
        sub = bool(h & (1 << 9))
        o = _imm((h >> 6) & 7) if h & (1 << 10) else _reg((h >> 6) & 7)
        return _dp(i, 'sub' if sub else 'add', True, rd, rs, o)
    if top == 1:
        op = (h >> 11) & 3
        rd = (h >> 8) & 7
        v = h & 0xff
        alu = ['mov', 'cmp', 'add', 'sub'][op]
        return _dp(i, alu, True, rd, rd, _imm(v))
    if (h >> 10) == 0x10:
        op = (h >> 6) & 15
        rs = (h >> 3) & 7
        rd = h & 7
        if op in (2, 3, 4, 7):
            st = {2: 'lsl', 3: 'lsr', 4: 'asr', 7: 'ror'}[op]
            return _dp(i, 'mov', True, rd, 0, ('regsh', rd, st, rs))
        if op == 9:
            return _dp(i, 'rsb', True, rd, rs, _imm(0))
        if op == 13:
            i.op = 'mul'
            i.s = True
            i.rd = rd
            i.rm = rs
            i.rs = rd
            i.rn = 0
            return i
        alu = {0: 'and', 1: 'eor', 5: 'adc', 6: 'sbc', 8: 'tst', 10: 'cmp',
               11: 'cmn', 12: 'orr', 14: 'bic', 15: 'mvn'}[op]
        return _dp(i, alu, True, rd, rd, _reg(rs))
    if (h >> 10) == 0x11:
        op = (h >> 8) & 3
        rs = ((h >> 3) & 7) | ((h >> 3) & 8)
        rd = (h & 7) | ((h >> 4) & 8)
        if op == 0:
            return _dp(i, 'add', False, rd, rd, _reg(rs))
        if op == 1:
            return _dp(i, 'cmp', True, rd, rd, _reg(rs))
        if op == 2:
            return _dp(i, 'mov', False, rd, 0, _reg(rs))
        i.op = 'bx'
        i.rm = rs
        return i
    if (h >> 11) == 9:
        i.op = 'mem'
        i.load = True
        i.rd = (h >> 8) & 7
        i.rn = 15
        i.pcval = (addr + 4) & ~3
        i.off = _imm((h & 0xff) << 2)
        i.pre = i.up = True
        return i
    if (h >> 12) == 5:
        ro = (h >> 6) & 7
        rb = (h >> 3) & 7
        rd = h & 7
        i.op = 'mem'
        i.pre = i.up = True
        i.rn = rb
        i.rd = rd
        i.off = _reg(ro)
        op = (h >> 9) & 7
        # 0 str 1 strh 2 strb 3 ldrsb 4 ldr 5 ldrh 6 ldrb 7 ldrsh
        i.load = op >= 3
        i.msize = [4, 2, 1, 1, 4, 2, 1, 2][op]
        i.signed = op in (3, 7)
        return i
    if top == 3:
        b = bool(h & (1 << 12))
        i.op = 'mem'
        i.pre = i.up = True
        i.load = bool(h & (1 << 11))
        i.rn = (h >> 3) & 7
        i.rd = h & 7
        off = (h >> 6) & 31
        i.msize = 1 if b else 4
        i.off = _imm(off if b else off << 2)
        return i
    if (h >> 12) == 8:
        i.op = 'mem'
        i.pre = i.up = True
        i.load = bool(h & (1 << 11))
        i.rn = (h >> 3) & 7
        i.rd = h & 7
        i.msize = 2
        i.off = _imm(((h >> 6) & 31) << 1)
        return i
    if (h >> 12) == 9:
        i.op = 'mem'
        i.pre = i.up = True
        i.load = bool(h & (1 << 11))
        i.rn = 13
        i.rd = (h >> 8) & 7
        i.off = _imm((h & 0xff) << 2)
        return i
    if (h >> 12) == 10:
        rd = (h >> 8) & 7
        if h & (1 << 11):
            return _dp(i, 'add', False, rd, 13, _imm((h & 0xff) << 2))
        _dp(i, 'add', False, rd, 15, _imm((h & 0xff) << 2))
        i.pcval = (addr + 4) & ~3
        return i
    if (h >> 8) == 0xb0:
        v = (h & 0x7f) << 2
        return _dp(i, 'sub' if h & 0x80 else 'add', False, 13, 13, _imm(v))
    if (h >> 12) == 11 and ((h >> 9) & 3) == 2:
        load = bool(h & (1 << 11))
        regs = [r for r in range(8) if h & (1 << r)]
        if h & 0x100:
            regs.append(15 if load else 14)
        i.op = 'ldm'
        i.load = load
        i.rn = 13
        i.wb = True
        i.regs = tuple(regs)
        if load:
            i.pre, i.up = False, True
        else:
            i.pre, i.up = True, False
        return i
    if (h >> 12) == 12:
        i.op = 'ldm'
        i.load = bool(h & (1 << 11))
        i.rn = (h >> 8) & 7
        i.pre, i.up = False, True
        i.regs = tuple(r for r in range(8) if h & (1 << r))
        # Thumb ldmia with rn in list: no writeback
        i.wb = not (i.load and i.rn in i.regs)
        return i
    if (h >> 12) == 13:
        cond = (h >> 8) & 15
        if cond == 15:
            i.op = 'swi'
            i.num = h & 0xff
            return i
        if cond == 14:
            return i
        i.op = 'b'
        i.cond = cond
        i.target = (addr + 4 + (sext(h & 0xff, 8) << 1)) & 0xffffffff
        return i
    if (h >> 11) == 0x1c:
        i.op = 'b'
        i.target = (addr + 4 + (sext(h & 0x7ff, 11) << 1)) & 0xffffffff
        return i
    if (h >> 11) == 0x1e:
        if h2 is not None and (h2 >> 11) == 0x1f:
            hi = sext(h & 0x7ff, 11) << 12
            lo = (h2 & 0x7ff) << 1
            i.op = 'bl'
            i.size = 4
            i.target = (addr + 4 + hi + lo) & 0xffffffff
            i.raw = h | (h2 << 16)
            return i
        i.op = 'blprefix'
        i.num = sext(h & 0x7ff, 11) << 12
        return i
    if (h >> 11) == 0x1f:
        # lone BL suffix: pc = lr + off, lr = next|1
        i.op = 'blh'
        i.num = (h & 0x7ff) << 1
        return i
    return i


class Rom:
    BASE = 0x08000000

    def __init__(self, path):
        self.data = open(path, 'rb').read()

    def in_rom(self, a):
        return self.BASE <= a < self.BASE + len(self.data)

    def u16(self, a):
        return struct.unpack_from('<H', self.data, a - self.BASE)[0]

    def u32(self, a):
        return struct.unpack_from('<I', self.data, a - self.BASE)[0]


class CodeSpace:
    """Maps guest addresses to bytes: ROM plus RAM regions copied from ROM."""

    def __init__(self, rom):
        self.rom = rom
        self.overlays = []  # (dst, size, rom_src)

    def add_overlay(self, dst, size, src):
        self.overlays.append((dst, size, src))

    def phys(self, a):
        """Return the ROM address holding the bytes for guest address a."""
        if self.rom.in_rom(a):
            return a
        if 0x0a000000 <= a < 0x0e000000:  # ROM mirrors (wait states 1/2)
            m = 0x08000000 + (a & 0x01ffffff)
            return m if self.rom.in_rom(m) else None
        for dst, size, src in self.overlays:
            if dst <= a < dst + size:
                return src + (a - dst)
        return None

    def valid(self, a, n=2):
        p = self.phys(a)
        return p is not None and self.rom.in_rom(p + n - 1)

    def u16(self, a):
        return self.rom.u16(self.phys(a))

    def u32(self, a):
        return self.rom.u32(self.phys(a))

    def decode(self, a, thumb):
        if thumb:
            h = self.u16(a)
            h2 = self.u16(a + 2) if self.valid(a + 2) else None
            return decode_thumb(h, a, h2)
        return decode_arm(self.u32(a), a)

#!/usr/bin/env python3
"""
Generate synthetic GBA display states (GBAS v1, see platform/host/main.c) that
exercise renderer features the game captures do not cover.

    python3 mksynth.py OUTDIR      -> OUTDIR/synth_*.bin

Every state is pure test data (random tiles/palettes from a fixed seed).
"""
import math
import os
import random
import struct
import sys


class St:
    def __init__(self):
        self.io = bytearray(0x400)
        self.pal = bytearray(0x400)
        self.vram = bytearray(0x18000)
        self.oam = bytearray(0x400)
        self.lrv = 0
        self.lines = [[256, 0, 0, 256, 0, 0, [0] * 4, [0] * 4] for _ in range(160)]
        for i in range(128):
            self.obj(i, 0x0200, 0, 0)          # disabled

    def r(self, off, v): struct.pack_into('<H', self.io, off, v & 0xFFFF)
    def r32(self, off, v): struct.pack_into('<I', self.io, off, v & 0xFFFFFFFF)
    def c(self, i, v): struct.pack_into('<H', self.pal, i * 2, v & 0x7FFF)
    def v16(self, o, v): struct.pack_into('<H', self.vram, o, v & 0xFFFF)
    def obj(self, i, a0, a1, a2): struct.pack_into('<3H', self.oam, i * 8, a0 & 0xFFFF, a1 & 0xFFFF, a2 & 0xFFFF)

    def aff(self, g, pa, pb, pc, pd):
        for k, v in enumerate((pa, pb, pc, pd)):
            struct.pack_into('<h', self.oam, g * 32 + 6 + 8 * k, int(v))

    def save(self, path):
        out = bytearray(b'GBAS') + struct.pack('<I', 1)
        out += self.io + self.pal + self.vram + self.oam + struct.pack('<I', self.lrv)
        for (pa, pb, pc, pd, x, y, h, v) in self.lines:
            out += struct.pack('<4H2I', pa & 0xFFFF, pb & 0xFFFF, pc & 0xFFFF, pd & 0xFFFF, x & 0xFFFFFFFF, y & 0xFFFFFFFF)
            out += struct.pack('<4H', *[a & 0xFFFF for a in h]) + struct.pack('<4H', *[a & 0xFFFF for a in v])
        open(path, 'wb').write(out)


R = random.Random(1234)


def rnd_pal(s, lo=0, hi=512, bright=False):
    for i in range(lo, hi):
        if bright:
            s.c(i, R.randrange(16, 32) | (R.randrange(16, 32) << 5) | (R.randrange(16, 32) << 10))
        else:
            s.c(i, R.randrange(0x8000))


def tiles8(s, addr, n, transp=0.3, lo=1, hi=256):
    for t in range(n):
        for k in range(64):
            s.vram[addr + t * 64 + k] = 0 if R.random() < transp else R.randrange(lo, hi)


def tiles4(s, addr, n, transp=0.3):
    for t in range(n):
        for k in range(32):
            a = 0 if R.random() < transp else R.randrange(1, 16)
            b = 0 if R.random() < transp else R.randrange(1, 16)
            s.vram[addr + t * 32 + k] = a | (b << 4)


def text_map(s, sb, size, ntiles, flips=True, banks=False):
    nblk = (1, 2, 2, 4)[size]
    for o in range(nblk * 0x400):
        e = R.randrange(ntiles)
        if flips:
            e |= R.randrange(4) << 10
        if banks:
            e |= R.randrange(16) << 12
        s.v16(sb * 0x800 + o * 2, e)


def obj_vram_random(s, transp=0.3):
    for k in range(0x8000):
        s.vram[0x10000 + k] = 0 if R.random() < transp else R.randrange(1, 256)


SHAPES = [(0, 0), (0, 1), (0, 2), (0, 3), (1, 0), (1, 1), (1, 2), (1, 3), (2, 0), (2, 1), (2, 2), (2, 3)]
DIMS = {(0, 0): (8, 8), (0, 1): (16, 16), (0, 2): (32, 32), (0, 3): (64, 64), (1, 0): (16, 8), (1, 1): (32, 8),
        (1, 2): (32, 16), (1, 3): (64, 32), (2, 0): (8, 16), (2, 1): (8, 32), (2, 2): (16, 32), (2, 3): (32, 64)}


def background(s, bg=3, prio=3, sb=31, cb=0, ntiles=64, tile0=0):
    """opaque 8bpp text BG"""
    tiles8(s, cb * 0x4000 + tile0 * 64, ntiles, transp=0.0)
    for o in range(0x400):
        s.v16(sb * 0x800 + o * 2, tile0 + R.randrange(ntiles) | (R.randrange(4) << 10))
    s.r(0x08 + 2 * bg, 0x80 | (cb << 2) | (sb << 8) | prio)


def st_bgtext():
    s = St()
    rnd_pal(s)
    tiles8(s, 0x0000, 128, transp=0.3)
    tiles4(s, 0x4000, 64, transp=0.3)
    tiles4(s, 0x8000, 64, transp=0.7)
    # BG0 4bpp cb1 sb28 256x256 prio1, BG1 8bpp cb0 sb20 512x256 prio2,
    # BG2 4bpp cb2 sb22 256x512 prio0, BG3 8bpp cb0 sb24 512x512 prio3 opaque
    text_map(s, 28, 0, 64, banks=True)
    text_map(s, 20, 1, 128)
    text_map(s, 22, 2, 64, banks=True)
    for o in range(0x1000):
        s.v16(24 * 0x800 + o * 2, (128 + R.randrange(16)) | (R.randrange(4) << 10))
    tiles8(s, 128 * 64, 16, transp=0.0)
    s.r(0x08, 0x0000 | (1 << 2) | (28 << 8) | 1)
    s.r(0x0A, 0x4080 | (0 << 2) | (20 << 8) | 2)
    s.r(0x0C, 0x8000 | (2 << 2) | (22 << 8) | 0)
    s.r(0x0E, 0xC080 | (0 << 2) | (24 << 8) | 3)
    for n, (h, v) in enumerate([(0x1234, 0x0567), (300, 77), (5, 400), (0xFFF0, 0x1F8)]):
        s.r(0x10 + 4 * n, h); s.r(0x12 + 4 * n, v)
    s.r(0x00, 0x0F40)
    return s


def sprites_grid(s, start=0, bpp4_banks=True, prio_cycle=True):
    i = start
    x, y = 2, 2
    unit = 0
    for k, sh in enumerate(SHAPES + SHAPES):
        w, h = DIMS[sh]
        bpp8 = k < len(SHAPES)
        flip = k % 4
        if x + w > 236:
            x = 2; y += 66
        a0 = (y & 0xFF) | (sh[0] << 14) | (0x2000 if bpp8 else 0)
        a1 = (x & 0x1FF) | (sh[1] << 14) | (flip << 12)
        prio = (k % 4) if prio_cycle else 0
        a2 = (unit & 0x3FF) | (prio << 10) | ((k % 16) << 12)
        s.obj(i, a0, a1, a2)
        unit += (w // 8) * (h // 8) * (2 if bpp8 else 1)
        x += w + 3
        i += 1
    return i


def st_obj(one_d=True):
    s = St()
    rnd_pal(s)
    background(s, bg=3, prio=2, sb=31)
    # BG1: partially transparent, prio 0 (above most sprites)
    tiles8(s, 64 * 64, 64, transp=0.8)
    for o in range(0x400):
        s.v16(30 * 0x800 + o * 2, 64 + R.randrange(64))
    s.r(0x0A, 0x80 | (30 << 8) | 0)
    obj_vram_random(s)
    i = sprites_grid(s)
    # wrap cases: y wraps (240), x negative (-16 via 9-bit 0x1F0)
    s.obj(i, 240 | (0 << 14) | 0x2000, 100 | (2 << 14), 40 | (1 << 10)); i += 1
    s.obj(i, 100 | 0x2000, 0x1F0 | (1 << 14), 80 | (0 << 10)); i += 1
    s.obj(i, 150 | (1 << 14), 220 | (3 << 14) | 0x1000, 700 | (0 << 10) | (5 << 12)); i += 1   # wraps y and clips right
    # overlapping sprites: OAM order and priority ties against BG
    for k in range(6):
        s.obj(i, (60 + k * 3) | 0x2000, (120 + k * 5) | (1 << 14), (200 + 4 * k) | ((k % 3) << 10)); i += 1
    s.r(0x00, 0x1A00 | (0x40 if one_d else 0))
    return s


def st_affobj():
    s = St()
    rnd_pal(s)
    background(s, bg=3, prio=3, sb=31)
    obj_vram_random(s, transp=0.2)
    cases = [
        # (shape,size, x,y, double, bpp8, matrix)
        ((0, 2), 4, 4, False, True, (171, 0, 0, 171)),          # uniform 1.5x
        ((0, 3), 40, 4, False, True, (512, 0, 0, 512)),         # 0.5x
        ((0, 2), 110, 4, True, True, 'rot30'),                  # rotation + double size
        ((1, 2), 180, 4, False, True, (128, 0, 0, 384)),        # non-uniform
        ((0, 2), 4, 80, False, True, (256, 0, 0, 256)),         # identity
        ((0, 2), 44, 80, False, True, 'rot45'),                 # rotation, clipped to box
        ((2, 2), 84, 80, False, True, (-256, 0, 0, 256)),       # mirror through matrix
        ((0, 2), 110, 70, True, False, 'rot60'),                # 4bpp rotation double
        ((0, 1), 180, 90, True, True, (256, 0, 0, 256)),        # identity but double size
        ((0, 2), 500, 250, True, True, 'rot20'),                # off left/top edge (x=-12, y=-6)
        ((0, 3), 150, 100, True, True, 'rot75'),                # 64x64 -> 128x128 box
        ((2, 3), 220, 120, False, True, (300, 40, -60, 200)),   # shear
    ]
    unit = 0
    for i, (sh, x, y, dbl, bpp8, m) in enumerate(cases):
        if isinstance(m, str):
            a = math.radians(int(m[3:]))
            sc = 0.9
            m = (int(math.cos(a) * 256 / sc), int(math.sin(a) * 256 / sc), int(-math.sin(a) * 256 / sc), int(math.cos(a) * 256 / sc))
        s.aff(i, *m)
        a0 = (y & 0xFF) | 0x100 | (0x200 if dbl else 0) | (0x2000 if bpp8 else 0) | (sh[0] << 14)
        a1 = (x & 0x1FF) | (i << 9) | (sh[1] << 14)
        a2 = (unit & 0x3FF) | ((i % 4) << 10) | ((i * 5 % 16) << 12)
        s.obj(i, a0, a1, a2)
        w, h = DIMS[sh]
        unit += (w // 8) * (h // 8) * (2 if bpp8 else 1)
    s.r(0x00, 0x1840)
    return s


def mode7_lines(s, angle=0.6):
    s.lrv = 1
    for ln in range(160):
        if ln < 56:
            pa, pc = 256, 0
            x, y = (ln * 37) << 8, (ln * 11) << 8
        else:
            z = 9000 / (ln - 40)
            sc = z / 60.0
            pa = int(math.cos(angle) * 256 * sc)
            pc = int(math.sin(angle) * 256 * sc)
            cx, cy = 300.0, 260.0
            dist = z * 2.5
            x = int(((cx + math.sin(angle) * dist) - 120 * pa / 256.0) * 256)
            y = int(((cy - math.cos(angle) * dist) - 120 * pc / 256.0) * 256)
        s.lines[ln][0] = pa & 0xFFFF
        s.lines[ln][2] = pc & 0xFFFF
        s.lines[ln][4] = x & 0x0FFFFFFF
        s.lines[ln][5] = y & 0x0FFFFFFF
        s.lines[ln][6] = [0, 3 * (ln // 10), 0, 0]      # BG1 scroll varies per band of lines
        s.lines[ln][7] = [0, 40, 0, 0]


def st_mode7():
    s = St()
    rnd_pal(s, 0, 256)
    rnd_pal(s, 1, 32, bright=True)
    tiles8(s, 0x0000, 64, transp=0.0, lo=32)     # affine tiles: opaque
    for o in range(64 * 64):                     # 512x512 affine map at sb30
        s.vram[30 * 0x800 + o] = R.randrange(64)
    tiles8(s, 64 * 64, 32, transp=0.6, lo=1, hi=32)    # BG0/BG1 tiles 64..95 (bright colours)
    for o in range(0x400):
        s.v16(28 * 0x800 + o * 2, 64 + R.randrange(32) if R.random() < 0.5 else 96)   # tile 96 is empty
        s.v16(29 * 0x800 + o * 2, 64 + R.randrange(32))
    s.r(0x08, 0x80 | (28 << 8) | 0)              # BG0 prio0
    s.r(0x0A, 0x80 | (29 << 8) | 1)              # BG1 prio1
    s.r(0x0C, 0xBE80 | 2)                        # BG2 affine 512 wrap sb30 prio2
    s.r(0x10 + 4, 0); s.r(0x12 + 4, 40)
    # live affine registers (used outside line snapshots / for reference)
    s.r(0x20, 256); s.r(0x22, 0); s.r(0x24, 0); s.r(0x26, 256)
    mode7_lines(s)
    obj_vram_random(s, transp=0.3)
    s.aff(0, 200, 0, 0, 200)
    s.obj(0, 90 | 0x100 | 0x2000, 100 | (0 << 9) | (2 << 14), 10 | (0 << 10))
    s.obj(1, 110 | 0x2000 | (1 << 14), 40 | (2 << 14), 80 | (1 << 10))
    s.r(0x50, 0x1641)
    s.r(0x52, 6 | (16 << 8))
    s.r(0x00, 0x1741)
    return s


def st_sat(eva=9, evb=16):
    s = St()
    rnd_pal(s, 0, 512)
    rnd_pal(s, 200, 256, bright=True)
    background(s, bg=3, prio=3, sb=31)
    tiles8(s, 64 * 64, 32, transp=0.5)
    for o in range(0x400):
        s.v16(30 * 0x800 + o * 2, 64 + R.randrange(32) | (R.randrange(4) << 10))
    s.r(0x0C, 0x80 | (30 << 8) | 2)              # BG2 partial prio2
    tiles8(s, 96 * 64, 32, transp=0.6, lo=200, hi=256)
    for o in range(0x400):
        s.v16(28 * 0x800 + o * 2, 96 + R.randrange(32) | (R.randrange(4) << 10))
    s.r(0x08, 0x80 | (28 << 8) | 0)              # BG0 first target, prio0
    obj_vram_random(s)
    sprites_grid(s, prio_cycle=False)
    for i in range(24):                          # sprites in prio 1 (below BG0)
        a2 = struct.unpack_from('<H', s.oam, i * 8 + 4)[0]
        struct.pack_into('<H', s.oam, i * 8 + 4, (a2 & ~0x0C00) | (1 << 10))
    s.r(0x50, 0x1E41)
    s.r(0x52, eva | (evb << 8))
    s.r(0x00, 0x1D40)
    return s


def st_weather(eva=16, evb=12, hole=False):
    """a solid full-screen first target with few colours (rain: red/green <= 7,
    blue up to 31) over 2nd targets, sprites below and above it, backdrop not a
    2nd target (with 'hole', BG3 has transparent tiles so it shows through)"""
    s = St()
    rnd_pal(s, 0, 512)
    for i, c in enumerate([(7, 7, 31), (0, 0, 31), (0, 0, 25), (0, 0, 15)]):
        s.c(48 + i, c[0] | (c[1] << 5) | (c[2] << 10))
    background(s, bg=3, prio=3, sb=31)
    if hole:
        tiles8(s, 60 * 64, 1, transp=1.0)        # tile 60: transparent
        for o in range(0, 0x400, 7):
            s.v16(31 * 0x800 + o * 2, 60)
    tiles8(s, 64 * 64, 32, transp=0.6)
    for o in range(0x400):
        s.v16(30 * 0x800 + o * 2, 64 + R.randrange(32) | (R.randrange(4) << 10))
    s.r(0x0C, 0x80 | (30 << 8) | 2)              # BG2 partial prio2
    tiles8(s, 96 * 64, 16, transp=0.0, lo=48, hi=52)
    for o in range(0x400):
        s.v16(28 * 0x800 + o * 2, 96 + R.randrange(16) | (R.randrange(4) << 10))
    s.r(0x08, 0x80 | (28 << 8) | 0)              # BG0 first target, solid, prio0
    obj_vram_random(s)
    sprites_grid(s, prio_cycle=False)
    for i in range(24):                          # sprites 0-11 above BG0 (prio 0), 12-23 below
        a2 = struct.unpack_from('<H', s.oam, i * 8 + 4)[0]
        struct.pack_into('<H', s.oam, i * 8 + 4, (a2 & ~0x0C00) | ((0 if i < 12 else 1) << 10))
    s.r(0x50, 0x1E41)                            # BG0 1st, BG1-3 + OBJ 2nd, backdrop not
    s.r(0x52, eva | (evb << 8))
    s.r(0x00, 0x1D40)
    return s


def st_sunbeam(eva=9, evb=16):
    """a partial first target whose colours stay below 28 (sunbeams through a
    window) over a room, backdrop not a 2nd target around it"""
    s = St()
    rnd_pal(s, 0, 512)
    for i in range(16):
        g = 4 + i
        s.c(32 + i, g | (g << 5) | (g << 10))
    # BG3: a room in the middle of the screen (opaque tiles), backdrop around it
    tiles8(s, 0, 32, transp=0.0)
    for o in range(0x400):
        x, y = o & 31, o >> 5
        s.v16(31 * 0x800 + o * 2, (R.randrange(32) | (R.randrange(4) << 10)) if 4 <= x < 26 and 3 <= y < 17 else 63)
    tiles8(s, 63 * 64, 1, transp=1.0)
    s.r(0x0E, 0x80 | (31 << 8) | 3)
    # BG0: soft beams over part of the room and past its top-left corner
    tiles8(s, 64 * 64, 16, transp=0.4, lo=32, hi=48)
    tiles8(s, 80 * 64, 1, transp=1.0)
    for o in range(0x400):
        x, y = o & 31, o >> 5
        s.v16(28 * 0x800 + o * 2, (64 + R.randrange(16)) if 2 <= x < 16 and 1 <= y < 12 else 80)
    s.r(0x08, 0x80 | (28 << 8) | 1)
    obj_vram_random(s)
    sprites_grid(s, prio_cycle=False)
    for i in range(24):
        a2 = struct.unpack_from('<H', s.oam, i * 8 + 4)[0]
        struct.pack_into('<H', s.oam, i * 8 + 4, (a2 & ~0x0C00) | ((1 if i < 12 else 2) << 10))
    s.r(0x50, 0x1841)                            # BG0 1st; BG3 + OBJ 2nd
    s.r(0x52, eva | (evb << 8))
    s.r(0x00, 0x1940)
    return s


def st_dialog(empty=True):
    """the field with BLDCNT set for a first target that is empty (or has a
    few tiles in one corner), as the game does during dialogues"""
    s = St()
    rnd_pal(s, 0, 512)
    background(s, bg=3, prio=3, sb=31)
    tiles8(s, 64 * 64, 32, transp=0.5)
    for o in range(0x400):
        s.v16(30 * 0x800 + o * 2, 64 + R.randrange(32) | (R.randrange(4) << 10))
    s.r(0x0C, 0x80 | (30 << 8) | 2)
    tiles8(s, 96 * 64, 1, transp=1.0)            # tile 96: transparent
    tiles8(s, 97 * 64, 4, transp=0.3)
    for o in range(0x400):
        x, y = o & 31, o >> 5
        s.v16(28 * 0x800 + o * 2, 97 + (o & 3) if (not empty and 25 <= x < 29 and 15 <= y < 19) else 96)
    s.r(0x08, 0x80 | (28 << 8) | 1)
    obj_vram_random(s)
    sprites_grid(s, prio_cycle=False)
    s.r(0x50, 0x1E41)
    s.r(0x52, 14 | (6 << 8))
    s.r(0x00, 0x1D40)
    return s


def st_mode4(page=0, scaled=False):
    s = St()
    rnd_pal(s)
    base = 0xA000 if page else 0
    for o in range(240 * 160):
        s.vram[base + o] = 0 if R.random() < 0.1 else R.randrange(1, 256)
    obj_vram_random(s)
    # sprites at tiles < 512 (invisible in bitmap modes), >= 512, and straddling
    s.obj(0, 20 | 0x2000 | (0 << 14), 20 | (2 << 14), 100)
    s.obj(1, 20 | 0x2000 | (0 << 14), 80 | (2 << 14), 600)
    s.obj(2, 80 | (0 << 14), 140 | (2 << 14), 1000 | (3 << 12))
    s.obj(3, 80 | 0x2000 | (0 << 14), 30 | (3 << 14), 480)       # straddles tile 512
    if scaled:
        s.r(0x20, 200); s.r(0x22, 30); s.r(0x24, -20); s.r(0x26, 300)
        s.r32(0x28, 10 << 8); s.r32(0x2C, (-5) << 8)
    else:
        s.r(0x20, 256); s.r(0x26, 256)
    s.r(0x0C, 0x0000)
    s.r(0x00, 0x1444 | (0x10 if page else 0))
    return s


def st_mode3():
    s = St()
    for o in range(0, 240 * 160 * 2, 2):
        s.v16(o, R.randrange(0x8000))
    s.r(0x20, 256); s.r(0x26, 256)
    s.r(0x00, 0x0403)
    s.r(0x50, 0x00C4); s.r(0x54, 6)               # darken BG2
    return s


def st_effect(effect, l1, evy):
    s = st_bgtext()
    obj_vram_random(s)
    sprites_grid(s)
    s.r(0x00, 0x1F40)
    s.r(0x50, (effect << 6) | l1)
    s.r(0x54, evy)
    return s


def st_fade():
    s = st_bgtext()
    s.r(0x50, 0x3F5F)
    s.r(0x52, 9)
    return s


def st_cross():
    s = St()
    rnd_pal(s)
    background(s, bg=3, prio=1, sb=31)
    tiles8(s, 64 * 64, 64, transp=0.4)
    for o in range(0x400):
        s.v16(30 * 0x800 + o * 2, 64 + R.randrange(64))
    s.r(0x0C, 0x80 | (30 << 8) | 0)
    s.r(0x50, 0x0844)
    s.r(0x52, 9 | (7 << 8))
    s.r(0x00, 0x0C00)
    return s


def st_semi():
    s = St()
    rnd_pal(s)
    background(s, bg=3, prio=3, sb=31)
    obj_vram_random(s)
    n = sprites_grid(s, prio_cycle=False)
    for i in range(0, n, 2):                     # every other sprite semi-transparent
        a0 = struct.unpack_from('<H', s.oam, i * 8)[0]
        struct.pack_into('<H', s.oam, i * 8, a0 | (1 << 10))
    s.r(0x50, 0x0F00)                            # 2nd targets only, effect none
    s.r(0x52, 10 | (6 << 8))
    s.r(0x00, 0x1840)
    return s


def st_approx_bd():
    """BG0 (1st target) partly directly over the backdrop, which is not a 2nd
    target: the painter's-order approximation blends there, ppu_soft does not."""
    s = st_sat(9, 16)
    for o in range(0x400):                       # punch holes into BG3
        if R.random() < 0.3:
            s.v16(31 * 0x800 + o * 2, 200)       # tile 200 is all zero -> transparent
    return s


def st_blank():
    s = st_bgtext()
    s.r(0x00, 0x0F80)
    return s


def st_bands512():
    """Per-line scroll bands whose world rows share ring slots: BG0 256x512,
    lines 0..79 at vofs 0 (map rows 0..9), lines 80..159 at vofs 176 (rows
    32..41, the second screen block).  Both bands use ring rows 0..9."""
    s = St()
    rnd_pal(s, 0, 256)
    tiles8(s, 0x0000, 128, transp=0.2)
    for o in range(0x800):                       # 2 screen blocks: 24 (rows 0..31), 25 (rows 32..63)
        s.v16(24 * 0x800 + o * 2, R.randrange(128) | (R.randrange(4) << 10))
    s.r(0x08, 0x8080 | (24 << 8) | 0)            # BG0 8bpp 256x512 sb24
    s.lrv = 1
    for ln in range(160):
        s.lines[ln][6] = [5 if ln < 80 else 13, 0, 0, 0]
        s.lines[ln][7] = [0 if ln < 80 else 176, 0, 0, 0]
    s.r(0x10, 5); s.r(0x12, 0)
    s.r(0x00, 0x0100)
    return s


def st_affobj_many():
    """16 double-size 64x64 rotated 8bpp OBJs (128x128 boxes, all on screen):
    more CPU-rasterised OBJ pixels than the per-frame scratch memory holds."""
    s = St()
    rnd_pal(s)
    background(s, bg=3, prio=3, sb=31)
    obj_vram_random(s, transp=0.2)
    for i in range(16):
        a = math.radians(10 + 20 * i)
        sc = 0.8
        s.aff(i, int(math.cos(a) * 256 / sc), int(math.sin(a) * 256 / sc), int(-math.sin(a) * 256 / sc), int(math.cos(a) * 256 / sc))
        x, y = (i % 8) * 16, (i // 8) * 32
        a0 = (y & 0xFF) | 0x100 | 0x200 | 0x2000 | (0 << 14)
        a1 = (x & 0x1FF) | (i << 9) | (3 << 14)
        a2 = ((i * 64) & 0x3FF) | (0 << 10)
        s.obj(i, a0, a1, a2)
    s.r(0x00, 0x1840)
    return s


def st_semi_bright(eva=10, evb=6):
    """Semi-transparent OBJs (also 1st targets) with BLDCNT brighten: over a 2nd
    target (BG2) they alpha-blend, over a non-target (BG3) they brighten."""
    s = St()
    rnd_pal(s)
    background(s, bg=3, prio=3, sb=31)
    tiles8(s, 64 * 64, 64, transp=0.5)
    for o in range(0x400):
        s.v16(30 * 0x800 + o * 2, 64 + R.randrange(64))
    s.r(0x0C, 0x80 | (30 << 8) | 2)              # BG2 partial, prio 2
    obj_vram_random(s)
    n = sprites_grid(s, prio_cycle=False)
    for i in range(n):                           # all semi-transparent, prio 1
        a0 = struct.unpack_from('<H', s.oam, i * 8)[0]
        struct.pack_into('<H', s.oam, i * 8, a0 | (1 << 10))
        a2 = struct.unpack_from('<H', s.oam, i * 8 + 4)[0]
        struct.pack_into('<H', s.oam, i * 8 + 4, (a2 & ~0x0C00) | (1 << 10))
    s.r(0x50, 0x0490)                            # brighten, 1st: OBJ, 2nd: BG2
    s.r(0x52, eva | (evb << 8))
    s.r(0x54, 9)
    s.r(0x00, 0x1C40)
    return s


def st_mode3_blend():
    """Mode 3 BG2 (1st target, prio 0) over OBJs (2nd targets, prio 1); the
    backdrop is not a 2nd target, so BG2 is drawn in several tagged passes."""
    s = St()
    rnd_pal(s, 256, 512)
    for o in range(0, 240 * 160 * 2, 2):
        s.v16(o, R.randrange(0x8000))
    obj_vram_random(s)
    for i in range(6):
        s.obj(i, (10 + 24 * (i // 3)) | 0x2000 | (0 << 14), (20 + 70 * (i % 3)) | (3 << 14), 512 + 64 * i | (1 << 10))
    s.r(0x20, 256); s.r(0x26, 256)
    s.r(0x0C, 0x0000)
    s.r(0x50, 0x1044)                            # alpha, 1st: BG2, 2nd: OBJ
    s.r(0x52, 9 | (16 << 8))                     # a configuration whose TLUT remap is not the identity
    s.r(0x00, 0x1443)
    return s


def st_mode2():
    """Mode 2: BG2 affine 256x256 without wraparound (rotated, scaled), BG3
    affine 1024x1024 with wraparound whose map runs past 0x10000 into OBJ VRAM
    (char base 2), both from the live registers (reference point + PB/PD * line)."""
    s = St()
    rnd_pal(s)
    tiles8(s, 0x0000, 256, transp=0.3)
    tiles8(s, 0x8000, 256, transp=0.0)
    for o in range(32 * 32):
        s.vram[20 * 0x800 + o] = R.randrange(256)
    for o in range(31 * 0x800, 0x10000):
        s.vram[o] = R.randrange(256)
    obj_vram_random(s)
    s.r(0x0C, 0x4000 | (20 << 8) | (0 << 2) | 0x80 | 1)        # BG2 256x256, no wrap, prio 1
    s.r(0x0E, 0xE000 | (31 << 8) | (2 << 2) | 0x80 | 2)        # BG3 1024x1024, wrap, prio 2
    s.r(0x20, 200); s.r(0x22, 60); s.r(0x24, -50); s.r(0x26, 220)
    s.r32(0x28, (-30) << 8); s.r32(0x2C, 20 << 8)
    s.r(0x30, 296); s.r(0x32, -30); s.r(0x34, 30); s.r(0x36, 256)
    s.r32(0x38, 1000 << 8); s.r32(0x3C, (-77 << 8) | 0x80)
    s.obj(0, 40 | 0x2000, 60 | (2 << 14), 0 | (1 << 10))
    s.obj(1, 90 | 0x2000 | (1 << 14), 150 | (3 << 14) | 0x1000, 128 | (3 << 10))
    s.r(0x00, 0x1C42)
    return s


def st_mode5():
    """Mode 5 (160x128 direct colour, page 1), scaled and rotated, brightened."""
    s = St()
    for o in range(0, 160 * 128 * 2, 2):
        s.v16(0xA000 + o, R.randrange(0x8000))
    s.r(0x20, 180); s.r(0x22, 40); s.r(0x24, -40); s.r(0x26, 200)
    s.r32(0x28, 12 << 8); s.r32(0x2C, (-8) << 8)
    s.r(0x50, 0x0084); s.r(0x54, 5)                # brighten BG2
    s.r(0x00, 0x0415)
    return s


def st_objwrap():
    """OBJs whose signed 9-bit x is -256..-233 (hardware x 256..279): off the
    left edge, never wrapped to the right (ppu_soft; visible in a wider view),
    next to x / y wrap controls that are visible."""
    s = St()
    rnd_pal(s)
    background(s, bg=3, prio=3, sb=31)
    obj_vram_random(s, transp=0.2)
    s.aff(0, 230, 60, -60, 230)
    cases = [
        # (a0 extra, shape, size, x, y, tile, a1 extra)
        (0x2000, 0, 3, 0x100, 20, 0, 0),             # 64x64 at x=-256
        (0x2000, 0, 2, 0x110, 90, 128, 0x1000),      # 32x32 at x=-240, H-flip
        (0x2000, 0, 1, 0x117, 130, 192, 0x2000),     # 16x16 at x=-233, V-flip
        (0x2000 | 0x300, 0, 3, 0x108, 30, 256, 0),   # affine double 64x64 (128 box) at x=-248
        (0x0000, 0, 1, 0x112, 60, 400, 0),           # 4bpp 16x16 at x=-238
        (0x2000, 0, 2, 0x1F0, 50, 420, 0),           # 32x32 at x=-16 (visible control)
        (0x2000, 0, 2, 100, 250, 484, 0x1000),       # 32x32 at y=250: wraps to the top
        (0x2000, 1, 3, 60, 180, 548, 0),             # 64x32 at y=180: below the GBA screen
        (0x2000, 2, 3, 0x1C0, 200, 676, 0x3000),     # 32x64 at x=-64, y=200 (-56): HV-flip
    ]
    for i, (a0x, sh, sz, x, y, tile, a1x) in enumerate(cases):
        a0 = (y & 0xFF) | a0x | (sh << 14)
        a1 = (x & 0x1FF) | (sz << 14) | a1x
        s.obj(i, a0, a1, (tile & 0x3FF) | ((i % 3) << 10) | (3 << 12))
    s.r(0x00, 0x1840)
    return s


def st_semi_bright2(eva=10, evb=6):
    """Semi-transparent OBJs over BG2, which is a 1st target of BLDCNT
    brighten and a 2nd target: ppu_soft blends the OBJ with BG2's raw colour."""
    s = st_semi_bright(eva, evb)
    s.r(0x50, 0x0494)                            # brighten, 1st: OBJ + BG2, 2nd: BG2
    return s


def st_bgedge():
    """Text BG addressing edges: 8bpp / 4bpp tiles past the end of BG VRAM
    (transparent), a 512x512 map whose last screen blocks lie in OBJ VRAM
    (entries read from there), 16-bit scroll values."""
    s = St()
    rnd_pal(s)
    for o in range(0x10000):                     # BG2 8bpp tiles everywhere (cb 0)
        s.vram[o] = 0 if R.random() < 0.3 else R.randrange(1, 256)
    obj_vram_random(s, transp=0.0)               # = BG2's map blocks 1..3
    for o in range(0x400):                       # BG0: 8bpp cb3 sb8, tiles 200..299
        s.v16(8 * 0x800 + o * 2, (200 + R.randrange(100)) | (R.randrange(4) << 10))
    for o in range(0x400):                       # BG1: 4bpp cb3 sb9, tiles 480..539
        s.v16(9 * 0x800 + o * 2, (480 + R.randrange(60)) | (R.randrange(4) << 10) | (R.randrange(16) << 12))
    tiles4(s, 0x4000, 512, transp=0.4)           # BG3: 4bpp cb1 sb30 512x256
    for o in range(0x800):
        s.v16(30 * 0x800 + o * 2, R.randrange(512) | (R.randrange(4) << 10) | (R.randrange(16) << 12))
    s.r(0x08, 0x0080 | (3 << 2) | (8 << 8) | 0)
    s.r(0x0A, 0x0000 | (3 << 2) | (9 << 8) | 1)
    s.r(0x0C, 0xC080 | (0 << 2) | (31 << 8) | 2)
    s.r(0x0E, 0x4000 | (1 << 2) | (30 << 8) | 3)
    for n, (h, v) in enumerate([(0x8003, 0x7FF9), (0xFFFF, 13), (0x0123, 0xFE77), (0x4321, 0x0105)]):
        s.r(0x10 + 4 * n, h); s.r(0x12 + 4 * n, v)
    s.r(0x00, 0x0F00)
    return s


def st_obj4_many():
    """48 4bpp 64x64 OBJs (3072 CPU-expanded tiles: the per-frame scratch
    tile area fills several times), flips, palette banks, priorities."""
    s = St()
    rnd_pal(s)
    background(s, bg=3, prio=2, sb=31)
    obj_vram_random(s, transp=0.4)
    for i in range(48):
        x, y = (i * 37) % 260 - 20, (i * 23) % 180 - 20
        a0 = (y & 0xFF) | (0 << 14)
        a1 = (x & 0x1FF) | (3 << 14) | ((i % 4) << 12)
        s.obj(i, a0, a1, ((i * 64) & 0x3FF) | ((i % 4) << 10) | ((i % 16) << 12))
    s.r(0x00, 0x1840)
    return s


def st_semi_dark_bd():
    """Semi-transparent OBJs with BLDCNT darken whose 1st and 2nd targets
    include BG2 and the backdrop (visible through BG2's holes)."""
    s = st_semi_bright(10, 6)
    s.r(0x50, 0x24F4)                            # darken, 1st: OBJ BG2 BD, 2nd: BG2 BD
    s.r(0x00, 0x1440)
    return s


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    states = {
        'synth_bgtext': st_bgtext(),
        'synth_obj1d': st_obj(True),
        'synth_obj2d': st_obj(False),
        'synth_affobj': st_affobj(),
        'synth_mode7': st_mode7(),
        'synth_sat9_16': st_sat(9, 16),
        'synth_sat16_16': st_sat(16, 16),
        'synth_sat14_6': st_sat(14, 6),
        'synth_mode4': st_mode4(0, False),
        'synth_mode4_affine': st_mode4(1, True),
        'synth_mode3_dark': st_mode3(),
        'synth_bright': st_effect(2, 0x3F, 7),
        'synth_dark': st_effect(3, 0x12, 11),
        'synth_fade': st_fade(),
        'synth_cross': st_cross(),
        'synth_semi': st_semi(),
        'synth_approx_bd': st_approx_bd(),
        'synth_blank': st_blank(),
        'synth_bands512': st_bands512(),
        'synth_affobj_many': st_affobj_many(),
        'synth_semi_bright': st_semi_bright(10, 6),
        'synth_semi_bright_id': st_semi_bright(16, 0),
        'synth_mode3_blend': st_mode3_blend(),
        'synth_mode2': st_mode2(),
        'synth_mode5': st_mode5(),
        'synth_objwrap': st_objwrap(),
        'synth_semi_bright2': st_semi_bright2(10, 6),
        'synth_bgedge': st_bgedge(),
        'synth_obj4_many': st_obj4_many(),
        'synth_semi_dark_bd': st_semi_dark_bd(),
        'synth_rain': st_weather(16, 12),
        'synth_rain_hole': st_weather(16, 12, hole=True),
        'synth_sunbeam': st_sunbeam(9, 16),
        'synth_dialog': st_dialog(True),
        'synth_dialog_corner': st_dialog(False),
    }
    for name, s in states.items():
        s.save(os.path.join(out, name + '.bin'))
    print(' '.join(states.keys()))


if __name__ == '__main__':
    main()

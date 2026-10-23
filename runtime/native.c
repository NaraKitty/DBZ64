/*
 * native.c - native C replacements for hot hand-written guest routines of
 * the Webfoot engine (the IWRAM sound mixer is byte-identical in both games;
 * only its address differs).
 *
 * games/<game>/game.toml [[native]] makes the recompiler emit a call to these
 * instead of translating the guest code.  Each one must produce exactly the
 * guest routine's results (memory and the registers its callers may read);
 * the host harness checks this by comparing audio output with and without
 * the replacement.
 */
#include "gba.h"
#include "io.h"
#include "rom.h"

/* mix_voice (ARM, IWRAM; LoG2 0x03000520, Buu's Fury 0x03000868): mix n
 * samples of one voice into the 32-bit accumulator.  r0 = voice {pos, step, data, vol}, r1 = acc, r2 = n.
 * Positions are 14-bit fixed point, samples are nearest-neighbour signed
 * bytes, and vol packs the left/right volumes in one word so a single
 * multiply-accumulate mixes both channels (the halves may carry into each
 * other; that is what the game does).  The voice position is not written
 * back: the caller advances it. */
void native_mix_voice(void) {
    uint32_t v = cpu.r[0], acc = cpu.r[1], n = cpu.r[2];
    uint32_t pos = rd32(v), step = rd32(v + 4), data = rd32(v + 8), vol = rd32(v + 12);
    uint32_t frac = pos << 18, fstep = step << 18;
    uint32_t p = data + (uint32_t)((int32_t)pos >> 14);
    uint32_t istep = (uint32_t)((int32_t)step >> 14);
    uint32_t *a = gba_host_ptr(acc);
    int32_t s = 0;
    if (a && ((acc & 0xFFFu) + n * 4u) <= 0x1000u) {
        for (uint32_t i = 0; i < n; i++) {
            s = (int8_t)rd8(p);
            uint32_t o = frac;
            frac += fstep;
            p += istep + (frac < o);
            a[i] += vol * (uint32_t)s;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            s = (int8_t)rd8(p);
            uint32_t o = frac;
            frac += fstep;
            p += istep + (frac < o);
            wr32(acc + 4 * i, rd32(acc + 4 * i) + vol * (uint32_t)s);
        }
    }
    cpu.r[0] = (uint32_t)s;
    cpu.r[1] = acc + 4 * n;
    cpu.r[2] = 0;
    cpu.r[3] = p;
}

static inline uint32_t clamp8(int32_t x) {
    return (uint32_t)(x > 127 ? 127 : x < -128 ? -128 : x) & 0xFF;
}

/* clip (ARM, IWRAM; LoG2 0x03000704, Buu's Fury 0x03000A4C): convert n
 * accumulated samples (n even) to the two signed 8-bit FIFO rings, clearing
 * the accumulator.  r0 = left ring (low halves), r1 = right ring (high
 * halves), r2 = acc, r3 = n. */
void native_clip(void) {
    uint32_t ra = cpu.r[0], rb = cpu.r[1], acc = cpu.r[2];
    int32_t n = (int32_t)cpu.r[3];
    do {
        uint32_t w0 = rd32(acc), w1 = rd32(acc + 4);
        wr32(acc, 0);
        wr32(acc + 4, 0);
        acc += 8;
        uint32_t l = clamp8((int32_t)(w0 << 16) >> 22) | (clamp8((int32_t)(w1 << 16) >> 22) << 8);
        uint32_t r = clamp8((int32_t)w0 >> 22) | (clamp8((int32_t)w1 >> 22) << 8);
        wr16(ra, l);
        wr16(rb, r);
        ra += 2;
        rb += 2;
        n -= 2;
    } while (n > 0);
    cpu.r[0] = ra;
    cpu.r[1] = rb;
    cpu.r[2] = acc;
    cpu.r[3] = (uint32_t)n;
}

/* host pointer to [a, a+len) when it lies in one of the contiguous RAMs */
static uint32_t *ram_span(uint32_t a, uint32_t len) {
    if (a >= 0x02000000u && len <= 0x40000u && a - 0x02000000u <= 0x40000u - len)
        return gba_mem.ewram + (a - 0x02000000u) / 4;
    if (a >= 0x03000000u && len <= 0x8000u && a - 0x03000000u <= 0x8000u - len)
        return gba_mem.iwram + (a - 0x03000000u) / 4;
    return NULL;
}

/* host pointer to a source block in RAM or in (resident) ROM */
static const uint32_t *src_span(uint32_t a, uint32_t len) {
    const uint32_t *p = ram_span(a, len);
    if (!p && (a >> 24) >= 0x08 && (a >> 24) <= 0x0D)
        p = gba_rom_ptr(a, len);
    return p;
}

/* 0x03000480 (ARM, IWRAM): gather 8x8 8bpp tiles out of a bitmap.
 * r0 = bitmap {+0x24 base, +0x2C stride}, r1 = output (64 bytes per tile,
 * advanced), r2 -> rect {x0, y0, x1, y1} in pixels (multiples of 8).  Each
 * tile is copied as 4 groups of two 8-byte rows (ldm, ldm, stm).
 * Outputs (the routine's summary): r0-r3, r12 and the flags (Z and C set:
 * the last 'subs' reaches 0). */
void native_tile_gather(void) {
    /* stm ignores the low address bits but its writeback keeps them */
    uint32_t bm = cpu.r[0], r1 = cpu.r[1], out = r1 & ~3u, rect = cpu.r[2], sp = cpu.r[13];
    uint32_t x0 = rd32(rect), y0 = rd32(rect + 4), x1 = rd32(rect + 8), y1 = rd32(rect + 12);
    uint32_t stride = rd32(bm + 0x2C);
    uint32_t base = rd32(bm + 0x24) + stride * y0 + x0;
    uint32_t rows = y1 - y0, width = x1 - x0;
    /* the pushed r4-r11 (not inputs: the recompiled routine pushes 0) */
    for (int i = 0; i < 8; i++)
        wr32(sp - 32 + 4 * (uint32_t)i, 0);
    uint32_t r0 = 0, r2 = 0, r12 = 0;
    /* both sides in plain RAM: no per-access address translation */
    uint32_t ntiles = (rows / 8) * (width / 8);
    const uint32_t *hs = NULL;
    uint32_t *hd = NULL;
    if (!(rows & 7) && !(width & 7) && rows && width && !(stride & 3) && !(base & 3) &&
        stride < 0x10000u && rows <= 0x1000u) {
        hs = src_span(base, stride * (rows - 1) + width);
        hd = ram_span(out, ntiles * 64);
    }
    if (hs && hd) {
        uint32_t sw = stride / 4;
        for (uint32_t band = 0; band < rows; band += 8) {
            r2 = base + band * stride;
            const uint32_t *col = hs + band * sw;
            for (uint32_t w = 0; w < width; w += 8, col += 2) {
                const uint32_t *s = col;
                for (int g = 0; g < 4; g++, s += 2 * sw) {
                    uint32_t a = s[0], b = s[1], c = s[sw], d = s[sw + 1];
                    hd[0] = a; hd[1] = b; hd[2] = c; hd[3] = d;
                    hd += 4;
                    r12 = d;
                }
                r0 = r2 + 8 * stride;
                r2 += 8;
            }
        }
        r1 += ntiles * 64;
    } else {
        uint32_t w;
        do {
            r2 = base;
            w = width;
            do {
                uint32_t s = r2;
                for (int g = 0; g < 4; g++) {
                    uint32_t a = rd32(s & ~3u), b = rd32((s & ~3u) + 4);
                    s += stride;
                    uint32_t c = rd32(s & ~3u), d = rd32((s & ~3u) + 4);
                    s += stride;
                    wr32(out, a); wr32(out + 4, b); wr32(out + 8, c); wr32(out + 12, d);
                    out += 16;
                    r1 += 16;
                    r12 = d;
                }
                r0 = s;
                r2 += 8;
                w -= 8;
            } while (w);
            base += stride * 8;
            rows -= 8;
        } while (rows);
    }
    cpu.r[0] = r0;
    cpu.r[1] = r1;
    cpu.r[2] = r2;
    cpu.r[3] = 0;
    cpu.r[12] = r12;
    cpu.n = 0;
    cpu.z = 1;
    cpu.c = 1;
    cpu.v = 0;
}

/* n halfwords from src to dst, like a 16-bit incrementing DMA */
static void copy16(uint32_t dst, uint32_t src, uint32_t n) {
    uint32_t bytes = n * 2;
    gba_mem_touch(dst, bytes);          /* VRAM dirty tracking, before writing */
    uint8_t *hs = gba_host_ptr(src), *hd = gba_host_wptr(dst);
    if (hs && hd && !((src ^ dst) & 2) && (src & 0xFFFu) + bytes <= 0x1000u &&
        (dst & 0xFFFu) + bytes <= 0x1000u) {
        /* same halfword phase: native words are guest words */
        if (n && (src & 2)) {
            *(uint16_t *)((uintptr_t)hd ^ GBA_HX) = *(const uint16_t *)((uintptr_t)hs ^ GBA_HX);
            hs += 2; hd += 2; n--;
        }
        for (; n >= 2; n -= 2, hs += 4, hd += 4)
            *(uint32_t *)hd = *(const uint32_t *)hs;
        if (n)
            *(uint16_t *)((uintptr_t)hd ^ GBA_HX) = *(const uint16_t *)((uintptr_t)hs ^ GBA_HX);
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        wr16(dst + 2 * i, rd16(src + 2 * i));
}

/* n halfwords at byte offset off of two word-aligned host blocks */
static inline void copy_block16(uint32_t *dst, const uint32_t *src, uint32_t off, uint32_t n) {
    uint8_t *d = (uint8_t *)dst + off;
    const uint8_t *s = (const uint8_t *)src + off;
    if (n && (off & 2)) {
        *(uint16_t *)((uintptr_t)d ^ GBA_HX) = *(const uint16_t *)((uintptr_t)s ^ GBA_HX);
        d += 2; s += 2; n--;
    }
    for (; n >= 2; n -= 2, d += 4, s += 4)
        *(uint32_t *)d = *(const uint32_t *)s;
    if (n)
        *(uint16_t *)((uintptr_t)d ^ GBA_HX) = *(const uint16_t *)((uintptr_t)s ^ GBA_HX);
}

/* do the words holding n halfwords at byte offset off already match? */
static inline int same_block16(const uint32_t *dst, const uint32_t *src, uint32_t off, uint32_t n) {
    for (uint32_t w = off >> 2, e = (off + 2 * n + 3) >> 2; w < e; w++)
        if (dst[w] != src[w])
            return 0;
    return 1;
}

/* 0x080054A4 (Thumb): rebuild a field layer's 32x32 hardware tilemap from
 * the four map-quadrant buffers at layer+0x0C/0x10/0x14/0x18, split at the
 * scroll position (x, y) in tiles.  r0 = layer, r1 -> {x word, y word}.
 * The game issues one DMA3 per row segment, about 256 per frame, rewriting
 * maps that rarely change; here rows that already match are skipped, so
 * VRAM is only marked dirty (and re-checked by the renderer) when a map
 * really changes, and the DMA registers are left as the last DMA leaves them.
 * Outputs (the routine's summary): r1, r2, r3, r12, r14 and the flags, which
 * are the same on every path. */
void native_map_refresh(void) {
    uint32_t layer = cpu.r[0], sp = cpu.r[13], lr = cpu.r[14];
    uint32_t x = (rd32(cpu.r[1]) << 24) >> 27, y = (rd32(cpu.r[1] + 4) << 24) >> 27;
    uint32_t sb = 0x0600E000u + (rd8(layer + 4) << 11);
    uint32_t qa = rd32(layer + 0x0C), qb = rd32(layer + 0x10);
    uint32_t qc = rd32(layer + 0x14), qd = rd32(layer + 0x18);
    /* the pushed {r3-r7, lr}; the r3 slot ends up holding the last row-loop-1
     * destination (the routine stores it there), r4-r7 are not inputs and the
     * recompiled routine pushes 0 for them */
    wr32(sp - 24, sb + 31 * 64 + 2 * x);
    wr32(sp - 20, 0);
    wr32(sp - 16, 0);
    wr32(sp - 12, 0);
    wr32(sp - 8, 0);
    wr32(sp - 4, lr);
    /* segments in the routine's order.  Normally all four buffers are in
     * RAM or ROM and the screen block in VRAM: then each 2KB block is
     * resolved to a host pointer once and rows are compared and copied
     * directly. */
    int last = (x && y) ? 4 : y ? 3 : x ? 2 : 1;
    uint32_t ls = 0, ld = 0, ln = 0;
    int touched = 0;
    const uint32_t *hq[4] = { src_span(qa, 0x800), src_span(qb, 0x800), src_span(qc, 0x800),
                              src_span(qd, 0x800) };
    uint32_t *hv = NULL;
    if (!((qa | qb | qc | qd) & 3) && hq[0] && hq[1] && hq[2] && hq[3] && sb + 0x800u <= 0x06010000u)
        hv = gba_mem.vram + (sb - 0x06000000u) / 4;
#define SEG(loop, row, lastrow, s, d, cnt)                                      \
    do {                                                                        \
        if (last == (loop) && (row) == (lastrow)) {                             \
            ls = (s); ld = (d); ln = (cnt);                                     \
        }                                                                       \
        if (!hv) {                                                              \
            copy16((d), (s), (cnt));                                            \
        } else if (!same_block16(hv, hq[(loop) - 1], (d) - sb, (cnt))) {        \
            if (!touched) {                                                     \
                gba_mem_touch(sb, 0x800);                                       \
                touched = 1;                                                    \
            }                                                                   \
            copy_block16(hv, hq[(loop) - 1], (d) - sb, (cnt));                  \
        }                                                                       \
    } while (0)
    for (uint32_t r = y; r < 32; r++)
        SEG(1, r, 31u, qa + r * 64 + 2 * x, sb + r * 64 + 2 * x, 32 - x);
    if (x)
        for (uint32_t r = y; r < 32; r++)
            SEG(2, r, 31u, qb + r * 64, sb + r * 64, x);
    for (uint32_t r = 0; r < y; r++)
        SEG(3, r, y - 1, qc + r * 64 + 2 * x, sb + r * 64 + 2 * x, 32 - x);
    if (x)
        for (uint32_t r = 0; r < y; r++)
            SEG(4, r, y - 1, qd + r * 64, sb + r * 64, x);
#undef SEG
    gba_dma_settle16(3, ls, ld, ln);
    cpu.r[1] = y;
    cpu.r[2] = y;
    cpu.r[3] = sb + 31 * 64 + 2 * x;
    cpu.r[12] = 32 - x;
    cpu.r[14] = y ? sb + (y - 1) * 64 + 2 * x : x ? x : 32;
    cpu.n = 0;
    cpu.z = 1;
    cpu.c = 1;
    cpu.v = 0;
}

/*
 * native.c - Buu's Fury: native replacements for hot IWRAM routines
 * (game.toml [[native]]; the mixer natives shared with LoG2 are in
 * runtime/native.c).  Each must leave memory and the registers and flags
 * the guest routine leaves exactly as it does.
 */
#include "gba.h"
#include "recomp.h"
#include "rom.h"

/* ------------------------------------------------------------ memcpy32 / memset32 / memset16 */

/* Host pointer for guest address a when the access may bypass rd32/wr32,
 * else NULL; valid up to seg_end(a).  EWRAM, IWRAM and resident ROM (reads)
 * come per 4KB page from the runtime; VRAM directly (callers mark what they
 * change dirty); palette and OAM only in their first 1KB, where every
 * platform's rd32/wr32 reach the same memory. */
static inline uint8_t *seg_ptr(uint32_t a, int write) {
    uint32_t r = (a >> 24) & 0xFu;
    if (r == 5 || r == 7) {
        if ((a & 0x00FFFFFFu) >= 0x400u)
            return NULL;
        return (uint8_t *)(r == 5 ? gba_mem.pal : gba_mem.oam) + (a & 0x3FFu);
    }
    if (r == 6) {
        uint32_t o = a & 0x1FFFFu;
        return (uint8_t *)gba_mem.vram + (o >= 0x18000u ? o - 0x8000u : o);
    }
    return write ? gba_host_wptr(a) : gba_host_ptr(a);
}

static inline uint32_t seg_end(uint32_t a) {
    uint32_t r = (a >> 24) & 0xFu;
    return (r == 5 || r == 7) ? (a | 0x3FFu) + 1 : (a | 0xFFFu) + 1;
}

/* Is [a, a + len) plain memory in one region (EWRAM, IWRAM, VRAM, or the
 * first 1KB of palette or OAM)?  Stores there have no side effects, and a
 * word store equals two halfword stores. */
static inline int plain_range(uint32_t a, uint32_t len) {
    uint32_t r = (a >> 24) & 0xFu, e = a + len - 1;
    if (!len || ((e >> 24) & 0xFu) != r || e < a)
        return 0;
    if (r == 2 || r == 3 || r == 6)
        return 1;
    return (r == 5 || r == 7) && (e & 0x00FFFFFFu) < 0x400u;
}

/* Copy nw words forward from s to d (both word aligned), as a forward word
 * loop does when d <= s or the ranges do not overlap.  VRAM words are only
 * written where they change, and the renderer is told which units changed
 * (gba_vram_note) instead of re-checking whole dirty pages.  Pieces that
 * host pointers cannot reach use rd32 / wr32 like the guest code. */
static void copy_words(uint32_t d, uint32_t s, uint32_t nw) {
    while (nw) {
        uint32_t n = nw, k;
        uint32_t ld = (seg_end(d) - d) >> 2, ls = (seg_end(s) - s) >> 2;
        if (n > ld) n = ld;
        if (n > ls) n = ls;
        uint32_t *hd = (uint32_t *)seg_ptr(d, 1);
        const uint32_t *hs = (const uint32_t *)seg_ptr(s, 0);
        if (hd && hs) {
            if ((d >> 24) == 0x06) {
                /* word by word, the same as the forward copy (when d is
                 * below s, the source words still to come are never
                 * written first) */
                uint32_t first = n, last = 0;
                for (k = 0; k < n; k++)
                    if (hd[k] != hs[k]) {
                        hd[k] = hs[k];
                        if (first == n)
                            first = k;
                        last = k;
                    }
                if (first < n)
                    gba_vram_note(d + 4 * first, 4 * (last - first + 1));
            } else {
                for (k = 0; k < n; k++)
                    hd[k] = hs[k];
            }
        } else {
            for (k = 0; k < n; k++)
                wr32(d + 4 * k, rd32(s + 4 * k));
        }
        d += 4 * n;
        s += 4 * n;
        nw -= n;
    }
}

/* Store nw copies of the word v from d on (word aligned); VRAM as above. */
static void fill_words(uint32_t d, uint32_t v, uint32_t nw) {
    while (nw) {
        uint32_t n = nw, k;
        uint32_t ld = (seg_end(d) - d) >> 2;
        if (n > ld) n = ld;
        uint32_t *hd = (uint32_t *)seg_ptr(d, 1);
        if (hd) {
            if ((d >> 24) == 0x06) {
                uint32_t first = n, last = 0;
                for (k = 0; k < n; k++)
                    if (hd[k] != v) {
                        hd[k] = v;
                        if (first == n)
                            first = k;
                        last = k;
                    }
                if (first < n)
                    gba_vram_note(d + 4 * first, 4 * (last - first + 1));
            } else {
                for (k = 0; k < n; k++)
                    hd[k] = v;
            }
        } else {
            for (k = 0; k < n; k++)
                wr32(d + 4 * k, v);
        }
        d += 4 * n;
        nw -= n;
    }
}

/* guest halfword k of a run that starts at byte offset o of the
 * word-aligned host block p (guest words are native words) */
#define HW(p, o, k) (*(uint16_t *)((p) + (((o) + 2 * (k)) ^ GBA_HX)))

/* Do the n guest halfwords at byte offset o of the word-aligned host blocks
 * a and b differ?  Compared a word at a time (the halfword at offset 2 of a
 * guest word is the high half of the native word on every host), four
 * words of a before four of b: a VRAM screen block and its source can sit
 * on the same lines of the N64's direct-mapped D-cache. */
static int halves_differ(const uint8_t *a, const uint8_t *b, uint32_t o, uint32_t n) {
    const uint32_t *wa = (const uint32_t *)a, *wb = (const uint32_t *)b;
    uint32_t w = o >> 2, w1 = (o + 2 * n - 1) >> 2;          /* inclusive */
    uint32_t m0 = (o & 2) ? 0xFFFF0000u : 0xFFFFFFFFu;
    uint32_t m1 = ((o + 2 * n) & 2) ? 0x0000FFFFu : 0xFFFFFFFFu;
    if (w == w1)
        return ((wa[w] ^ wb[w]) & m0 & m1) != 0;
    uint32_t d = ((wa[w] ^ wb[w]) & m0) | ((wa[w1] ^ wb[w1]) & m1);
    for (w++; w + 4 <= w1; w += 4) {
        uint32_t a0 = wa[w], a1 = wa[w + 1], a2 = wa[w + 2], a3 = wa[w + 3];
        uint32_t b0 = wb[w], b1 = wb[w + 1], b2 = wb[w + 2], b3 = wb[w + 3];
        d |= (a0 ^ b0) | (a1 ^ b1) | (a2 ^ b2) | (a3 ^ b3);
    }
    for (; w < w1; w++)
        d |= wa[w] ^ wb[w];
    return d != 0;
}

/* 0x030007FC (ARM, IWRAM) memcpy16: r0 = dst, r1 = src, r2 = bytes.  A
 * Duff's device: 'add pc, pc, r3, lsl #2' with r3 = (16 - r2) & 0xE enters
 * an unrolled block of eight ldrh/strh pairs, skipping r3 / 2 of them, then
 * 'subs r2, r2, #16; bgt' repeats whole blocks.  So it copies halfwords
 * forward (overlap behaves like the loop), rounds odd sizes up and copies 16
 * bytes for r2 = 0.  Exit: r0/r1 advanced, r2 = what the last subs left,
 * r3 = the last halfword, flags from that subs.  The game's tile and map
 * streamers spend most of their time here.  VRAM is marked dirty only when
 * the copy changes it. */
void native_memcpy16(void) {
    uint32_t d = cpu.r[0], s = cpu.r[1], r2 = cpu.r[2];
    uint32_t count = 8 - (((16u - r2) & 0xEu) >> 1);
    uint32_t prev = r2;
    r2 -= 16;
    while ((int32_t)r2 > 0) {
        count += 8;
        prev = r2;
        r2 -= 16;
    }
    uint32_t bytes = count * 2, v = 0;
    uint8_t *hs = NULL, *hd = NULL;
    /* fast path: aligned, each range inside one host block (the common
     * case: a tile or map row), the destination plain memory */
    if (!((s | d) & 1) && s + bytes <= seg_end(s) && d + bytes <= seg_end(d) && plain_range(d, bytes)) {
        hs = seg_ptr(s, 0);
        hd = seg_ptr(d, 1);
    }
    if (hs && hd) {
        uint32_t so = s & 3u, dof = d & 3u;
        uint8_t *ps = hs - so, *pd = hd - dof;      /* word-aligned bases */
        uint32_t k = 0;
        int copy = 1;
        if ((d >> 24) == 0x06) {
            /* unchanged VRAM stays clean (equal halfwords everywhere: the
             * forward copy would only rewrite the same values) */
            if (so == dof) {
                copy = halves_differ(pd, ps, so, count);
            } else {
                while (k < count && HW(pd, dof, k) == HW(ps, so, k))
                    k++;
                copy = k < count;
                k = 0;
            }
            if (copy)
                gba_vram_note(d, bytes);
        }
        if (copy) {
            if (so == dof) {
                /* same alignment: guest words are host words, so whole
                 * words can move.  Equivalent to the halfword loop even when
                 * the ranges overlap: the distance is a multiple of 4. */
                if (so) {
                    HW(pd, dof, 0) = HW(ps, so, 0);
                    k = 1;
                }
                const uint32_t *ws = (const uint32_t *)(ps + so + 2 * k);
                uint32_t *wd = (uint32_t *)(pd + dof + 2 * k);
                uint32_t nw = (count - k) / 2;
                for (uint32_t w = 0; w < nw; w++)
                    wd[w] = ws[w];
                k += 2 * nw;
            }
            for (; k < count; k++)
                HW(pd, dof, k) = HW(ps, so, k);
        }
        /* r3 = the last halfword loaded, which is the last one stored */
        v = HW(pd, dof, count - 1);
    } else {
        for (uint32_t k = 0; k < count; k++) {
            v = rd16(s + 2 * k);
            wr16(d + 2 * k, v);
        }
    }
    cpu.r[0] = d + bytes;
    cpu.r[1] = s + bytes;
    cpu.r[2] = r2;
    cpu.r[3] = v;
    /* flags of the last 'subs r2, r2, #16' (prev - 16) */
    cpu.n = r2 >> 31;
    cpu.z = r2 == 0;
    cpu.c = prev >= 16u;
    cpu.v = ((prev ^ 16u) & (prev ^ r2)) >> 31;
}

/* ------------------------------------------------------------ field map streamer */

/* a run of map rows copied by one loop of the routine: quadrant, first
 * row, rows, first column, columns, return address of its memcpy16 call */
typedef struct { uint32_t quad, r0, nr, c0, nc, ret; } map_seg_t;

/* host pointer to a word-aligned 2KB map quadrant in RAM or resident ROM */
static const uint8_t *quad_ptr(uint32_t a) {
    if (a & 3u)
        return NULL;
    uint32_t r = (a >> 24) & 0xFu;
    if (r == 2 && (a & 0x3FFFFu) + 0x800u <= 0x40000u)
        return (const uint8_t *)gba_mem.ewram + (a & 0x3FFFFu);
    if (r == 3 && (a & 0x7FFFu) + 0x800u <= 0x8000u)
        return (const uint8_t *)gba_mem.iwram + (a & 0x7FFFu);
    if (r >= 8 && r <= 0xD)
        return (const uint8_t *)gba_rom_ptr(a, 0x800);
    return NULL;
}

/* 0x08007D98 (Thumb): rebuild a field layer's 32x32 hardware tilemap (screen
 * block 0x0600E000 + bg * 0x800, bg = byte layer+4) from its four quadrant
 * maps at layer+0x10/0x14/0x18/0x1C, split at the scroll position (x, y) in
 * tiles (bits 3-7 of the words at r1 and r1+4).  r0 = layer, r1 -> {x, y}.
 * Rows y..31 come from quadrants 0 (columns x..31) and 1 (0..x-1), rows
 * 0..y-1 from 2 and 3: 64 memcpy16 calls per layer and frame, which mostly
 * rewrite what is already there.  Here rows that match are skipped, so the
 * map's VRAM is marked dirty only when it changes.
 * The exit is what the recompiled routine leaves: r0 = x, or the last
 * row's destination when quadrant 3 was copied; r1-r3 from the last
 * memcpy16; r4-r7 popped (pushed as 0: not inputs); r14 = the return
 * address of the last call; flags Z and C (every path ends with an equal
 * compare).  The stack frame (pushed registers and the locals) is written
 * as the routine writes it. */
void native_field_map_stream(void) {
    uint32_t layer = cpu.r[0], sp0 = cpu.r[13];
    uint32_t x = (rd32(cpu.r[1]) << 24) >> 27, y = (rd32(cpu.r[1] + 4) << 24) >> 27;
    uint32_t sb = 0x0600E000u + (rd8(layer + 4) << 11);
    uint32_t q[4] = { rd32(layer + 0x10), rd32(layer + 0x14), rd32(layer + 0x18), rd32(layer + 0x1C) };
    uint32_t sp = sp0 - 56;
    sp_wr32(sp0 - 20, 0);
    sp_wr32(sp0 - 16, 0);
    sp_wr32(sp0 - 12, 0);
    sp_wr32(sp0 - 8, 0);
    sp_wr32(sp0 - 4, cpu.r[14]);
    sp_wr32(sp + 8, x);
    sp_wr32(sp + 4, 32 - x);
    sp_wr32(sp + 0x20, 2 * (32 - x));
    sp_wr32(sp + 0x1C, 2 * x);
    if (x)
        sp_wr32(sp + 0x18, 2 * x);
    if (y) {
        sp_wr32(sp + 0x14, 2 * (32 - x));
        sp_wr32(sp + 0x10, 2 * x);
        if (x)
            sp_wr32(sp + 0xC, 2 * x);
    }
    /* the segments in the routine's order */
    map_seg_t seg[4];
    int nseg = 0;
    seg[nseg++] = (map_seg_t){ 0, y, 32 - y, x, 32 - x, 0x08007DE3u };
    if (x)
        seg[nseg++] = (map_seg_t){ 1, y, 32 - y, 0, x, 0x08007E09u };
    if (y) {
        seg[nseg++] = (map_seg_t){ 2, 0, y, x, 32 - x, 0x08007E35u };
        if (x)
            seg[nseg++] = (map_seg_t){ 3, 0, y, 0, x, 0x08007E5Bu };
    }
    uint8_t *hv = NULL;
    if (sb + 0x800u <= 0x06010000u)
        hv = (uint8_t *)gba_mem.vram + (sb - 0x06000000u);
    int last_fast = 0;
    for (int i = 0; i < nseg; i++) {
        const uint8_t *hq = hv ? quad_ptr(q[seg[i].quad]) : NULL;
        last_fast = hq != NULL;
        for (uint32_t r = seg[i].r0; r < seg[i].r0 + seg[i].nr; r++) {
            uint32_t off = r * 64 + 2 * seg[i].c0, n = seg[i].nc;
            if (hq) {
                /* both blocks word aligned, the row at the same offset */
                if (!halves_differ(hv, hq, off, n))
                    continue;
                gba_vram_note(sb + off, 2 * n);
                for (uint32_t k = 0; k < n; k++)
                    HW(hv, off, k) = HW(hq, off, k);
            } else {
                cpu.r[0] = sb + off;
                cpu.r[1] = q[seg[i].quad] + off;
                cpu.r[2] = 2 * n;
                native_memcpy16();
            }
        }
    }
    /* the last memcpy16 call's results (its byte count is even: no
     * rounding); without the fast path cpu.r[3] already holds its last
     * halfword, with it the source (RAM or ROM) still does */
    const map_seg_t *l = &seg[nseg - 1];
    uint32_t lr = l->r0 + l->nr - 1, bytes = 2 * l->nc;
    uint32_t src = q[l->quad] + lr * 64 + 2 * l->c0;
    cpu.r[1] = src + bytes;
    cpu.r[2] = bytes - 16 * ((bytes + 15) / 16);
    if (last_fast)
        cpu.r[3] = rd16(src + bytes - 2);
    cpu.r[0] = (x && y) ? sb + lr * 64 : x;
    cpu.r[4] = cpu.r[5] = cpu.r[6] = cpu.r[7] = 0;
    cpu.r[14] = l->ret;
    cpu.n = 0;
    cpu.z = 1;
    cpu.c = 1;
    cpu.v = 0;
}

/* Loop counts of the routines below for a byte count n:
 *  - 32-byte blocks ('sub r2, #32', then 'subs r2, #32; bgt' after each),
 *    entered when n >= 32 (signed): max(1, ceil((n - 32) / 32)), leaving
 *    0..32 bytes;
 *  - the tail loop ('subs r2, #unit; bgt', run at least once) for r bytes:
 *    ceil(r / unit), 1 when r <= unit (signed). */
static inline uint32_t blocks32(uint32_t n) {
    return n <= 64 ? 1 : (n - 1) / 32;
}

static inline uint32_t tail_count(uint32_t r, uint32_t unit) {
    return (int32_t)r <= (int32_t)unit ? 1 : (r + unit - 1) / unit;
}

/* flags of 'subs rd, fa, #k' */
static inline void flags_sub(uint32_t fa, uint32_t k) {
    uint32_t fr = fa - k;
    cpu.n = fr >> 31;
    cpu.z = fr == 0;
    cpu.c = fa >= k;
    cpu.v = ((fa ^ k) & (fa ^ fr)) >> 31;
}

/* flags of 'adds rd, fa, #k' */
static inline void flags_add(uint32_t fa, uint32_t k) {
    uint32_t fr = fa + k;
    cpu.n = fr >> 31;
    cpu.z = fr == 0;
    cpu.c = fr < fa;
    cpu.v = (~(fa ^ k) & (fa ^ fr)) >> 31;
}

/* Byte counts above this run the literal guest loop (no RAM region is
 * larger, so no real call gets there). */
#define BIG_COUNT 0x40000u

/* 0x03000768 (ARM, IWRAM) memcpy32: r0 = dst, r1 = src, r2 = bytes.
 *     cmp r2, #32; blt tail
 *     push {r4-r9}; sub r2, #32
 *  1: ldm r1!, {r3-r9, ip}; stm r0!, {r3-r9, ip}; subs r2, #32; bgt 1b
 *     pop {r4-r9}; adds r2, #32; bxeq lr
 *  tail: ldr r3, [r1], #4; str r3, [r0], #4; subs r2, #4; bgt tail
 * The exit is what the recompiled routine leaves: r0/r1 advanced, r2 and
 * the flags from the last subs/adds, r3 the last word loaded (the first word
 * of the last block when no tail ran), r12 the last block's eighth word,
 * r4-r9 popped.  r4-r7 are not inputs, so the recompiled routine pushes 0
 * for them (and leaves 0 in them on the tail-only path too). */
void native_memcpy32(void) {
    uint32_t d = cpu.r[0], s = cpu.r[1], n = cpu.r[2], sp = cpu.r[13];
    uint32_t r3 = cpu.r[3], r12 = cpu.r[12];
    uint32_t nb = 0, nt, r2 = n;
    int blk = (int32_t)n >= 32;
    if (blk) {
        nb = blocks32(n);
        r2 = n - 32 * nb;               /* after 'adds r2, #32' */
        flags_add(r2 - 32, 32);
        sp_wr32(sp - 24, 0);
        sp_wr32(sp - 20, 0);
        sp_wr32(sp - 16, 0);
        sp_wr32(sp - 12, 0);
        sp_wr32(sp - 8, cpu.r[8]);
        sp_wr32(sp - 4, cpu.r[9]);
    }
    nt = (blk && r2 == 0) ? 0 : tail_count(r2, 4);
    uint32_t total = 32 * nb + 4 * nt;
    if (((d | s) & 3) || n > BIG_COUNT || (uint32_t)(d - s) - 1u < total - 1u ||
        !plain_range(d, total)) {
        /* unaligned (ldm/stm and ldr treat the low bits differently),
         * overlapping with d above s, I/O or ROM, or absurd: the guest loop
         * itself */
        for (uint32_t b = 0; b < nb; b++, s += 32, d += 32) {
            uint32_t w[8];
            for (int k = 0; k < 8; k++)
                w[k] = rd32(s + 4 * k);
            for (int k = 0; k < 8; k++)
                wr32(d + 4 * k, w[k]);
            r3 = w[0];
            r12 = w[7];
        }
        for (uint32_t t = 0; t < nt; t++, s += 4, d += 4) {
            r3 = rd32(s);
            wr32(d, r3);
        }
    } else {
        copy_words(d, s, total / 4);
        /* the stores wrote exactly the words loaded to plain memory and
         * later stores go to higher addresses: the registers' values are in
         * the destination */
        if (nb) {
            uint32_t lb = d + 32 * (nb - 1);
            r3 = rd32(lb);
            r12 = rd32(lb + 28);
        }
        if (nt)
            r3 = rd32(d + total - 4);
        d += total;
        s += total;
    }
    if (nt) {
        uint32_t fa = r2 - 4 * (nt - 1);
        flags_sub(fa, 4);
        r2 = fa - 4;
    }
    cpu.r[0] = d;
    cpu.r[1] = s;
    cpu.r[2] = r2;
    cpu.r[3] = r3;
    cpu.r[4] = cpu.r[5] = cpu.r[6] = cpu.r[7] = 0;
    if (blk) {
        cpu.r[4] = sp_rd32(sp - 24);
        cpu.r[5] = sp_rd32(sp - 20);
        cpu.r[6] = sp_rd32(sp - 16);
        cpu.r[7] = sp_rd32(sp - 12);
        cpu.r[8] = sp_rd32(sp - 8);
        cpu.r[9] = sp_rd32(sp - 4);
    }
    cpu.r[12] = r12;
}

/* 0x030007A8 (ARM, IWRAM) memset32: r0 = dst, r1 = word, r2 = bytes.
 *     cmp r2, #32; blt tail
 *     push {r4-r8}; r3-r8, ip = r1; sub r2, #32
 *  1: stm r0!, {r1, r3-r8, ip}; subs r2, #32; bgt 1b
 *     pop {r4-r8}; adds r2, #32; bxeq lr
 *  tail: str r1, [r0], #4; subs r2, #4; bgt tail
 * Exit as the recompiled routine leaves it: r0 advanced, r2 and the flags
 * from the last subs/adds, r3 = r12 = r1 after blocks (else unchanged),
 * r4-r8 popped (pushed: 0, 0, 0, 0, r8), else r4-r7 = 0. */
void native_memset32(void) {
    uint32_t d = cpu.r[0], v = cpu.r[1], n = cpu.r[2], sp = cpu.r[13];
    uint32_t nb = 0, nt, r2 = n;
    int blk = (int32_t)n >= 32;
    if (blk) {
        nb = blocks32(n);
        r2 = n - 32 * nb;
        flags_add(r2 - 32, 32);
        sp_wr32(sp - 20, 0);
        sp_wr32(sp - 16, 0);
        sp_wr32(sp - 12, 0);
        sp_wr32(sp - 8, 0);
        sp_wr32(sp - 4, cpu.r[8]);
    }
    nt = (blk && r2 == 0) ? 0 : tail_count(r2, 4);
    uint32_t total = 32 * nb + 4 * nt;
    if ((d & 3) || n > BIG_COUNT) {
        for (uint32_t k = 0; k < total / 4; k++)
            wr32(d + 4 * k, v);
    } else {
        fill_words(d, v, total / 4);
    }
    d += total;
    if (nt) {
        uint32_t fa = r2 - 4 * (nt - 1);
        flags_sub(fa, 4);
        r2 = fa - 4;
    }
    cpu.r[0] = d;
    cpu.r[2] = r2;
    cpu.r[4] = cpu.r[5] = cpu.r[6] = cpu.r[7] = 0;
    if (blk) {
        cpu.r[3] = v;
        cpu.r[12] = v;
        cpu.r[4] = sp_rd32(sp - 20);
        cpu.r[5] = sp_rd32(sp - 16);
        cpu.r[6] = sp_rd32(sp - 12);
        cpu.r[7] = sp_rd32(sp - 8);
        cpu.r[8] = sp_rd32(sp - 4);
    }
}

/* 0x03000858 (ARM, IWRAM) memset16: r0 = dst, r1 = halfword, r2 = bytes.
 *  1: strh r1, [r0], #2; subs r2, #2; bgt 1b
 * Exit: r0 advanced, r2 and the flags from the last subs. */
void native_memset16(void) {
    uint32_t d = cpu.r[0], v = cpu.r[1] & 0xFFFFu, n = cpu.r[2];
    uint32_t nh = tail_count(n, 2);
    if ((d & 1) || n > BIG_COUNT || !plain_range(d, 2 * nh)) {
        for (uint32_t k = 0; k < nh; k++)
            wr16(d + 2 * k, v);
    } else {
        uint32_t a = d, left = nh;
        if (a & 2) {
            wr16(a, v);
            a += 2;
            left--;
        }
        fill_words(a, v | (v << 16), left / 2);
        a += 4 * (left / 2);
        if (left & 1)
            wr16(a, v);
    }
    uint32_t fa = n - 2 * (nh - 1);
    flags_sub(fa, 2);
    cpu.r[0] = d + 2 * nh;
    cpu.r[2] = fa - 2;
}

/* ------------------------------------------------------------ LZ decoder */

/* ARM shifts by a register: the bottom byte counts, 32 and more give 0 */
static inline uint32_t arm_lsl(uint32_t v, uint32_t n) {
    n &= 0xFFu;
    return n >= 32 ? 0 : v << n;
}
static inline uint32_t arm_lsr(uint32_t v, uint32_t n) {
    n &= 0xFFu;
    return n >= 32 ? 0 : v >> n;
}

/* 0x03000040 (ARM, IWRAM): the engine's bit-stream LZ decoder (LoG2
 * 0x03000000; the game's resource, map chunk and sprite decompression).
 * r0 = destination (bytes, advanced), r1 = source (words, advanced), r3 =
 * the literal width in bits, r4 = the literal base (until a code changes
 * it).  The bits come MSB first from 32-bit words: r6 holds them, sl counts
 * those left.  Codes:
 *   1                literal: r3 bits + r4
 *   01 gamma ...     match: gamma 2 = the last offset again, else offset =
 *                    (gamma - 3) << r5 | r5 bits; then a gamma length,
 *                    longer for far offsets
 *   001 x(7)         x != 0: offset x, length 2 bits + 2; x = 0: 2 bits y,
 *                    y = 0 ends, else r5 = (y + 3 bits)
 *   000 n(4)         n = 1: a 0 byte; n >= 2: the byte n - 1 back; n = 0:
 *                    1 -> 256 bytes of 8 bits (repeated while 1 follows),
 *                    0 then 0 -> r4 = 8 bits, r3 = 7, 0 then 1 -> r3 = 8
 * The routine is transcribed with its registers; the bit reader at
 * 0x0300028C is inlined (its flags are never read).  Exit as the
 * recompiled routine leaves it: r0, r1, r12 (last offset) from the
 * decoder, r4-r11 and lr popped (pushed: r4, 0, r6, 0, 0, r9, 0, 0, lr;
 * r5, r7, r8, r10, r11 are not inputs), flags of the final 'cmp r7, #0'. */
void native_lz_decode(void) {
    uint32_t r0 = cpu.r[0], r1 = cpu.r[1], r3 = cpu.r[3], r4 = cpu.r[4], r6 = cpu.r[6];
    uint32_t sp = cpu.r[13];
    sp_wr32(sp - 36, r4);
    sp_wr32(sp - 32, 0);
    sp_wr32(sp - 28, r6);
    sp_wr32(sp - 24, 0);
    sp_wr32(sp - 20, 0);
    sp_wr32(sp - 16, cpu.r[9]);
    sp_wr32(sp - 12, 0);
    sp_wr32(sp - 8, 0);
    sp_wr32(sp - 4, cpu.r[14]);
    uint32_t r5 = 8, sl = 0, ip = 1, r7, r8, sb, fp, c;
/* one bit into c: subs sl, #1; ldrmi r6, [r1], #4; movmi sl, #31; adds r6, r6, r6 */
#define BIT() do {                                      \
        sl--;                                           \
        if ((int32_t)sl < 0) {                          \
            r6 = rd32(r1);                              \
            r1 += 4;                                    \
            sl = 31;                                    \
        }                                               \
        c = r6 >> 31;                                   \
        r6 += r6;                                       \
    } while (0)
/* r7 = the next n bits (0x0300028C) */
#define BITS(n) do {                                    \
        r8 = (n);                                       \
        if (sl == 0) {                                  \
            r6 = rd32(r1);                              \
            r1 += 4;                                    \
            sl = 32;                                    \
        }                                               \
        fp = r8 > sl ? sl : r8;                         \
        r7 = arm_lsr(r6, 32 - fp);                      \
        r6 = arm_lsl(r6, fp);                           \
        sl -= fp;                                       \
        r8 -= fp;                                       \
        if (r8) {                                       \
            r6 = rd32(r1);                              \
            r1 += 4;                                    \
            sl = 32 - r8;                               \
            fp = arm_lsr(r6, sl);                       \
            r7 = fp + arm_lsl(r7, r8);                  \
            r6 = arm_lsl(r6, r8);                       \
        }                                               \
    } while (0)
/* sb = 1, then 'BIT; sb = 2 * sb + c; BIT' while that second bit is 1 */
#define GAMMA() do {                                    \
        sb = 1;                                         \
        do {                                            \
            BIT();                                      \
            sb = sb + sb + c;                           \
            BIT();                                      \
        } while (c);                                    \
    } while (0)
    (void)fp;
    goto next;
literal:                                    /* 0x03000054 */
    BITS(r3);
    r7 += r4;
store:                                      /* 0x03000060 */
    wr8(r0, r7);
    r0++;
next:                                       /* 0x03000064 */
    BIT();
    if (c)
        goto literal;
    BIT();
    if (c)
        goto match;
    BIT();
    if (!c)
        goto short_match;
    BITS(4);                                /* 0x030000A0 */
    r7 -= 1;
    if (r7 == 0)
        goto store;
    if ((int32_t)r7 >= 0) {                 /* 0x03000128: a byte r7 back */
        r7 = rd8(r0 - r7);
        goto store;
    }
    BIT();                                  /* 0x030000B4 */
    if (!c)
        goto set_literal;
    do {                                    /* 0x030000C8: runs of 256 raw bytes */
        sb = 0x100;
        do {
            BITS(8);
            wr8(r0, r7);
            r0++;
        } while (--sb);
        BIT();
    } while (c);
    goto next;
set_literal:                                /* 0x030000F8 */
    r4 = 0;
    BIT();
    r3 = 7 + c;
    if (r3 == 8)
        goto next;
    BITS(8);
    r4 = r7;
    goto next;
short_match:                                /* 0x03000130 */
    BITS(7);
    if (r7 == 0) {
        BITS(2);                            /* 0x0300015C */
        if (r7 == 0)
            goto done;
        BITS(r7 + 3);
        r5 = r7;
        goto next;
    }
    ip = r7;
    BITS(2);
    sb = r7 + 2;
    goto copy;
match:                                      /* 0x0300017C */
    GAMMA();
    if (sb == 2) {                          /* the last offset again */
        GAMMA();
        goto copy;
    }
    sb -= 3;                                /* 0x030001E8 */
    BITS(r5);
    ip = r7 + arm_lsl(sb, r5);
    GAMMA();
    if (ip >= 0x10000u)
        sb += 3;
    else if (ip >= 0x37FFu)
        sb += 2;
    else if (ip >= 0x27Fu)
        sb += 1;
    else if (ip <= 0x7Fu)
        sb += 4;
copy:                                       /* 0x03000264 / 0x03000278 */
    do {
        r8 = rd8(r0 - ip);
        wr8(r0, r8);
        r0++;
    } while (--sb);
    goto next;
done:                                       /* 0x030002D4 */
#undef BIT
#undef BITS
#undef GAMMA
    cpu.r[0] = r0;
    cpu.r[1] = r1;
    cpu.r[3] = r3;
    cpu.r[4] = sp_rd32(sp - 36);
    cpu.r[5] = sp_rd32(sp - 32);
    cpu.r[6] = sp_rd32(sp - 28);
    cpu.r[7] = sp_rd32(sp - 24);
    cpu.r[8] = sp_rd32(sp - 20);
    cpu.r[9] = sp_rd32(sp - 16);
    cpu.r[10] = sp_rd32(sp - 12);
    cpu.r[11] = sp_rd32(sp - 8);
    cpu.r[12] = ip;
    cpu.r[14] = sp_rd32(sp - 4);
    cpu.n = 0;
    cpu.z = 1;
    cpu.c = 1;
    cpu.v = 0;
}

/* 0x03000388 (ARM, IWRAM): a linear 8bpp bitmap {+0x24 base, +0x28 width
 * = stride, +0x2C height} to 8x8 tiles at r1 (64 bytes each, advanced),
 * band by band, each tile as 4 groups of two 8-byte rows (ldm, ldm, stm).
 * Exit as the recompiled routine leaves it: r1 advanced, r2 = base +
 * height * stride, r3 = stride, r4-r11 popped (pushed as 0: not inputs),
 * flags of the last 'subs r4, #8'. */
void native_bitmap_tiles(void) {
    uint32_t bm = cpu.r[0], r1 = cpu.r[1], sp = cpu.r[13];
    uint32_t base = rd32(bm + 0x24), stride = rd32(bm + 0x28), height = rd32(bm + 0x2C);
    for (int i = 0; i < 8; i++)
        sp_wr32(sp - 32 + 4 * (uint32_t)i, 0);
    /* the loops run while 'subs #8' leaves non-zero: counts 0 or not a
     * multiple of 8 never end in the original either */
    uint32_t r4 = height, fa = 0;
    do {
        uint32_t r5 = stride, r6 = base;
        do {
            uint32_t r7 = r6;
            /* ldm/stm as the recompiled routine does them (rd32/wr32 of
             * the addresses as they are) */
            for (int g = 0; g < 4; g++) {
                uint32_t a = rd32(r7), b = rd32(r7 + 4);
                r7 += stride;
                uint32_t c = rd32(r7), d = rd32(r7 + 4);
                r7 += stride;
                wr32(r1, a); wr32(r1 + 4, b); wr32(r1 + 8, c); wr32(r1 + 12, d);
                r1 += 16;
            }
            r6 += 8;
            r5 -= 8;
        } while (r5);
        base += stride * 8;
        fa = r4;
        r4 -= 8;
    } while (r4);
    flags_sub(fa, 8);
    cpu.r[1] = r1;
    cpu.r[2] = base;
    cpu.r[3] = stride;
    cpu.r[4] = sp_rd32(sp - 32);
    cpu.r[5] = sp_rd32(sp - 28);
    cpu.r[6] = sp_rd32(sp - 24);
    cpu.r[7] = sp_rd32(sp - 20);
    cpu.r[8] = sp_rd32(sp - 16);
    cpu.r[9] = sp_rd32(sp - 12);
    cpu.r[10] = sp_rd32(sp - 8);
    cpu.r[11] = sp_rd32(sp - 4);
}

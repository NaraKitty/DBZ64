/*
 * native.c - Buu's Fury: native replacements for hot IWRAM routines
 * (game.toml [[native]]; the mixer natives shared with LoG2 are in
 * runtime/native.c).  Each must leave memory and the registers and flags
 * the guest routine leaves exactly as it does.
 */
#include "gba.h"

/* 0x030007FC (ARM, IWRAM) memcpy16: r0 = dst, r1 = src, r2 = bytes.  A
 * Duff's device: 'add pc, pc, r3, lsl #2' with r3 = (16 - r2) & 0xE enters
 * an unrolled block of eight ldrh/strh pairs, skipping r3 / 2 of them, then
 * 'subs r2, r2, #16; bgt' repeats whole blocks.  So it copies halfwords
 * forward (overlap behaves like the loop), rounds odd sizes up and copies 16
 * bytes for r2 = 0.  Exit: r0/r1 advanced, r2 = what the last subs left,
 * r3 = the last halfword, flags from that subs.  The game's tile and map
 * streamers spend most of their time here. */
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
    uint32_t so = s & 0xFFFu, dof = d & 0xFFFu;
    uint8_t *hs = NULL, *hd = NULL;
    /* fast path: aligned, both ranges inside one 4KB page each (the common
     * case: a tile or map row) and the pages are plain RAM / VRAM */
    if (!((s | d) & 1) && so + bytes <= 0x1000u && dof + bytes <= 0x1000u) {
        gba_mem_touch(d, bytes);             /* VRAM: mark dirty and writable */
        hs = gba_host_ptr(s);
        hd = gba_host_wptr(d);
    }
    if (hs && hd) {
        uint8_t *ps = hs - so, *pd = hd - dof;  /* page bases */
        uint32_t k = 0;
        if (((so ^ dof) & 3) == 0) {
            /* same alignment: guest words are host words, so whole words
             * can move.  Equivalent to the halfword loop even when the
             * ranges overlap: the distance is a multiple of 4. */
            if (so & 2) {
                *(uint16_t *)(pd + (dof ^ GBA_HX)) = *(const uint16_t *)(ps + (so ^ GBA_HX));
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
            *(uint16_t *)(pd + ((dof + 2 * k) ^ GBA_HX)) = *(const uint16_t *)(ps + ((so + 2 * k) ^ GBA_HX));
        /* r3 = the last halfword loaded, which is the last one stored */
        v = *(const uint16_t *)(pd + ((dof + 2 * (count - 1)) ^ GBA_HX));
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

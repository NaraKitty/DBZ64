/*
 * native.c - LoG2-only native replacements (game.toml [[native]]), in
 * addition to the engine routines shared with Buu's Fury in runtime/native.c.
 * Same contract: each one produces exactly the guest routine's results (the
 * memory it writes and the registers and flags its callers may read).
 */
#include "gba.h"
#include "io.h"

/* 0x08011F88 (Thumb): the HBlank IRQ handler of the mode-1 world map.  It
 * copies the next line's BG2 affine parameters, a 16-byte entry {PA|PB,
 * PC|PD, X, Y} of the per-line table at [View+0x6C] (View = 0x03001090, read
 * from ROM 0x086D7ED8) indexed by VCOUNT+1, to 0x04000020..0x0400002F.  The
 * guest code does that with a VCOUNT load and four word stores to I/O, all
 * emulated accesses on N64 (five traps per line, 228 lines per frame).
 *
 *   push {r4}; r0 = VCOUNT (ldrb); r1 = [[0x086D7ED8] + 0x6C];
 *   r0 = r1 + ((r0 + 1) << 4) (adds: the flags); ldm r0, {r0-r3};
 *   stm 0x04000020!, {r0-r3}; pop {r4}; bx lr
 */
void native_m1_hblank(void) {
    /* push {r4}: r4 is not an input, the recompiled routine pushes 0 */
    wr32(cpu.r[13] - 4, 0);
    uint32_t vc = gba_io_read8(REG_VCOUNT);
    uint32_t tab = rd32(rd32(0x086D7ED8u) + 0x6Cu);
    uint32_t off = (vc + 1) << 4;
    uint32_t e = tab + off;
    uint32_t w0 = rd32(e & ~3u), w1 = rd32((e & ~3u) + 4), w2 = rd32((e & ~3u) + 8),
             w3 = rd32((e & ~3u) + 12);
    /* gba_io_write32() of 0x20..0x2C: plain register stores (no side
     * effects), so the native 32-bit words of the register file are written
     * directly */
    gba_mem.io[REG_BG2PA >> 2] = w0;
    gba_mem.io[(REG_BG2PA >> 2) + 1] = w1;
    gba_mem.io[REG_BG2X >> 2] = w2;
    gba_mem.io[(REG_BG2X >> 2) + 1] = w3;
    cpu.r[0] = w0;
    cpu.r[1] = w1;
    cpu.r[2] = w2;
    cpu.r[3] = w3;
    cpu.n = e >> 31;
    cpu.z = e == 0;
    cpu.c = e < tab;
    cpu.v = ((tab ^ e) & (off ^ e)) >> 31;
}

/* n words from guest RAM src to VRAM dst like a 32-bit incrementing DMA,
 * marking VRAM dirty only when the contents change */
static void copy32_to_vram(uint32_t dst, uint32_t src, uint32_t n) {
    uint32_t *hs = gba_host_ptr(src), *hd = gba_host_wptr(dst);
    if (hs && hd && !((src | dst) & 3) && (src & 0xFFFu) + 4 * n <= 0x1000u &&
        (dst & 0xFFFu) + 4 * n <= 0x1000u) {
        uint32_t diff = 0;
        for (uint32_t k = 0; k < n; k++)
            diff |= hs[k] ^ hd[k];
        if (diff) {
            gba_mem_touch(dst, 4 * n);
            for (uint32_t k = 0; k < n; k++)
                hd[k] = hs[k];
        }
        return;
    }
    for (uint32_t k = 0; k < n; k++)
        wr32(dst + 4 * k, rd32(src + 4 * k));
}

/* 0x0801278A (Thumb): the mode-1 sky panorama (layer obj, vt 0x08023A00)
 * into its 32x32 hardware map.  r0 = obj (+4 BG, +8 two 32x32 halves of a
 * 64-column panorama), r1 = scroll s.  With h = bit 8 of s and c = (s >> 3)
 * & 0x1E, each of the 32 rows gets columns c..31 from half h and, if c != 0,
 * columns 0..c-1 from half h^1: one DMA3 (32-bit, immediate) per row
 * segment, 64 with four emulated I/O stores each.  The DMA registers end as
 * the last DMA leaves them.  Outputs as the recompiled routine leaves them:
 * r0-r7, r12, r14 and the flags; its stack frame {r3 slot = h^1, r4-r7 = 0
 * (not inputs), lr}. */
void native_m1_sky_rows(void) {
    uint32_t obj = cpu.r[0], s = cpu.r[1], sp = cpu.r[13];
    uint32_t h = (s >> 8) & 1, c = ((uint32_t)((int32_t)s >> 3)) & 0x1E;
    uint32_t map = 0x0600E000u + (rd8(obj + 4) << 11);
    uint32_t n1 = (32 - c) >> 1, n2 = c >> 1;
    wr32(sp - 24, h ^ 1);
    wr32(sp - 20, 0);
    wr32(sp - 16, 0);
    wr32(sp - 12, 0);
    wr32(sp - 8, 0);
    wr32(sp - 4, cpu.r[14]);
    uint32_t src = 0, dst = 0, n;
    for (uint32_t i = 0; i < 32; i++) {
        src = obj + h * 0x800 + i * 64 + c * 2 + 8;
        dst = map + i * 64 + c * 2;
        copy32_to_vram(dst, src, n1);
    }
    n = n1;
    if (c) {
        for (uint32_t i = 0; i < 32; i++) {
            src = obj + (h ^ 1) * 0x800 + i * 64 + 8;
            dst = map + i * 64;
            copy32_to_vram(dst, src, n2);
        }
        n = n2;
    }
    gba_dma_settle32(3, src, dst, n);
    cpu.r[0] = c ? obj + (h ^ 1) * 0x800 : obj;
    cpu.r[1] = 32;
    cpu.r[2] = c ? n2 : 0;
    cpu.r[3] = h ^ 1;
    cpu.r[4] = cpu.r[5] = cpu.r[6] = cpu.r[7] = 0;
    cpu.r[12] = n1;
    cpu.r[14] = obj + h * 0x800;
    /* the last flag-setting instruction: cmp r1, #0x20 (r1 = 32), or with
     * c = 0 the cmp r4, #0 after the first loop; both leave Z and C set */
    cpu.n = 0;
    cpu.z = 1;
    cpu.c = 1;
    cpu.v = 0;
}

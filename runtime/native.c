/*
 * native.c - native C replacements for hot hand-written guest routines.
 *
 * tools/game.toml [[native]] makes the recompiler emit a call to these
 * instead of translating the guest code.  Each one must produce exactly the
 * guest routine's results (memory and the registers its callers may read);
 * the host harness checks this by comparing audio output with and without
 * the replacement.
 */
#include "gba.h"

/* 0x03000520 (ARM, IWRAM): mix n samples of one voice into the 32-bit
 * accumulator.  r0 = voice {pos, step, data, vol}, r1 = acc, r2 = n.
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

/* 0x03000704 (ARM, IWRAM): convert n accumulated samples (n even) to the
 * two signed 8-bit FIFO rings, clearing the accumulator.  r0 = left ring
 * (low halves), r1 = right ring (high halves), r2 = acc, r3 = n. */
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

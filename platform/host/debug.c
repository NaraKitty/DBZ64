/*
 * debug.c - host debug build (DEBUG=1 platform/host/build.sh, generated code
 * from tools/recomp.py --trace): every guest function entry goes through
 * gba_trace_enter().
 *
 *  - The last 4096 entries (address, lr, r0) are kept and printed when the
 *    run dies (gba_panic -> plat_fatal).
 *  - GBA_INTERP_FN=lo-hi[,lo-hi...] (hex): functions entered in one of the
 *    ranges run in the interpreter instead of their recompiled code, and so
 *    does everything they call.  Bisecting the ranges finds the function
 *    whose recompiled code misbehaves; 08000000-08000001 (the reset entry)
 *    interprets the whole game.
 *
 * Callees of an interpreted function must be interpreted too: recompiled
 * code keeps the registers it saves and restores in C locals and does not
 * write them back to cpu.r[], which an interpreted caller would see as
 * clobbered.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba.h"
#include "interp.h"
#include "recomp.h"

#define RING 4096u
static uint32_t ring_pc[RING], ring_lr[RING], ring_r0[RING];
static unsigned ring_pos;

static int nranges = -1;
static uint32_t range_lo[64], range_hi[64];
static int interp_depth;

static void init_ranges(void) {
    nranges = 0;
    const char *s = getenv("GBA_INTERP_FN");
    if (!s)
        return;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ","); t && nranges < 64; t = strtok(NULL, ",")) {
        unsigned lo, hi;
        if (sscanf(t, "%x-%x", &lo, &hi) == 2) {
            range_lo[nranges] = lo;
            range_hi[nranges] = hi;
            nranges++;
        }
    }
    fprintf(stderr, "[debug] %d interpreter ranges\n", nranges);
}

int gba_trace_enter(uint32_t addr) {
    unsigned k = ring_pos++ & (RING - 1);
    ring_pc[k] = addr;
    ring_lr[k] = cpu.r[14];
    ring_r0[k] = cpu.r[0];
    if (nranges < 0)
        init_ranges();
    int interp = interp_depth > 0;
    for (int i = 0; i < nranges && !interp; i++)
        if ((addr & ~1u) >= range_lo[i] && (addr & ~1u) < range_hi[i])
            interp = 1;
    if (!interp)
        return 0;
    interp_depth++;
    gba_interp_call(addr);
    interp_depth--;
    return 1;
}

static void trace_dump(FILE *f, unsigned n) {
    if (n > RING)
        n = RING;
    if (n > ring_pos)
        n = ring_pos;
    for (unsigned k = ring_pos - n; k != ring_pos; k++)
        fprintf(f, "  %08X lr=%08X r0=%08X\n", (unsigned)ring_pc[k & (RING - 1)],
                (unsigned)ring_lr[k & (RING - 1)], (unsigned)ring_r0[k & (RING - 1)]);
}

__attribute__((noreturn)) void plat_fatal(const char *msg) {
    fprintf(stderr, "FATAL: %s\nlast function entries (oldest first):\n", msg);
    trace_dump(stderr, 200);
    abort();
}

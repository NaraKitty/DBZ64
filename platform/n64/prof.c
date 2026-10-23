/*
 * prof.c - sampling profiler for test runs (enabled by "profile F" in
 * rom:/test.txt).  A timer interrupt records the interrupted PC in a
 * histogram of 32-byte code lines; the report prints the hottest lines as
 * raw addresses, to be symbolised offline (tools/prof_report.sh runs
 * addr2line, which also resolves inlined functions).
 */
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>
#include "prof.h"

#define SLOTS 8192

static uint32_t *line_pc, *line_hits;
static uint32_t total, dropped;
static timer_link_t *timer;

static void tick(int ovfl) {
    (void)ovfl;
    uint32_t line = (uint32_t)C0_EPC() & ~31u;
    total++;
    uint32_t h = (line >> 5) * 2654435761u >> 19;   /* 13-bit hash */
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & (SLOTS - 1)) {
        if (line_pc[h] == line) { line_hits[h]++; return; }
        if (!line_pc[h]) { line_pc[h] = line; line_hits[h] = 1; return; }
    }
    dropped++;
}

void prof_start(int period_us) {
    line_pc = calloc(SLOTS, sizeof(uint32_t));
    line_hits = calloc(SLOTS, sizeof(uint32_t));
    timer_init();
    timer = new_timer(TIMER_TICKS(period_us), TF_CONTINUOUS, tick);
}

void prof_report(void) {
    if (!timer)
        return;
    delete_timer(timer);
    timer = NULL;
    debugf("PROFILE %lu samples (%lu dropped)\n", (unsigned long)total, (unsigned long)dropped);
    for (int k = 0; k < 120; k++) {
        int best = -1;
        for (int i = 0; i < SLOTS; i++)
            if (line_hits[i] && (best < 0 || line_hits[i] > line_hits[best]))
                best = i;
        if (best < 0)
            break;
        debugf("PROF %lu %08lX\n", (unsigned long)line_hits[best], (unsigned long)line_pc[best]);
        line_hits[best] = 0;
    }
}

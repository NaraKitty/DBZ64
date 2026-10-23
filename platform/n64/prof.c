/*
 * prof.c - sampling profiler for test runs (enabled by "profile" in
 * rom:/test.txt).  A timer interrupt records the interrupted PC; samples in
 * recompiled code are attributed to their guest function, everything else
 * is kept by raw PC so it can be symbolised with addr2line afterwards.
 */
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>
#include "gba.h"
#include "dispatch.h"
#include "prof.h"

typedef struct { uint32_t host, guest; } fnmap_t;

static fnmap_t *map;
static uint32_t *hits;
static int nmap;
static uint32_t lo_host, hi_host;
static uint32_t other_pc[64], other_hits[64];
static uint32_t total, n_other, n_dropped;
static timer_link_t *timer;

static int cmp_host(const void *a, const void *b) {
    uint32_t x = ((const fnmap_t *)a)->host, y = ((const fnmap_t *)b)->host;
    return x < y ? -1 : x > y;
}

static void tick(int ovfl) {
    (void)ovfl;
    uint32_t pc = (uint32_t)C0_EPC();
    total++;
    if (pc >= lo_host && pc < hi_host) {
        int lo = 0, hi = nmap - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (map[mid].host <= pc) lo = mid; else hi = mid - 1;
        }
        hits[lo]++;
        return;
    }
    /* outside generated code: bucket by 64-byte line */
    uint32_t line = pc & ~63u;
    for (int i = 0; i < 64; i++) {
        if (other_pc[i] == line) { other_hits[i]++; n_other++; return; }
        if (!other_pc[i]) { other_pc[i] = line; other_hits[i] = 1; n_other++; return; }
    }
    n_dropped++;
}

void prof_start(int period_us) {
    nmap = (int)gba_func_count;
    map = malloc(sizeof(fnmap_t) * (size_t)nmap);
    hits = calloc((size_t)nmap, sizeof(uint32_t));
    for (int i = 0; i < nmap; i++) {
        map[i].host = (uint32_t)(uintptr_t)gba_func_table[i].fn;
        map[i].guest = gba_func_table[i].addr;
    }
    qsort(map, (size_t)nmap, sizeof(fnmap_t), cmp_host);
    lo_host = map[0].host;
    hi_host = map[nmap - 1].host + 0x4000;   /* last function: generous bound */
    timer_init();
    timer = new_timer(TIMER_TICKS(period_us), TF_CONTINUOUS, tick);
}

void prof_report(void) {
    if (!timer)
        return;
    delete_timer(timer);
    timer = NULL;
    debugf("PROFILE %lu samples (%lu outside generated code, %lu dropped)\n",
           (unsigned long)total, (unsigned long)n_other, (unsigned long)n_dropped);
    for (int k = 0; k < 40; k++) {
        int best = -1;
        for (int i = 0; i < nmap; i++)
            if (hits[i] && (best < 0 || hits[i] > hits[best]))
                best = i;
        if (best < 0)
            break;
        debugf("PROF %5.2f%% guest %08lX\n", 100.0 * hits[best] / total, (unsigned long)map[best].guest);
        hits[best] = 0;
    }
    for (int k = 0; k < 30; k++) {
        int best = -1;
        for (int i = 0; i < 64; i++)
            if (other_hits[i] && (best < 0 || other_hits[i] > other_hits[best]))
                best = i;
        if (best < 0)
            break;
        debugf("PROF %5.2f%% host %08lX\n", 100.0 * other_hits[best] / total, (unsigned long)other_pc[best]);
        other_hits[best] = 0;
    }
}

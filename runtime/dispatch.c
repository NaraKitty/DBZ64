/*
 * dispatch.c - guest address -> recompiled function lookup, indirect calls
 * and jumps, interpreter fallback for addresses with no recompiled code.
 */
#include <stdio.h>
#include <stdlib.h>
#include "gba.h"
#include "dispatch.h"
#include "interp.h"

gba_cpu_t cpu;

#define HASH_BITS 13
#define HASH_SIZE (1u << HASH_BITS)

static struct { uint32_t key; gba_func_t fn; } htab[HASH_SIZE];

static inline uint32_t hash_key(uint32_t k) {
    return ((k >> 1) * 2654435761u) >> (32 - HASH_BITS);
}

void gba_dispatch_init(void) {
    if (gba_func_count * 2 > HASH_SIZE) {
        fprintf(stderr, "dispatch: hash table too small (%u functions)\n", gba_func_count);
        abort();
    }
    for (unsigned i = 0; i < gba_func_count; i++) {
        uint32_t k = gba_func_table[i].addr;
        uint32_t h = hash_key(k);
        while (htab[h].fn)
            h = (h + 1) & (HASH_SIZE - 1);
        htab[h].key = k;
        htab[h].fn = gba_func_table[i].fn;
    }
}

/* recent targets: indirect calls (call_via_rX veneers, vtables) hit a few
 * dozen functions per frame, while the 64KB hash table misses the D-cache */
#define RECENT 256
static struct { uint32_t key; gba_func_t fn; } recent[RECENT];

gba_func_t gba_lookup(uint32_t addr, int thumb) {
    uint32_t k = (addr & ~1u) | (thumb ? 1u : 0u);
    uint32_t c = (k >> 1) & (RECENT - 1);
    if (recent[c].fn && recent[c].key == k)
        return recent[c].fn;
    uint32_t h = hash_key(k);
    while (htab[h].fn) {
        if (htab[h].key == k) {
            recent[c].key = k;
            recent[c].fn = htab[h].fn;
            return htab[h].fn;
        }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return NULL;
}

/* statistics for coverage work */
uint32_t gba_miss_log[256];
unsigned gba_miss_count;

static void note_miss(uint32_t target) {
    for (unsigned i = 0; i < gba_miss_count; i++)
        if (gba_miss_log[i] == target)
            return;
    if (gba_miss_count < 256)
        gba_miss_log[gba_miss_count++] = target;
#ifndef N64
    fprintf(stderr, "[dispatch] no recompiled code for %08X (%s)\n",
            target & ~1u, (target & 1) ? "thumb" : "arm");
#endif
}

/* Call: cpu.r[14] already holds the return address. */
void gba_call(uint32_t target) {
    gba_func_t fn = gba_lookup(target, target & 1);
    if (fn) {
        fn();
        return;
    }
    note_miss(target);
    gba_interp_call(target);
}

/* Jump with lr unchanged: run a known function; 0 = unknown, caller unwinds. */
int gba_jump(uint32_t target) {
    gba_func_t fn = gba_lookup(target, target & 1);
    if (!fn)
        return 0;
    fn();
    return 1;
}

/* Top level: run guest code from 'target' forever (or until it returns to
 * the sentinel link value).  Unknown continuation addresses unwound to here
 * are executed by the interpreter, which calls back into recompiled code. */
void gba_run_from(uint32_t target, uint32_t sentinel_lr) {
    cpu.r[14] = sentinel_lr;
    uint32_t t = target;
    for (;;) {
        gba_func_t fn = gba_lookup(t, t & 1);
        if (fn) {
            fn();
        } else {
            note_miss(t);
            gba_interp_jump(t);
        }
        t = cpu.next_pc;
        if (((t ^ sentinel_lr) >> 1) == 0)
            return;
    }
}

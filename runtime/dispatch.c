/*
 * dispatch.c - guest address -> recompiled function lookup, indirect calls
 * and jumps, interpreter fallback for addresses with no recompiled code.
 */
#include <stdio.h>
#include <stdlib.h>
#include "gba.h"
#include "dispatch.h"
#include "interp.h"
#include "game.h"

gba_cpu_t cpu;

#define HASH_BITS GAME_DISPATCH_HASH_BITS
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

gba_func_t gba_lookup(uint32_t addr, int thumb) {
    uint32_t k = (addr & ~1u) | (thumb ? 1u : 0u);
    uint32_t h = hash_key(k);
    while (htab[h].fn) {
        if (htab[h].key == k)
            return htab[h].fn;
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return NULL;
}

/* ABI summary of the function at target: gba_func_table is sorted by key
 * (address | Thumb bit), gba_func_mod is parallel to it.  Unknown targets
 * may change everything. */
static uint32_t func_mod(uint32_t target) {
    unsigned lo = 0, hi = gba_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (gba_func_table[mid].addr < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < gba_func_count && gba_func_table[lo].addr == target)
        return gba_func_mod[lo];
    return 0xFFFFFFFFu;
}

void gba_save_regs(gba_saved_regs_t *s) {
    for (int i = 0; i < 15; i++)
        s->r[i] = cpu.r[i];
    s->n = cpu.n;
    s->z = cpu.z;
    s->c = cpu.c;
    s->v = cpu.v;
}

void gba_restore_preserved(const gba_saved_regs_t *s, uint32_t target) {
    uint32_t mod = func_mod(target);
    for (int i = 0; i < 15; i++)
        if (!(mod & (1u << i)))
            cpu.r[i] = s->r[i];
    if (!(mod & (1u << 16))) cpu.n = s->n;
    if (!(mod & (1u << 17))) cpu.z = s->z;
    if (!(mod & (1u << 18))) cpu.c = s->c;
    if (!(mod & (1u << 19))) cpu.v = s->v;
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
            uint32_t link = cpu.r[14];
            gba_saved_regs_t keep;
            gba_save_regs(&keep);
            fn();
            if (((cpu.next_pc ^ link) >> 1) == 0)
                gba_restore_preserved(&keep, t);
        } else {
            note_miss(t);
            gba_interp_jump(t);
        }
        t = cpu.next_pc;
        if (((t ^ sentinel_lr) >> 1) == 0)
            return;
    }
}

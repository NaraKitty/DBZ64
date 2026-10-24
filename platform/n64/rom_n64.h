#ifndef ROM_N64_H
#define ROM_N64_H

#include <stdint.h>

typedef struct {
    uint32_t misses;        /* pages loaded on demand (a stall each) */
    uint32_t evictions;
    uint32_t preloaded;     /* pages loaded at boot */
} rom_cache_stats_t;

extern rom_cache_stats_t rom_cache_stats;

/* RDRAM for resident ROM pages (needs dfs_init first): 'bytes' allocated
 * from the heap, plus ram_bytes at 'ram' (16KB aligned) that nothing else
 * uses (NULL / 0 for none) */
void rom_cache_init(int bytes, void *ram, int ram_bytes);

/* load the game's hot pages (game.toml [rom] preload) while nothing else
 * runs, so play does not stall on their first use */
void rom_cache_preload(void);

#endif

#ifndef ROM_N64_H
#define ROM_N64_H

#include <stdint.h>

typedef struct {
    uint32_t misses;        /* pages loaded on demand (a stall each) */
    uint32_t evictions;
    uint32_t preloaded;     /* pages loaded at boot */
} rom_cache_stats_t;

extern rom_cache_stats_t rom_cache_stats;

/* allocate 'bytes' of RDRAM for resident ROM pages (needs dfs_init first) */
void rom_cache_init(int bytes);

/* load the game's hot pages (game.toml [rom] preload) while nothing else
 * runs, so play does not stall on their first use */
void rom_cache_preload(void);

#endif

#ifndef ROM_N64_H
#define ROM_N64_H

#include <stdint.h>

typedef struct {
    uint32_t misses;
    uint32_t evictions;
} rom_cache_stats_t;

extern rom_cache_stats_t rom_cache_stats;

/* allocate 'bytes' of RDRAM for resident ROM pages (needs dfs_init first) */
void rom_cache_init(int bytes);

#endif

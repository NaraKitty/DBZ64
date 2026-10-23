/*
 * rom_n64.c - GBA cartridge ROM paging for the N64.
 *
 * The 8MB GBA ROM lives in the N64 cartridge (DFS file rom:/rom.bin, already
 * word-swapped to the runtime's native-word layout by tools/mkrom.py).  It
 * cannot fit in RDRAM next to the recompiled code, so 4KB pages are copied
 * in by PI DMA on first access and mapped into the runtime's page tables.
 * Pages are evicted round-robin; the sample data the mixer reads every frame
 * stays resident in practice because it is touched before anything else.
 */
#include <libdragon.h>
#include <malloc.h>
#include <string.h>
#include "gba.h"
#include "rom.h"
#include "rom_n64.h"

#define PAGE      0x1000u
#define NPAGES    (GBA_ROM_SIZE / PAGE)

static pi_addr_t rom_base;
static uint8_t *slots;
static int nslots;
static int16_t page_slot[NPAGES];      /* -1 = not resident */
static int16_t *slot_page;             /* -1 = free */
static int next_victim;

rom_cache_stats_t rom_cache_stats;

void rom_cache_init(int bytes) {
    rom_base = dfs_rom_addr("/rom.bin");
    if (!rom_base)
        gba_panic("rom:/rom.bin missing from the filesystem");
    nslots = bytes / (int)PAGE;
    slots = memalign(16, (size_t)nslots * PAGE);
    if (!slots)
        gba_panic("cannot allocate the ROM cache (%d bytes)", bytes);
    slot_page = malloc(sizeof(int16_t) * (size_t)nslots);
    for (int i = 0; i < nslots; i++)
        slot_page[i] = -1;
    for (int i = 0; i < (int)NPAGES; i++)
        page_slot[i] = -1;
    next_victim = 0;
}

void gba_rom_map_init(void) {
    /* called on boot and soft reset: re-map whatever is resident */
    for (uint32_t pg = 0; pg < 0x1000; pg++) {
        int s = pg < NPAGES ? page_slot[pg] : -1;
        gba_mem_map_rom_page(0x08000 + pg, s >= 0 ? slots + (size_t)s * PAGE : NULL);
    }
}

static uint8_t *load_page(uint32_t pg) {
    int s = next_victim;
    next_victim = (next_victim + 1) % nslots;
    if (slot_page[s] >= 0) {
        page_slot[slot_page[s]] = -1;
        gba_mem_map_rom_page(0x08000 + (uint32_t)slot_page[s], NULL);
        rom_cache_stats.evictions++;
    }
    uint8_t *p = slots + (size_t)s * PAGE;
    data_cache_hit_writeback_invalidate(p, PAGE);
    dma_read(p, rom_base + pg * PAGE, PAGE);
    slot_page[s] = (int16_t)pg;
    page_slot[pg] = (int16_t)s;
    gba_mem_map_rom_page(0x08000 + pg, p);
    rom_cache_stats.misses++;
    return p;
}

uint32_t gba_rom_read32(uint32_t a) {
    uint32_t off = a & 0x01FFFFFCu;
    if (off >= GBA_ROM_SIZE) {
        uint32_t h = (a >> 1) & 0xFFFF;
        return h | ((h + 1) << 16);
    }
    uint32_t pg = off / PAGE;
    int s = page_slot[pg];
    uint8_t *p = s >= 0 ? slots + (size_t)s * PAGE : load_page(pg);
    return *(const uint32_t *)(p + (off & (PAGE - 1)));
}

const void *gba_rom_ptr(uint32_t addr, uint32_t len) {
    uint32_t off = addr & 0x01FFFFFFu;
    if (off + len > GBA_ROM_SIZE || ((off & (PAGE - 1)) + len) > PAGE)
        return NULL;
    uint32_t pg = off / PAGE;
    int s = page_slot[pg];
    uint8_t *p = s >= 0 ? slots + (size_t)s * PAGE : load_page(pg);
    return p + (off & (PAGE - 1));
}

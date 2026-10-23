/*
 * rom_n64.c - GBA cartridge ROM paging for the N64.
 *
 * The 8MB GBA ROM lives in the N64 cartridge (DFS file rom:/rom.bin, already
 * word-swapped to the runtime's native-word layout by tools/mkrom.py).  It
 * cannot fit in RDRAM next to the recompiled code, so 16KB pages are copied
 * in by PI DMA on first access.  A resident page is visible to the runtime
 * through its 4KB page tables and, in GBA_TLB builds, to generated code
 * through TLB entries installed on demand by tlb.c.
 *
 * Every miss is a stall with interrupts off (PI DMA inside the TLB
 * exception), so the game's hot pages (game.toml [rom] preload, measured on
 * the host) are loaded at boot.  When the cache is full the least recently
 * used page goes: a resident page is stamped whenever it is looked up, which
 * happens on every TLB refill and slow-path read.
 */
#include <libdragon.h>
#include <malloc.h>
#include <string.h>
#include "gba.h"
#include "rom.h"
#include "rom_n64.h"
#include "tlb.h"

#define PAGE      0x4000u
#define NPAGES    (GBA_ROM_SIZE / PAGE)

static pi_addr_t rom_base;
static uint8_t *slots;
static int nslots;
static int16_t page_slot[NPAGES];      /* -1 = not resident */
static int16_t *slot_page;             /* -1 = free */
static uint32_t *slot_used;            /* use stamp */
static uint32_t use_clock;

/* the preload list comes with the generated code (gen_rom_preload.c) */
extern const uint16_t gba_rom_preload[] __attribute__((weak));
extern const int gba_rom_preload_n __attribute__((weak));

rom_cache_stats_t rom_cache_stats;

void rom_cache_init(int bytes) {
    rom_base = dfs_rom_addr("/rom.bin");
    if (!rom_base)
        gba_panic("rom:/rom.bin missing from the filesystem");
    nslots = bytes / (int)PAGE;
    /* 16KB aligned so a slot can back a 16KB TLB page */
    slots = memalign(PAGE, (size_t)nslots * PAGE);
    if (!slots)
        gba_panic("cannot allocate the ROM cache (%d bytes)", bytes);
    slot_page = malloc(sizeof(int16_t) * (size_t)nslots);
    slot_used = calloc((size_t)nslots, sizeof(uint32_t));
    if (!slot_page || !slot_used)
        gba_panic("cannot allocate the ROM cache tables");
    for (int i = 0; i < nslots; i++)
        slot_page[i] = -1;
    for (int i = 0; i < (int)NPAGES; i++)
        page_slot[i] = -1;
}

static void map_page(uint32_t pg, const uint8_t *p) {
    for (uint32_t k = 0; k < PAGE / 0x1000u; k++)
        gba_mem_map_rom_page(0x08000 + pg * (PAGE / 0x1000u) + k, p ? p + k * 0x1000u : NULL);
}

void gba_rom_map_init(void) {
    /* called on boot and soft reset: re-map whatever is resident */
    for (uint32_t pg4 = 0; pg4 < 0x1000; pg4++)
        gba_mem_map_rom_page(0x08000 + pg4, NULL);
    for (uint32_t pg = 0; pg < NPAGES; pg++)
        if (page_slot[pg] >= 0)
            map_page(pg, slots + (size_t)page_slot[pg] * PAGE);
}

static uint8_t *load_page(uint32_t pg) {
    /* a free slot, else the least recently used one */
    int s = 0;
    for (int i = 0; i < nslots; i++) {
        if (slot_page[i] < 0) { s = i; break; }
        if (slot_used[i] < slot_used[s]) s = i;
    }
    if (slot_page[s] >= 0) {
        uint32_t old = (uint32_t)slot_page[s];
        page_slot[old] = -1;
        map_page(old, NULL);
#ifdef GBA_TLB
        tlb_rom_unmap(0x08000000u + old * PAGE);
#endif
        rom_cache_stats.evictions++;
    }
    uint8_t *p = slots + (size_t)s * PAGE;
    data_cache_hit_writeback_invalidate(p, PAGE);
    dma_read(p, rom_base + pg * PAGE, PAGE);
    slot_page[s] = (int16_t)pg;
    page_slot[pg] = (int16_t)s;
    slot_used[s] = ++use_clock;
    map_page(pg, p);
    rom_cache_stats.misses++;
    return p;
}

void rom_cache_preload(void) {
    if (!&gba_rom_preload_n)
        return;
    for (int i = 0; i < gba_rom_preload_n; i++) {
        uint32_t pg = gba_rom_preload[i];
        if (pg >= NPAGES || page_slot[pg] >= 0)
            continue;
        int free_slot = 0;
        for (int k = 0; k < nslots; k++)
            if (slot_page[k] < 0) { free_slot = 1; break; }
        if (!free_slot)
            break;
        load_page(pg);
        rom_cache_stats.misses--;
        rom_cache_stats.preloaded++;
    }
}

const void *rom_page16_load(uint32_t va16k, int load) {
    uint32_t off = va16k & 0x01FFFFFFu;
    if (off >= GBA_ROM_SIZE)
        return NULL;
    uint32_t pg = off / PAGE;
    int s = page_slot[pg];
    if (s >= 0) {
        slot_used[s] = ++use_clock;
        return slots + (size_t)s * PAGE;
    }
    return load ? load_page(pg) : NULL;
}

uint32_t gba_rom_read32(uint32_t a) {
    uint32_t off = a & 0x01FFFFFCu;
    if (off >= GBA_ROM_SIZE) {
        uint32_t h = (a >> 1) & 0xFFFF;
        return h | ((h + 1) << 16);
    }
    const uint8_t *p = rom_page16_load(0x08000000u + off, 1);
    return *(const uint32_t *)(p + (off & (PAGE - 1)));
}

const void *gba_rom_ptr(uint32_t addr, uint32_t len) {
    uint32_t off = addr & 0x01FFFFFFu;
    if (off + len > GBA_ROM_SIZE || ((off & (PAGE - 1)) + len) > PAGE)
        return NULL;
    const uint8_t *p = rom_page16_load(0x08000000u + off, 1);
    return p + (off & (PAGE - 1));
}

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
 * the host) are loaded at boot and pinned.  When the cache is full the least
 * recently used unpinned page goes (pinned ones only if nothing else is
 * left), so title screens and the attract demo cannot push gameplay data
 * out before play starts.  A resident page is stamped whenever it is looked
 * up, which happens on every TLB refill and slow-path read.
 *
 * The slots are the RAM the alignment of .bss leaves unused before it
 * (main.c, bss_gap.ld) followed by a block from the heap.
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
/* slots 0..nslots0-1 in the RAM before .bss (main.c), the rest in 'slots' */
static uint8_t *slots0, *slots;
static int nslots0, nslots;
static int16_t page_slot[NPAGES];      /* -1 = not resident */
static int16_t *slot_page;             /* -1 = free */
static uint32_t *slot_used;            /* use stamp */
static uint8_t *slot_pinned;           /* loaded by the boot preload */
static uint32_t use_clock;

/* the preload list comes with the generated code (gen_rom_preload.c) */
extern const uint16_t gba_rom_preload[] __attribute__((weak));
extern const int gba_rom_preload_n __attribute__((weak));

rom_cache_stats_t rom_cache_stats;

static inline uint8_t *slot_mem(int s) {
    return s < nslots0 ? slots0 + (size_t)s * PAGE : slots + (size_t)(s - nslots0) * PAGE;
}

void rom_cache_init(int bytes, void *ram, int ram_bytes) {
    rom_base = dfs_rom_addr("/rom.bin");
    if (!rom_base)
        gba_panic("rom:/rom.bin missing from the filesystem");
    /* 16KB aligned so a slot can back a 16KB TLB page */
    slots0 = ram;
    nslots0 = ram && !((uintptr_t)ram & (PAGE - 1)) ? ram_bytes / (int)PAGE : 0;
    nslots = nslots0 + bytes / (int)PAGE;
    slots = bytes >= (int)PAGE ? memalign(PAGE, (size_t)(nslots - nslots0) * PAGE) : NULL;
    if (bytes >= (int)PAGE && !slots)
        gba_panic("cannot allocate the ROM cache (%d bytes)", bytes);
    if (!nslots)
        gba_panic("no RAM for the ROM cache");
    slot_page = malloc(sizeof(int16_t) * (size_t)nslots);
    slot_used = calloc((size_t)nslots, sizeof(uint32_t));
    slot_pinned = calloc((size_t)nslots, 1);
    if (!slot_page || !slot_used || !slot_pinned)
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
            map_page(pg, slot_mem(page_slot[pg]));
}

static uint8_t *load_page(uint32_t pg) {
    /* a free slot, else the least recently used unpinned one, else the
     * least recently used pinned one */
    int s = -1, sp = -1;
    for (int i = 0; i < nslots; i++) {
        if (slot_page[i] < 0) { s = i; break; }
        if (slot_pinned[i]) {
            if (sp < 0 || slot_used[i] < slot_used[sp]) sp = i;
        } else if (s < 0 || slot_used[i] < slot_used[s]) {
            s = i;
        }
    }
    if (s < 0)
        s = sp;
    slot_pinned[s] = 0;
    if (slot_page[s] >= 0) {
        uint32_t old = (uint32_t)slot_page[s];
        page_slot[old] = -1;
        map_page(old, NULL);
#ifdef GBA_TLB
        tlb_rom_unmap(0x08000000u + old * PAGE);
#endif
        rom_cache_stats.evictions++;
    }
    uint8_t *p = slot_mem(s);
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
    /* pinned pages are only evicted when nothing else is left, and a page
     * loaded then is the next one to go: keep an eighth of the cache for
     * pages loaded on demand, whatever the list holds */
    int limit = nslots - nslots / 8;
    for (int i = 0; i < gba_rom_preload_n; i++) {
        uint32_t pg = gba_rom_preload[i];
        if (pg >= NPAGES || page_slot[pg] >= 0)
            continue;
        if ((int)rom_cache_stats.preloaded >= limit)
            break;
        load_page(pg);
        slot_pinned[page_slot[pg]] = 1;
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
        return slot_mem(s);
    }
    return load ? load_page(pg) : NULL;
}

#ifdef GBA_TLB
/* the 4KB page of a resident ROM address, or NULL (gba.h GBA_PAGE_RD: what
 * the page tables of non-TLB builds hold; no load, no use stamp) */
const void *gba_rom_page_host(uint32_t a) {
    uint32_t off = a & 0x00FFF000u;
    if (off >= GBA_ROM_SIZE)
        return NULL;
    int s = page_slot[off / PAGE];
    return s >= 0 ? slot_mem(s) + (off & (PAGE - 1)) : NULL;
}
#endif

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

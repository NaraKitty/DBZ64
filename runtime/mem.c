/*
 * mem.c - GBA memory map: page tables, slow paths, ROM access.
 */
#include <string.h>
#include "gba.h"
#include "io.h"
#include "rom.h"

gba_mem_t gba_mem;
uintptr_t gba_rd_page[0x10000];
uintptr_t gba_wr_page[0x10000];

/* open-bus approximation: last value seen on a ROM-ish read */
static uint32_t open_bus;

static void map_range(uint32_t base, uint32_t span, void *host, uint32_t size, int writable) {
    /* map [base, base+span) onto host[0..size) with mirroring every 'size' */
    for (uint32_t a = base; a < base + span; a += 0x1000) {
        uintptr_t p = (uintptr_t)host + ((a - base) % size);
        gba_rd_page[(a >> 12) & 0xFFFF] = p;
        gba_wr_page[(a >> 12) & 0xFFFF] = writable ? p : (p | GBA_PAGE_SLOW);
    }
}

void gba_mem_map_rom_page(uint32_t page, const void *host) {
    /* page = (addr >> 12) for 0x08000000.. ; mirrors at 0x0A and 0x0C */
    for (int m = 0; m < 3; m++) {
        uint32_t idx = (page + (uint32_t)m * 0x2000u) & 0xFFFF;
        gba_rd_page[idx] = host ? (uintptr_t)host : GBA_PAGE_SLOW;
    }
}

#ifdef GBA_TLB
/* generated code writes VRAM through the TLB: protection works on 16KB
 * pages there, so a mark covers the four 4KB pages of the TLB page */
void tlb_vram_set_writable(uint32_t page4k, int writable);
#define VRAM_GRAIN 4u
#else
#define VRAM_GRAIN 1u
#endif

void gba_mem_init(void) {
    memset(&gba_mem, 0, sizeof(gba_mem));
    for (int i = 0; i < 0x10000; i++) {
        gba_rd_page[i] = GBA_PAGE_SLOW;
        gba_wr_page[i] = GBA_PAGE_SLOW;
    }
    /* BIOS is readable only while executing inside it; reads from game code
     * return the last fetched BIOS opcode.  Keep it on the slow path. */
    map_range(0x02000000, 0x01000000, gba_mem.ewram, GBA_EWRAM_SIZE, 1);
    map_range(0x03000000, 0x01000000, gba_mem.iwram, GBA_IWRAM_SIZE, 1);
    /* 0x04 I/O: slow */
    /* palette, OAM: 1KB regions inside 4KB pages -> slow path handles them,
     * but the common case (base page) is mapped with mirroring handled by
     * the slow path for the other 3KB. */
    /* VRAM: 96KB mirrored in 128KB blocks; 64K+32K, the last 32K mirrors
     * the previous 32K.  Map page by page. */
    for (uint32_t a = 0x06000000; a < 0x07000000; a += 0x1000) {
        uint32_t off = a & 0x1FFFF;
        if (off >= 0x18000)
            off -= 0x8000;
        uintptr_t p = (uintptr_t)gba_mem.vram + off;
        gba_rd_page[(a >> 12) & 0xFFFF] = p;
        gba_wr_page[(a >> 12) & 0xFFFF] = p;
    }
    /* invariant: a page's dirty bit is set exactly when it is writable;
     * everything starts dirty, gba_vram_take_dirty() re-protects */
    gba_vram_dirty = 0xFFFFFF;
#ifdef GBA_TLB
    for (uint32_t page = 0; page < 24; page += 4)
        tlb_vram_set_writable(page, 1);
#endif
    gba_rom_map_init();
}

/* ------------------------------------------------------------ VRAM dirty */

volatile uint32_t gba_vram_dirty;

static void vram_set_protect(uint32_t page, int protect) {
    /* page 0..23 is mapped at every 128KB mirror; pages 16..23 also appear
     * at offsets 0x18000..0x1FFFF of each mirror */
    for (uint32_t base = 0x06000; base < 0x07000; base += 0x20) {
        uint32_t idx[2] = { base + page, page >= 16 ? base + page + 8 : 0 };
        for (int k = 0; k < 2; k++) {
            if (!idx[k])
                continue;
            uintptr_t p = gba_wr_page[idx[k]] & ~(uintptr_t)GBA_PAGE_SLOW;
            gba_wr_page[idx[k]] = protect ? (p | GBA_PAGE_SLOW) : p;
        }
    }
}


static inline uint32_t vram_offset(uint32_t a) {
    uint32_t off = a & 0x1FFFF;
    return off >= 0x18000 ? off - 0x8000 : off;
}

static inline void vram_mark(uint32_t a) {
    uint32_t page = (vram_offset(a) >> 12) & ~(VRAM_GRAIN - 1);
    uint32_t bits = ((1u << VRAM_GRAIN) - 1) << page;
    if ((gba_vram_dirty & bits) != bits) {
        gba_vram_dirty |= bits;
        vram_set_protect(page, 0);
#ifdef GBA_TLB
        tlb_vram_set_writable(page, 1);
#endif
    }
}

int gba_vram_track_ok = 1;

uint32_t gba_vram_take_dirty(void) {
    if (!gba_vram_track_ok)
        return 0xFFFFFF;   /* no write tracking: treat everything as dirty */
    uint32_t d = gba_vram_dirty;
    gba_vram_dirty = 0;
    for (uint32_t page = 0; page < 24; page++)
        if (d & (1u << page)) {
            vram_set_protect(page, 1);
#ifdef GBA_TLB
            if ((page & (VRAM_GRAIN - 1)) == 0)
                tlb_vram_set_writable(page, 0);
#endif
        }
    return d;
}

void gba_mem_touch(uint32_t a, uint32_t len) {
    if ((a >> 24) != 0x06 || !len)
        return;
    for (uint32_t p = a & ~0xFFFu; p < a + len; p += 0x1000)
        vram_mark(p);
}

/* ------------------------------------------------------------ slow paths */

static inline uint32_t rot_read32(uint32_t aligned_val, uint32_t a) {
    uint32_t s = (a & 3) * 8;
    return s ? (aligned_val >> s) | (aligned_val << (32 - s)) : aligned_val;
}

static uint32_t pal_oam_rd32(uint32_t a) {
    uint32_t r = a >> 24;
    if (r == 5) return gba_mem.pal[(a & 0x3FC) >> 2];
    return gba_mem.oam[(a & 0x3FC) >> 2];
}

uint32_t gba_rd32_slow(uint32_t a) {
    uint32_t al = a & ~3u;
    uint32_t v;
    uintptr_t p = gba_rd_page[(al >> 12) & 0xFFFF];
    if (!(p & GBA_PAGE_SLOW)) {
        v = *(const uint32_t *)(p + (al & 0xFFF));
        return rot_read32(v, a);
    }
    switch (al >> 24) {
    case 0x00:
        v = al < 0x4000 ? gba_mem.bios[al >> 2] : open_bus;
        break;
    case 0x04:
        v = gba_io_read32(al & 0x00FFFFFF);
        break;
    case 0x05:
    case 0x07:
        v = pal_oam_rd32(al);
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        v = gba_rom_read32(al);
        break;
    case 0x0E: case 0x0F:
        v = 0;   /* no SRAM on this cartridge */
        break;
    default:
        v = open_bus;
        break;
    }
    return rot_read32(v, a);
}

uint32_t gba_rd16_slow(uint32_t a) {
    uint32_t al = a & ~1u;
    uint32_t v;
    uintptr_t p = gba_rd_page[(al >> 12) & 0xFFFF];
    if (!(p & GBA_PAGE_SLOW)) {
        v = *(const uint16_t *)(p + ((al & 0xFFF) ^ GBA_HX));
    } else {
        switch (al >> 24) {
        case 0x04:
            v = gba_io_read16(al & 0x00FFFFFF);
            break;
        case 0x0D:
            if (gba_eeprom_active()) { v = gba_eeprom_read(); break; }
            /* fall through */
        default: {
            uint32_t w = gba_rd32_slow(al & ~3u);
            v = (al & 2) ? (w >> 16) : (w & 0xFFFF);
            break;
        }
        }
    }
    if (a & 1)  /* misaligned LDRH rotates */
        v = (v >> 8) | ((v & 0xFF) << 24);
    return v;
}

uint32_t gba_rd8_slow(uint32_t a) {
    if ((a >> 24) == 0x04)
        return gba_io_read8(a & 0x00FFFFFF);
    uint32_t w = gba_rd32_slow(a & ~3u);
    return (w >> ((a & 3) * 8)) & 0xFF;
}

void gba_wr32_slow(uint32_t a, uint32_t v) {
    a &= ~3u;
    switch (a >> 24) {
    case 0x04:
        gba_io_write32(a & 0x00FFFFFF, v);
        return;
    case 0x05:
        gba_mem.pal[(a & 0x3FC) >> 2] = v;
        return;
    case 0x06:
        vram_mark(a);
        gba_mem.vram[vram_offset(a) >> 2] = v;
        return;
    case 0x07:
        gba_mem.oam[(a & 0x3FC) >> 2] = v;
        return;
    default:
        return;  /* ROM / BIOS / unmapped: ignored */
    }
}

void gba_wr16_slow(uint32_t a, uint32_t v) {
    a &= ~1u;
    v &= 0xFFFF;
    switch (a >> 24) {
    case 0x04:
        gba_io_write16(a & 0x00FFFFFF, v);
        return;
    case 0x05:
        *(uint16_t *)((uint8_t *)gba_mem.pal + ((a & 0x3FE) ^ GBA_HX)) = (uint16_t)v;
        return;
    case 0x06:
        vram_mark(a);
        *(uint16_t *)((uint8_t *)gba_mem.vram + (vram_offset(a) ^ GBA_HX)) = (uint16_t)v;
        return;
    case 0x07:
        *(uint16_t *)((uint8_t *)gba_mem.oam + ((a & 0x3FE) ^ GBA_HX)) = (uint16_t)v;
        return;
    case 0x0D:
        gba_eeprom_write(v);
        return;
    default:
        return;
    }
}

void gba_wr8_slow(uint32_t a, uint32_t v) {
    v &= 0xFF;
    switch (a >> 24) {
    case 0x04:
        gba_io_write8(a & 0x00FFFFFF, v);
        return;
    case 0x05:
        /* 8-bit palette writes store the byte to both halves */
        gba_wr16_slow(a, v | (v << 8));
        return;
    case 0x06:
        /* BG VRAM: the byte is written to both halves of the halfword;
         * OBJ VRAM (0x10000+ in modes 0-2) ignores byte writes */
        if ((a & 0x1FFFF) < 0x10000)
            gba_wr16_slow(a & ~1u, v | (v << 8));
        return;
    case 0x07:
        return;  /* ignored by hardware */
    default:
        return;
    }
}

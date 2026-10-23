/*
 * host_ref: render GBAS state files with the reference software PPU
 * (runtime/ppu_soft.c) on the host.  Output: raw 240x160 little-endian
 * BGR555 halfwords, or, for an output name ending in ".wide.raw", the 320x240
 * view used by the test ROM's WIDE test (region (-40,-40)-(280,200) at (40,40),
 * hooks returning the hardware map entry / hardware OBJ position) rendered
 * with ppu_soft_render_view (when the runtime provides it); ".wide2.raw":
 * the same view without hooks.
 *
 *   host_ref in1.bin out1.raw [in2.bin out2.raw ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba.h"
#include "io.h"
#include "ppu_soft.h"

#if GBA_BIG_ENDIAN
#error "host_ref expects a little-endian host"
#endif

gba_mem_t gba_mem __attribute__((aligned(64)));
gba_line_regs_t gba_line_regs[160];
int gba_line_regs_valid;

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int load(const char *path) {
    static uint8_t buf[200 * 1024];
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 8 || memcmp(buf, "GBAS", 4) || le32(buf + 4) != 1) {
        fprintf(stderr, "%s: not a GBAS v1 state\n", path);
        return 0;
    }
    const uint8_t *p = buf + 8;
    memcpy(gba_mem.io, p, 0x400); p += 0x400;
    memcpy(gba_mem.pal, p, 0x400); p += 0x400;
    memcpy(gba_mem.vram, p, 0x18000); p += 0x18000;
    memcpy(gba_mem.oam, p, 0x400); p += 0x400;
    gba_line_regs_valid = (int)le32(p); p += 4;
    for (int l = 0; l < 160; l++) {
        gba_line_regs_t *s = &gba_line_regs[l];
        s->bg2pa = le16(p); s->bg2pb = le16(p + 2); s->bg2pc = le16(p + 4); s->bg2pd = le16(p + 6);
        s->bg2x = le32(p + 8); s->bg2y = le32(p + 12);
        for (int i = 0; i < 4; i++) s->hofs[i] = le16(p + 16 + 2 * i);
        for (int i = 0; i < 4; i++) s->vofs[i] = le16(p + 24 + 2 * i);
        p += 32;
    }
    return 1;
}

#ifdef WS_W
/* the same hooks as tests/n64_render/main.c (wide_test) */
static int hook_entry(int bg, int cx, int cy, uint16_t *entry) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * bg);
    uint32_t sb = ((cnt >> 8) & 31) * 0x800u, size = (cnt >> 14) & 3;
    uint32_t xc = (uint32_t)cx & ((size & 1) ? 63u : 31u), yc = (uint32_t)cy & ((size & 2) ? 63u : 31u);
    uint32_t a = sb + ((yc >> 5) * ((size & 1) ? 2u : 1u) + (xc >> 5)) * 0x800u + (yc & 31) * 64 + (xc & 31) * 2;
    a &= 0x1FFFF;
    if (a >= 0x18000) a -= 0x8000;
    *entry = *(const uint16_t *)((const uint8_t *)gba_mem.vram + (a ^ GBA_HX));
    return 1;
}
static int hook_obj(int i, int *x, int *y) {
    uint32_t a0 = *(const uint16_t *)((const uint8_t *)gba_mem.oam + ((i * 8) ^ GBA_HX));
    uint32_t a1 = *(const uint16_t *)((const uint8_t *)gba_mem.oam + ((i * 8 + 2) ^ GBA_HX));
    int xx = (int)(a1 & 0x1FF), yy = (int)(a0 & 0xFF);
    if (xx >= 256) xx -= 512;
    if (yy >= 160) yy -= 256;
    *x = xx; *y = yy;
    return 1;
}
#endif

int main(int argc, char **argv) {
    static uint16_t fb[320 * 240];
    int rc = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *out = argv[i + 1];
        size_t ol = strlen(out);
        int wide = ol > 9 && !strcmp(out + ol - 9, ".wide.raw");
        int wide2 = ol > 10 && !strcmp(out + ol - 10, ".wide2.raw");
        wide |= wide2;
#ifndef WS_W
        if (wide) continue;             /* runtime without ppu_soft_render_view */
#endif
        if (!load(argv[i])) { rc = 1; continue; }
        int n = 240 * 160;
        if (!wide) {
            ppu_soft_render(fb, 240);
        } else {
#ifdef WS_W
            ws_view_t v = { 40, 40, -40, -40, 280, 200, hook_entry, hook_obj };
            if (wide2) { v.bg_entry = NULL; v.obj_pos = NULL; }
            ppu_soft_render_view(fb, &v);
            n = 320 * 240;
#endif
        }
        FILE *f = fopen(out, "wb");
        if (!f) { perror(out); rc = 1; continue; }
        fwrite(fb, 2, (size_t)n, f);
        fclose(f);
    }
    return rc;
}

/*
 * rdpbench.c - RDP throughput micro-benchmarks for real hardware (Analogue
 * 3D / N64), to calibrate the cost model of tests/n64_render/rdpcost.py
 * (docs/RDP_COST.md).  Built into the renderer test ROM with -DRDPBENCH
 * (tests/n64_render/build.sh, EXTRA_CFLAGS="-DRDPBENCH"); the results are
 * shown on screen (and printed with debugf; emulators report ~0).
 *
 * Every workload covers the 320x240 screen once per pass, the way the
 * renderer draws (64x32 texture chunks with a TMEM load each, 8x8 sprite
 * tiles, fills), and is timed with the CPU counter around rspq_wait() over
 * several passes of a prebuilt raw command list.  Reported: microseconds per
 * pass and RDP clocks (62.5 MHz) per pixel.
 */
#ifdef RDPBENCH
#include <libdragon.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include "render_raw.h"

#define W 320
#define H 240
#define PASSES 4

static uint64_t *cmd;               /* raw command list (uncached) */
static int ncmd;
static uint8_t *tex;                /* CI8 512x256 texture, all indices non-zero */
static uint16_t *tlut;              /* opaque colours */
static surface_t s32, s16;
static uint8_t *big;                /* colour 32 + two Z buffers: one in the same 1MB RDRAM bank, one 1MB away */
static void *z_same, *z_far;

static char lines[24][48];
static int nlines;

static void put(uint64_t w) { cmd[ncmd++] = w; }

static uint64_t rect_cmd(int x0, int y0, int x1, int y1, int copy) {
    int xh = (x1 - copy) * 4, yh = (y1 - copy) * 4;
    return (0x24ull << 56) | ((uint64_t)xh << 44) | ((uint64_t)yh << 32) | ((uint64_t)(x0 * 4) << 12) | (uint64_t)(y0 * 4);
}

/* a pass of 64x32 chunks of the CI8 texture over the screen, as draw_ring does */
static void emit_chunks(int copy, int load) {
    put(RAW_SYNC_PIPE);
    put(raw_tex_image(RF_CI, RS_8, 512, PhysicalAddr(tex)));
    put(RAW_SYNC_TILE);
    put(raw_set_tile(RF_CI, RS_8, 64, 0, 1, 0));
    put(raw_set_tile(RF_CI, RS_8, 64, 0, 0, 0));
    put(raw_set_tile_size(0, 0, 0, 64, 32));
    for (int y = 0; y < H; y += 32)
        for (int x = 0; x < W; x += 64) {
            int y1 = y + 32 > H ? H : y + 32;
            if (load) {
                put(RAW_SYNC_TILE);
                put(RAW_SYNC_LOAD);
                put(raw_load_tile(1, x, y & 255, x + 64, (y & 255) + (y1 - y)));
            }
            put(rect_cmd(x, y, x + 64, y1, copy));
            put(((uint64_t)(copy ? 0x1000 : 0x400) << 16) | 0x400);
        }
    put(RAW_SYNC_LOAD); put(RAW_SYNC_TILE); put(RAW_SYNC_PIPE);
}

/* a pass of 8x8 rectangles (sprite tiles), a LOAD_BLOCK per 32 tiles */
static void emit_tiles(void) {
    put(RAW_SYNC_PIPE);
    put(raw_tex_image(RF_RGBA, RS_16, 16, PhysicalAddr(tex)));
    put(RAW_SYNC_TILE);
    put(raw_set_tile(RF_RGBA, RS_16, 0, 0, 1, 0));
    put(raw_set_tile(RF_CI, RS_8, 8, 0, 0, 0));
    put(raw_set_tile_size(0, 0, 0, 8, 256));
    int n = 0;
    for (int y = 0; y < H; y += 8)
        for (int x = 0; x < W; x += 8, n++) {
            if ((n & 31) == 0) {
                put(RAW_SYNC_TILE);
                put(RAW_SYNC_LOAD);
                put(raw_load_block(1, 0, 0, 32 * 32, 8));
            }
            put(rect_cmd(x, y, x + 8, y + 8, 0));
            put(((uint64_t)((n & 31) * 8 * 32) << 32) | (0x400ull << 16) | 0x400);
        }
    put(RAW_SYNC_LOAD); put(RAW_SYNC_TILE); put(RAW_SYNC_PIPE);
}

/* a pass of the 2x final pass: the 32-bit surface in 64x16 RGBA32 loads */
static void emit_final2x(void) {
    put(RAW_SYNC_PIPE);
    put(raw_tex_image(RF_RGBA, RS_32, W, PhysicalAddr(s32.buffer)));
    put(RAW_SYNC_TILE);
    put(raw_set_tile(RF_RGBA, RS_32, 128, 0, 1, 0));
    put(raw_set_tile(RF_RGBA, RS_32, 128, 0, 0, 0));
    put(raw_set_tile_size(0, 0, 0, 64, 16));
    for (int y = 0; y < H; y += 16)
        for (int x = 0; x < W; x += 64) {
            put(RAW_SYNC_TILE);
            put(RAW_SYNC_LOAD);
            put(raw_load_tile(1, x, y, x + 64, y + 16));
            put(rect_cmd(x, y, x + 64, y + 16, 0));
            put((0x400ull << 16) | 0x400);
        }
    put(RAW_SYNC_LOAD); put(RAW_SYNC_TILE); put(RAW_SYNC_PIPE);
}

static void emit_fill(int copylike) {
    put(RAW_SYNC_PIPE);
    put((0x36ull << 56) | ((uint64_t)((W - copylike) * 4) << 44) | ((uint64_t)((H - copylike) * 4) << 32));
    put(RAW_SYNC_PIPE);
}

/* time PASSES runs of the command list (modes already set) */
static uint32_t run_us(void) {
    rspq_wait();
    uint64_t t0 = get_ticks();
    for (int p = 0; p < PASSES; p++)
        rdpq_exec(cmd, ncmd * 8);
    rspq_wait();
    return (uint32_t)(TICKS_TO_US(get_ticks() - t0) / PASSES);
}

static void report(const char *name, uint32_t us, uint32_t base) {
    uint32_t net = us > base ? us - base : 0;
    float cpp = (float)net * 62.5f / (float)(W * H);
    debugf("RDPBENCH %-14s %6lu us/pass  %5.2f clk/px\n", name, (unsigned long)net, (double)cpp);
    if (nlines < 24)
        snprintf(lines[nlines++], sizeof lines[0], "%-13s%6lu us %5.2f c/px", name, (unsigned long)net, (double)cpp);
}

static void attach32(void *zbuf) {
    rspq_wait();
    if (zbuf) {
        surface_t z = surface_make(zbuf, FMT_RGBA16, W, H, W * 2);
        rdpq_attach(&s32, &z);
    } else {
        rdpq_attach(&s32, NULL);
    }
}

static void mode_tex(int bits32, int zmode, int rd) {
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_mode_dithering(DITHER_NONE_NONE);
    if (bits32) {
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (0, 0, 0, TEX0)));
        rdpq_set_prim_color(RGBA32(124, 124, 124, 255));
    } else {
        rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, TEX0), (0, 0, 0, TEX0)));
    }
    if (rd) {
        rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, FOG_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
        rdpq_set_fog_color(RGBA32(0, 0, 0, 128));
    }
    if (zmode) {
        rdpq_mode_zoverride(true, 0, 0);
        rdpq_set_prim_depth_raw(0x2000, 0);
        if (zmode == 1) rdpq_mode_zbuf(false, true);
        else { rdpq_mode_zbuf(true, false); rdpq_mode_zmode(ZMODE_DECAL); }
    }
    rdpq_tex_upload_tlut(tlut, 0, 256);
}

void rdpbench_run(void) {
    cmd = malloc_uncached_aligned(16, 64 * 1024);
    tex = malloc_uncached_aligned(64, 512 * 256);
    tlut = malloc_uncached_aligned(64, 512);
    for (int i = 0; i < 512 * 256; i++) tex[i] = (uint8_t)(1 + (i * 37 + (i >> 9) * 11) % 255);
    for (int i = 0; i < 256; i++) tlut[i] = (uint16_t)((i * 0x4F1B) | 1);
    big = malloc_uncached_aligned(64, 0x280000);
    uint8_t *base = (uint8_t *)(((uintptr_t)big + 0xFFFFF) & ~(uintptr_t)0xFFFFF);
    s32 = surface_make(base, FMT_RGBA32, W, H, W * 4);
    z_same = base + 0x50000;
    z_far = base + 0x150000;
    s16 = surface_alloc(FMT_RGBA16, W, H);
    debugf("RDPBENCH colour32 %p zsame %p zfar %p\n", s32.buffer, z_same, z_far);

    /* baseline: an almost empty list */
    ncmd = 0; put(RAW_SYNC_PIPE);
    rdpq_attach(&s16, NULL);
    uint32_t base_us = run_us();
    rdpq_detach_wait();

    rdpq_attach(&s16, NULL);
    rdpq_set_mode_fill(RGBA32(10, 20, 30, 255));
    ncmd = 0; emit_fill(1); report("fill16", run_us(), base_us);
    rdpq_set_mode_copy(true); rdpq_mode_tlut(TLUT_RGBA16); rdpq_tex_upload_tlut(tlut, 0, 256);
    ncmd = 0; emit_chunks(1, 1); report("copy16 chunks", run_us(), base_us);
    ncmd = 0; emit_chunks(1, 0); report("copy16 noload", run_us(), base_us);
    mode_tex(0, 0, 0);
    ncmd = 0; emit_chunks(0, 1); report("1cyc16 chunks", run_us(), base_us);
    ncmd = 0; emit_tiles(); report("1cyc16 8x8", run_us(), base_us);
    mode_tex(0, 0, 1);
    ncmd = 0; emit_chunks(0, 1); report("1cyc16+rd", run_us(), base_us);
    rdpq_detach_wait();

    attach32(NULL);
    rdpq_set_mode_fill(RGBA32(10, 20, 30, 255));
    ncmd = 0; emit_fill(1); report("fill32", run_us(), base_us);
    mode_tex(1, 0, 0);
    ncmd = 0; emit_chunks(0, 1); report("1cyc32 chunks", run_us(), base_us);
    mode_tex(1, 0, 1);
    ncmd = 0; emit_chunks(0, 1); report("1cyc32+rd", run_us(), base_us);
    rdpq_detach_wait();
    for (int far = 0; far < 2; far++) {
        attach32(far ? z_far : z_same);
        const char *nm[4] = { "1cyc32+zw", "1cyc32+zr", "1cyc32+rd+zr", "fill32+zw" };
        char name[24];
        mode_tex(1, 1, 0);
        ncmd = 0; emit_chunks(0, 1);
        snprintf(name, sizeof name, "%s%s", nm[0], far ? "/f" : ""); report(name, run_us(), base_us);
        mode_tex(1, 2, 0);
        ncmd = 0; emit_chunks(0, 1);
        snprintf(name, sizeof name, "%s%s", nm[1], far ? "/f" : ""); report(name, run_us(), base_us);
        mode_tex(1, 2, 1);
        ncmd = 0; emit_chunks(0, 1);
        snprintf(name, sizeof name, "%s%s", nm[2], far ? "/f" : ""); report(name, run_us(), base_us);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_set_prim_color(RGBA32(40, 40, 40, 255));
        rdpq_mode_zoverride(true, 0, 0);
        rdpq_set_prim_depth_raw(0x1000, 0);
        rdpq_mode_zbuf(false, true);
        ncmd = 0; emit_fill(0);
        snprintf(name, sizeof name, "%s%s", nm[3], far ? "/f" : ""); report(name, run_us(), base_us);
        rdpq_detach_wait();
    }
    rdpq_attach(&s16, NULL);
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, 0, PRIM, TEX0), (0, 0, 0, 1), (COMBINED, 0, ENV, COMBINED), (0, 0, 0, 1)));
    rdpq_set_prim_color(RGBA32(85, 85, 85, 255));
    rdpq_set_env_color(RGBA32(128, 128, 128, 255));
    rdpq_mode_dithering(DITHER_NONE_NONE);
    ncmd = 0; emit_final2x(); report("2cyc final2x", run_us(), base_us);
    rdpq_detach_wait();

    debugf("RDPBENCH done\n");
    for (;;) {
        surface_t *disp = display_get();
        graphics_fill_screen(disp, graphics_make_color(0, 0, 32, 255));
        graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
        graphics_draw_text(disp, 8, 4, "RDP bench (us per 320x240 pass)");
        for (int i = 0; i < nlines; i++)
            graphics_draw_text(disp, 8, 16 + 9 * i, lines[i]);
        display_show(disp);
    }
}
#else
typedef int rdpbench_unused;     /* (not an empty translation unit) */
#endif

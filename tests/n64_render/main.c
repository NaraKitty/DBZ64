/*
 * n64_render test ROM: renders GBA display states with platform/n64/render
 * and dumps the framebuffer (hex over the emulator debug channel) plus
 * timings, for comparison against runtime/ppu_soft.c (tests/n64_render/fbdiff.py).
 *
 * rom:/states.txt lists state files (GBAS v1, see platform/host/main.c).
 * Each state is rendered three times:
 *   pass 0  caches invalidated, all VRAM pages dirty   (cold)
 *   pass 1  nothing dirty                              (steady state)
 *   pass 2  all pages dirty, caches warm               (worst-case tracking)
 * The three framebuffers must be identical; pass 1 is dumped.
 * Afterwards an in-ROM blend calibration renders BG2-over-BG3 alpha blends for
 * every (EVA,EVB) and compares each pixel against the GBA formula.
 */
#include <libdragon.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "gba.h"
#include "io.h"
#include "gba_render.h"

extern uint32_t stub_dirty_mask;

static uint16_t saved[3][320 * 240];

#ifdef PROFILE
/* PC-sampling profiler (same scheme as platform/n64/prof.c), sampling only
 * while gba_render_frame() runs.  Symbolise with tools/prof_report.py. */
#define PSLOTS 8192
static uint32_t *line_pc, *line_hits;
static uint32_t prof_total, prof_dropped;
static volatile int prof_on;
static void prof_tick(int ovfl) {
    (void)ovfl;
    if (!prof_on) return;
#ifdef PROFILE_EXACT
    uint32_t line = (uint32_t)C0_EPC() & ~3u;       /* per instruction */
#else
    uint32_t line = (uint32_t)C0_EPC() & ~31u;
#endif
    prof_total++;
    uint32_t h = (line >> 5) * 2654435761u >> 19;
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & (PSLOTS - 1)) {
        if (line_pc[h] == line) { line_hits[h]++; return; }
        if (!line_pc[h]) { line_pc[h] = line; line_hits[h] = 1; return; }
    }
    prof_dropped++;
}
static void prof_begin(void) {
    line_pc = calloc(PSLOTS, sizeof(uint32_t));
    line_hits = calloc(PSLOTS, sizeof(uint32_t));
    timer_init();
    new_timer(TIMER_TICKS(PROFILE), TF_CONTINUOUS, prof_tick);
}
static void prof_dump(void) {
    debugf("PROFILE %lu samples (%lu dropped)\n", (unsigned long)prof_total, (unsigned long)prof_dropped);
    for (int k = 0; k < 160; k++) {
        int best = -1;
        for (int i = 0; i < PSLOTS; i++)
            if (line_hits[i] && (best < 0 || line_hits[i] > line_hits[best])) best = i;
        if (best < 0) break;
        debugf("PROF %lu %08lX\n", (unsigned long)line_hits[best], (unsigned long)line_pc[best]);
        line_hits[best] = 0;
    }
}
static int prof_enable;                 /* bench frames only (not the cold render) */
#define PROF_ON() (prof_on = prof_enable)
#define PROF_OFF() (prof_on = 0)
#else
#define PROF_ON() ((void)0)
#define PROF_OFF() ((void)0)
#endif

/* ------------------------------------------------------------ state loading */

static uint8_t filebuf[110 * 1024];

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void put_words(uint32_t *dst, const uint8_t *src, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes / 4; i++)
        dst[i] = le32(src + 4 * i);
}

static int load_state(const char *name) {
    char path[128];
    snprintf(path, sizeof path, "rom:/%s.bin", name);
    FILE *f = fopen(path, "rb");
    if (!f) { debugf("ERROR cannot open %s\n", path); return 0; }
    size_t n = fread(filebuf, 1, sizeof filebuf, f);
    fclose(f);
    if (n < 8 + 0x400 * 3 + 0x18000 + 4 + 160 * 32 || memcmp(filebuf, "GBAS", 4) || le32(filebuf + 4) != 1) {
        debugf("ERROR bad state %s (%u bytes)\n", path, (unsigned)n);
        return 0;
    }
    const uint8_t *p = filebuf + 8;
    put_words(gba_mem.io, p, 0x400); p += 0x400;
    put_words(gba_mem.pal, p, 0x400); p += 0x400;
    put_words(gba_mem.vram, p, 0x18000); p += 0x18000;
    put_words(gba_mem.oam, p, 0x400); p += 0x400;
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

/* ------------------------------------------------------------ output */

static void fb_dump(const char *tag, const uint16_t *fb, int w, int h) {
    static char line[16 + 320 * 4 + 2];
    static const char hex[] = "0123456789abcdef";
    debugf("FBDUMP %s %d %d\n", tag, w, h);
    for (int y = 0; y < h; y++) {
        int n = sprintf(line, "FB %d ", y);
        for (int x = 0; x < w; x++) {
            uint16_t v = fb[y * w + x];
            line[n++] = hex[v >> 12]; line[n++] = hex[(v >> 8) & 15];
            line[n++] = hex[(v >> 4) & 15]; line[n++] = hex[v & 15];
        }
        line[n++] = '\n'; line[n] = 0;
        debugf("%s", line);
    }
    debugf("FBEND %s\n", tag);
}

static void print_stats(const char *name, int pass, uint32_t cpu_us, uint32_t rdp_us) {
    const gba_render_stats_t *s = gba_render_get_stats();
    debugf("TIME %s pass=%d cpu=%lu rdp_done=%lu wait=%lu track=%lu bg=%lu bgupd=%lu obj=%lu oparse=%lu raster=%lu "
           "dirty=%lu chunks=%lu cells=%lu/%lu objs=%lu tiles=%lu aff=%lu rects=%lu "
           "pxcopy=%lu px1=%lu px2=%lu pxfill=%lu blend=%lu tags=%lu wrongpairs=%lu loads=%lu loadbytes=%lu "
           "blendpx=%lu tagpx=%lu blendrects=%lu\n",
           name, pass, (unsigned long)cpu_us, (unsigned long)rdp_us, (unsigned long)s->us_wait,
           (unsigned long)s->us_track, (unsigned long)s->us_bg, (unsigned long)s->us_bg_update, (unsigned long)s->us_obj, (unsigned long)s->us_obj_parse,
           (unsigned long)s->us_raster, (unsigned long)s->dirty_pages, (unsigned long)s->chunks_changed,
           (unsigned long)s->cells_expanded, (unsigned long)s->cells_checked,
           (unsigned long)s->obj_drawn, (unsigned long)s->obj_tiles, (unsigned long)s->obj_affine,
           (unsigned long)s->rects, (unsigned long)s->px_copy, (unsigned long)s->px_1cyc,
           (unsigned long)s->px_2cyc, (unsigned long)s->px_fill, (unsigned long)s->blend_path,
           (unsigned long)s->tags, (unsigned long)s->blend_wrong, (unsigned long)s->loads, (unsigned long)s->load_bytes,
           (unsigned long)s->blend_px, (unsigned long)s->tag_px, (unsigned long)s->blend_rects);
}

/* render one frame into a fresh display buffer; returns it (already shown) */
static int frame_no;

static void render_once(const gba_view_t *view, uint16_t *copy, uint32_t *cpu_us, uint32_t *rdp_us) {
    surface_t *fb = display_get();
#ifdef PREFILL
    {   /* poison the buffer so stale pixels are recognisable */
        uint64_t *p = (uint64_t *)fb->buffer;
        for (int i = 0; i < fb->stride * fb->height / 8; i++) p[i] = 0x1234123412341234ull;
    }
#endif
    rdpq_attach(fb, NULL);
    rspq_wait();
#if defined(RDPQ_VALIDATE) && defined(RDPQ_LOG_FRAME)
    if (frame_no == RDPQ_LOG_FRAME || frame_no == RDPQ_LOG_FRAME2) rdpq_debug_log(true);
#endif
    uint64_t t0 = get_ticks();
    PROF_ON();
    gba_render_frame(fb, view);
    PROF_OFF();
    uint64_t t1 = get_ticks();
    rdpq_detach_wait();
    uint64_t t2 = get_ticks();
#if defined(RDPQ_VALIDATE) && defined(RDPQ_LOG_FRAME)
    if (frame_no == RDPQ_LOG_FRAME || frame_no == RDPQ_LOG_FRAME2) rdpq_debug_log(false);
#endif
    debugf("FRAME %d\n", frame_no++);
    *cpu_us = (uint32_t)TICKS_TO_US(t1 - t0);
    *rdp_us = (uint32_t)TICKS_TO_US(t2 - t0);
    if (copy) {
        const uint16_t *src = (const uint16_t *)fb->buffer;
        for (int y = 0; y < 240; y++)
            memcpy(copy + y * 320, src + y * (fb->stride / 2), 320 * 2);
#ifdef READBACK_CHECK
        static uint16_t again[320 * 240];
        wait_ms(50);
        for (int y = 0; y < 240; y++)
            memcpy(again + y * 320, src + y * (fb->stride / 2), 320 * 2);
        int d = 0;
        for (int i = 0; i < 320 * 240; i++) d += again[i] != copy[i];
        debugf("READBACK frame %d buffer %p changed_after_wait=%d\n", frame_no - 1, fb->buffer, d);
#endif
    }
    display_show(fb);
}

static int count_diff(const uint16_t *a, const uint16_t *b) {
    int d = 0;
    for (int i = 0; i < 320 * 240; i++) d += a[i] != b[i];
    return d;
}

static void run_state(const char *name, const gba_view_t *view) {
    if (!load_state(name)) return;
    uint32_t cpu, rdp;
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 0) { gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF; }
        else if (pass == 1) stub_dirty_mask = 0;
        else stub_dirty_mask = 0xFFFFFF;
        render_once(view, saved[pass], &cpu, &rdp);
        print_stats(name, pass, cpu, rdp);
    }
    int d0 = count_diff(saved[0], saved[1]), d2 = count_diff(saved[2], saved[1]);
    debugf("CMP %s pass0_vs_1=%d pass2_vs_1=%d\n", name, d0, d2);
    fb_dump(name, saved[1], 320, 240);
    char tag[64];
    if (d0) { snprintf(tag, sizeof tag, "%s.p0", name); fb_dump(tag, saved[0], 320, 240); }
    if (d2) { snprintf(tag, sizeof tag, "%s.p2", name); fb_dump(tag, saved[2], 320, 240); }
}

/* ------------------------------------------------------------ blend calibration */

static void wr_vram8(uint32_t o, uint32_t v) { ((uint8_t *)gba_mem.vram)[o ^ GBA_BX] = (uint8_t)v; }
static void wr_vram16(uint32_t o, uint32_t v) { *(uint16_t *)((uint8_t *)gba_mem.vram + (o ^ GBA_HX)) = (uint16_t)v; }
static void wr_pal16(uint32_t i, uint32_t v) { *(uint16_t *)((uint8_t *)gba_mem.pal + ((i * 2) ^ GBA_HX)) = (uint16_t)v; }

/* Incremental cache test: scroll every BG for 24 frames with nothing dirty,
 * then edit tile data, map entries and OBJ tiles (pages reported dirty), and
 * compare each incremental result with a cold render of the same state. */
static uint32_t lcg = 12345;
static uint32_t rnd(void) { lcg = lcg * 1103515245u + 12345u; return lcg >> 8; }

/* the units the next gba_vram_take_units() reports (stubs.c) */
extern uint32_t stub_units[96];
extern int stub_units_any;

static void note_units(uint32_t off, uint32_t len) {
    for (uint32_t u = off >> 5; u <= (off + len - 1) >> 5; u++)
        stub_units[u >> 5] |= 1u << (u & 31);
    stub_units_any = 1;
}

/* VRAM edits: for every text BG, rewrite the tile under the screen centre
 * and change a few map entries in view; rewrite the tiles of the first OBJs.
 * Returns the pages written; with 'units' they are noted as 32-byte units
 * instead. */
static uint32_t vram_edits(int units) {
    uint32_t mask = 0;
    uint32_t dispcnt = io16(REG_DISPCNT);
    for (int n = 0; n < 4; n++) {
        if (!(dispcnt & (0x100u << n)) || (dispcnt & 7) > 1 || ((dispcnt & 7) == 1 && n >= 2)) continue;
        uint32_t cnt = io16(REG_BG0CNT + 2 * n);
        uint32_t sb = ((cnt >> 8) & 31) * 0x800u, cb = ((cnt >> 2) & 3) * 0x4000u;
        int bpp8 = (cnt & 0x80) != 0;
        uint32_t cx = ((io16(REG_BG0HOFS + 4 * n) + 120) >> 3) & 31, cy = ((io16(REG_BG0HOFS + 4 * n + 2) + 80) >> 3) & 31;
        uint32_t ea = sb + cy * 64 + cx * 2;
        uint32_t e = *(const uint16_t *)((const uint8_t *)gba_mem.vram + (ea ^ GBA_HX));
        uint32_t ta = cb + (e & 0x3FF) * (bpp8 ? 64u : 32u);
        if (ta + 64 <= 0x10000) {
            for (int k = 0; k < (bpp8 ? 64 : 32); k++) wr_vram8(ta + k, rnd() & 0xFF);
            mask |= 1u << (ta >> 12);
            if (units) note_units(ta, bpp8 ? 64 : 32);
        }
        for (int k = 0; k < 6; k++) {
            uint32_t a2 = sb + ((cy + k) & 31) * 64 + ((cx + 3 * k) & 31) * 2;
            wr_vram16(a2, (rnd() & 0x1FF) | (rnd() & 0xC00) | (bpp8 ? 0 : (rnd() & 0xF000)));
            mask |= 1u << (a2 >> 12);
            if (units) note_units(a2, 2);
        }
    }
    for (int i = 0; i < 4; i++) {
        uint32_t a2 = *(const uint16_t *)((const uint8_t *)gba_mem.oam + ((i * 8 + 4) ^ GBA_HX)) & 0x3FF;
        uint32_t off = 0x10000 + a2 * 32;
        uint32_t len = off + 128 <= 0x18000 ? 128 : 0x18000 - off;
        for (uint32_t k = 0; k < len; k++) wr_vram8(off + k, rnd() & 0xFF);
        mask |= 1u << (off >> 12);
        if (units) note_units(off, len);
    }
    return mask;
}

static void dyn_test(const char *name, const gba_view_t *view) {
    if (!load_state(name)) return;
    uint32_t cpu, rdp, sum = 0, mx = 0;
    gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
    render_once(view, NULL, &cpu, &rdp);
    for (int f = 0; f < 24; f++) {
        for (int n = 0; n < 4; n++) {
            io16_set(REG_BG0HOFS + 4 * n, (uint16_t)(io16(REG_BG0HOFS + 4 * n) + 3 + n));
            io16_set(REG_BG0HOFS + 4 * n + 2, (uint16_t)(io16(REG_BG0HOFS + 4 * n + 2) + 2));
            if (gba_line_regs_valid)
                for (int l = 0; l < 160; l++) {
                    gba_line_regs[l].hofs[n] += (uint16_t)(3 + n);
                    gba_line_regs[l].vofs[n] += 2;
                }
        }
        stub_dirty_mask = 0;
        render_once(view, f == 23 ? saved[0] : NULL, &cpu, &rdp);
        sum += cpu; if (cpu > mx) mx = cpu;
    }
    gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
    render_once(view, saved[1], &cpu, &rdp);
    int d_scroll = count_diff(saved[0], saved[1]);

    /* VRAM edits, reported as dirty pages */
    uint32_t mask = vram_edits(0);
    stub_dirty_mask = mask;
    render_once(view, saved[0], &cpu, &rdp);
    uint32_t vcpu = cpu;
    const gba_render_stats_t *s = gba_render_get_stats();
    uint32_t chunks = s->chunks_changed, exp = s->cells_expanded, chk = s->cells_checked;
    gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
    render_once(view, saved[1], &cpu, &rdp);
    int d_vram = count_diff(saved[0], saved[1]);
    /* more edits, reported as noted 32-byte units only (gba_vram_note) */
    stub_dirty_mask = 0;
    render_once(view, NULL, &cpu, &rdp);
    vram_edits(1);
    stub_dirty_mask = 0;
    render_once(view, saved[0], &cpu, &rdp);
    gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
    render_once(view, saved[1], &cpu, &rdp);
    int d_units = count_diff(saved[0], saved[1]);
    debugf("DYN %s scroll_diff=%d vram_diff=%d units_diff=%d scroll_cpu_avg=%lu scroll_cpu_max=%lu vram_cpu=%lu dirty=%08lx chunks=%lu cells=%lu/%lu\n",
           name, d_scroll, d_vram, d_units, (unsigned long)(sum / 24), (unsigned long)mx, (unsigned long)vcpu,
           (unsigned long)mask, (unsigned long)chunks, (unsigned long)exp, (unsigned long)chk);
    if (d_scroll || d_vram || d_units) {
        char tag[64];
        snprintf(tag, sizeof tag, "%s.dyn_inc", name); fb_dump(tag, saved[0], 320, 240);
        snprintf(tag, sizeof tag, "%s.dyn_cold", name); fb_dump(tag, saved[1], 320, 240);
    }
}

/* Widescreen plumbing test: a 320x240 region with a bg_entry hook that returns
 * the hardware map entry of the world cell (so the 240x160 centre must equal
 * the native render) and an obj_pos hook that returns the hardware position. */
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
    if (yy >= 160) yy -= 256;       /* same choice ppu_soft makes for lines 0..159 */
    *x = xx; *y = yy;
    return 1;
}

static void wide_test(const char *name, const gba_view_t *native) {
    if (!load_state(name)) return;
    uint32_t cpu, rdp;
    gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
    render_once(native, saved[0], &cpu, &rdp);
    gba_view_t wv = *native;
    wv.x0 = -40; wv.y0 = -40; wv.x1 = 280; wv.y1 = 200;
    wv.bg_entry = hook_entry;
    wv.obj_pos = hook_obj;
    stub_dirty_mask = 0xFFFFFF;
    render_once(&wv, NULL, &cpu, &rdp);             /* cold */
    stub_dirty_mask = 0;
    render_once(&wv, saved[1], &cpu, &rdp);         /* steady */
    int d = 0, outside = 0;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            int in = x >= 40 && x < 280 && y >= 40 && y < 200;
            uint16_t a = saved[0][y * 320 + x], b = saved[1][y * 320 + x];
            if (in) d += a != b;
            else outside += (b >> 1) != 0;
        }
    debugf("WIDE %s centre_diff=%d nonblack_outside=%d cpu=%lu\n", name, d, outside, (unsigned long)cpu);
    char tag[64];
    snprintf(tag, sizeof tag, "%s.dyn_wide", name);
    fb_dump(tag, saved[1], 320, 240);
    /* the same region without hooks: hardware map entries and OBJ positions
     * (x signed 9-bit, y wrapping) around the GBA screen; compared with
     * ppu_soft_render_view without hooks (fbdiff.py, .wide2.raw) */
    gba_view_t nv = *native;
    nv.x0 = -40; nv.y0 = -40; nv.x1 = 280; nv.y1 = 200;
    stub_dirty_mask = 0;
    render_once(&nv, saved[2], &cpu, &rdp);
    snprintf(tag, sizeof tag, "%s.dyn_wide2", name);
    fb_dump(tag, saved[2], 320, 240);
}

#ifdef CMDLOG
/* RDP command stream of one steady-state frame per state, for the RDP cost
 * model (tests/n64_render/rdpcost.py): the 320x240 view with the WIDE hooks
 * (like the in-game field view) and the native view.  Needs RDPQ_VALIDATE
 * (rdpq's trace engine prints every command the RDP receives).  The logged
 * frames are dumped too, so they can be checked against ppu_soft. */
static void cmdlog_state(const char *name, const gba_view_t *native) {
    uint32_t cpu, rdp;
    char tag[64];
    for (int v = 0; v < 2; v++) {
        if (!load_state(name)) return;
        gba_view_t wv = *native;
        if (v == 0) {
            wv.x0 = -40; wv.y0 = -40; wv.x1 = 280; wv.y1 = 200;
            wv.bg_entry = hook_entry;
            wv.obj_pos = hook_obj;
        }
        gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
        render_once(&wv, NULL, &cpu, &rdp);
        stub_dirty_mask = 0;
        debugf("CMDLOG_BEGIN %s %s\n", name, v == 0 ? "wide" : "native");
        rdpq_debug_log(true);
        render_once(&wv, saved[1], &cpu, &rdp);
        rdpq_debug_log(false);
        rspq_wait();
        debugf("CMDLOG_END %s\n", name);
        print_stats(name, v == 0 ? 10 : 11, cpu, rdp);
        snprintf(tag, sizeof tag, v == 0 ? "%s.dyn_wide" : "%s", name);
        fb_dump(tag, saved[1], 320, 240);
    }
}
#endif

#if defined(BENCH) || defined(PROFILE)
/* CPU time per frame over 32 frames for gameplay-like dirty patterns
 * (the runtime tracks VRAM writes in 16KB groups on the N64):
 *   steady  nothing dirty
 *   scroll  every BG scrolls 1 px right / 1 px down per frame, nothing dirty
 *   obj     both OBJ groups dirty (sprite animation uploads), content unchanged
 *   bgmap   the 16KB group holding screen blocks 28..31 dirty + OBJ groups
 *   all     all 24 pages dirty
 * BENCH_SCEN=k limits the run to scenario k (for the profiler). */
#ifndef BENCH_SCEN
#define BENCH_SCEN -1
#endif
static void bench_state(const char *name, const gba_view_t *view) {
    static const char *sn[5] = { "steady", "scroll", "obj", "bgmap", "all" };
    static const uint32_t smask[5] = { 0, 0, 0xFF0000, 0xFFF000, 0xFFFFFF };
    for (int sc = 0; sc < 5; sc++) {
        if (BENCH_SCEN >= 0 && sc != BENCH_SCEN) continue;
        if (!load_state(name)) return;
        uint32_t cpu, rdp, sum = 0, mx = 0, rmax = 0;
        gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
        render_once(view, NULL, &cpu, &rdp);
#ifdef PROFILE
        prof_enable = 1;
#endif
        for (int f = 0; f < 32; f++) {
            if (sc == 1)
                for (int n = 0; n < 4; n++) {
                    io16_set(REG_BG0HOFS + 4 * n, (uint16_t)(io16(REG_BG0HOFS + 4 * n) + 1));
                    io16_set(REG_BG0HOFS + 4 * n + 2, (uint16_t)(io16(REG_BG0HOFS + 4 * n + 2) + 1));
                    if (gba_line_regs_valid)
                        for (int l = 0; l < 160; l++) { gba_line_regs[l].hofs[n]++; gba_line_regs[l].vofs[n]++; }
                }
            stub_dirty_mask = smask[sc];
            render_once(view, NULL, &cpu, &rdp);
            sum += cpu; if (cpu > mx) mx = cpu;
            if (gba_render_get_stats()->blend_rects > rmax) rmax = gba_render_get_stats()->blend_rects;
        }
#ifdef PROFILE
        prof_enable = 0;
#endif
        debugf("BENCH %s %s avg=%lu max=%lu blendrects_max=%lu\n", name, sn[sc], (unsigned long)(sum / 32),
               (unsigned long)mx, (unsigned long)rmax);
    }
}
#endif

/* Frame sequences for the BG layer cache, each compared with a cold render:
 *   blank:   a forced-blank frame in which map entries and tile data change
 *            (pages dirty), then the same display again with nothing dirty
 *   disable: the same with the BGs disabled in DISPCNT instead of forced blank
 *   far:     40 frames of large scroll steps and jumps (ring wraps, windows
 *            that do not overlap, 16-bit scroll wrap), nothing dirty */
static uint32_t lcg2 = 777;
static uint32_t rnd2(void) { lcg2 = lcg2 * 1103515245u + 12345u; return lcg2 >> 8; }

static uint32_t edit_text_bgs(void) {
    uint32_t mask = 0, dispcnt = io16(REG_DISPCNT);
    for (int n = 0; n < 4; n++) {
        if ((dispcnt & 7) > 1 || ((dispcnt & 7) == 1 && n >= 2)) continue;
        uint32_t cnt = io16(REG_BG0CNT + 2 * n);
        uint32_t sb = ((cnt >> 8) & 31) * 0x800u, cb = ((cnt >> 2) & 3) * 0x4000u;
        int bpp8 = (cnt & 0x80) != 0;
        uint32_t cx = ((io16(REG_BG0HOFS + 4 * n) + 120) >> 3) & 31, cy = ((io16(REG_BG0HOFS + 4 * n + 2) + 80) >> 3) & 31;
        for (int k = 0; k < 8; k++) {
            uint32_t ea = sb + ((cy + 2 * k) & 31) * 64 + ((cx + 3 * k) & 31) * 2;
            if (ea + 2 > 0x10000) continue;
            uint32_t e = *(const uint16_t *)((const uint8_t *)gba_mem.vram + (ea ^ GBA_HX));
            uint32_t ta = cb + (e & 0x3FF) * (bpp8 ? 64u : 32u);
            if (k < 3 && ta + 64 <= 0x10000) {         /* tile data of a cell in view */
                for (int b = 0; b < (bpp8 ? 64 : 32); b++) wr_vram8(ta + b, rnd2() & 0xFF);
                mask |= 1u << (ta >> 12);
            }
            wr_vram16(ea, (rnd2() & 0x1FF) | (rnd2() & 0xC00) | (bpp8 ? 0 : (rnd2() & 0xF000)));
            mask |= 1u << (ea >> 12);
        }
    }
    return mask;
}

static int seq_cold_diff(const gba_view_t *view, const char *name, const char *what) {
    uint32_t cpu, rdp;
    gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
    render_once(view, saved[1], &cpu, &rdp);
    int d = count_diff(saved[0], saved[1]);
    if (d) {
        char tag[64];
        snprintf(tag, sizeof tag, "%s.dyn_%s_inc", name, what); fb_dump(tag, saved[0], 320, 240);
        snprintf(tag, sizeof tag, "%s.dyn_%s_cold", name, what); fb_dump(tag, saved[1], 320, 240);
    }
    return d;
}

static void seq_test(const char *name, const gba_view_t *view) {
    if (!load_state(name)) return;
    uint32_t cpu, rdp;
    uint16_t dispcnt = io16(REG_DISPCNT);
    gba_render_invalidate(); stub_dirty_mask = 0xFFFFFF;
    render_once(view, NULL, &cpu, &rdp);
    /* forced blank while VRAM changes */
    io16_set(REG_DISPCNT, dispcnt | 0x80);
    stub_dirty_mask = edit_text_bgs();
    render_once(view, NULL, &cpu, &rdp);
    io16_set(REG_DISPCNT, dispcnt);
    stub_dirty_mask = 0;
    render_once(view, saved[0], &cpu, &rdp);
    int d_blank = seq_cold_diff(view, name, "blank");
    /* BGs disabled while VRAM changes (caches warm from the cold render) */
    io16_set(REG_DISPCNT, dispcnt & ~0x0F00);
    stub_dirty_mask = edit_text_bgs();
    render_once(view, NULL, &cpu, &rdp);
    io16_set(REG_DISPCNT, dispcnt);
    stub_dirty_mask = 0;
    render_once(view, saved[0], &cpu, &rdp);
    int d_disable = seq_cold_diff(view, name, "disable");
    /* far scrolling */
    for (int f = 0; f < 40; f++) {
        for (int n = 0; n < 4; n++) {
            int dh = f == 20 ? 301 + 64 * n : f == 30 ? -0x7FF0 : 37 + 11 * n;
            int dv = f == 25 ? 170 : f == 33 ? 0x8003 : 23 + 5 * n;
            io16_set(REG_BG0HOFS + 4 * n, (uint16_t)(io16(REG_BG0HOFS + 4 * n) + dh));
            io16_set(REG_BG0HOFS + 4 * n + 2, (uint16_t)(io16(REG_BG0HOFS + 4 * n + 2) + dv));
            if (gba_line_regs_valid)
                for (int l = 0; l < 160; l++) {
                    gba_line_regs[l].hofs[n] += (uint16_t)dh;
                    gba_line_regs[l].vofs[n] += (uint16_t)dv;
                }
        }
        stub_dirty_mask = 0;
        render_once(view, f == 39 ? saved[0] : NULL, &cpu, &rdp);
    }
    int d_far = seq_cold_diff(view, name, "far");
    debugf("SEQ %s blank_diff=%d disable_diff=%d far_diff=%d\n", name, d_blank, d_disable, d_far);
}

static inline int ch_a(int ch, int a) { return ch == 0 ? a : ch == 1 ? 31 - a : (a * 7) & 31; }
static inline int ch_b(int ch, int b) { return ch == 0 ? b : ch == 1 ? 31 - b : (b * 13) & 31; }

static void calibrate(const gba_view_t *view) {
    memset(&gba_mem.io, 0, sizeof gba_mem.io);
    memset(&gba_mem.pal, 0, sizeof gba_mem.pal);
    memset(&gba_mem.vram, 0, sizeof gba_mem.vram);
    memset(&gba_mem.oam, 0, sizeof gba_mem.oam);
    gba_line_regs_valid = 0;
    /* BG2: pixel (x,y) -> index 1 + (x & 31);  BG3: 33 + (y & 31) */
    for (int k = 0; k < 4; k++)
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                wr_vram8(k * 64 + r * 8 + c, 1 + k * 8 + c);           /* tiles 0..3 */
                wr_vram8((4 + k) * 64 + r * 8 + c, 33 + k * 8 + r);    /* tiles 4..7 */
            }
    for (int cy = 0; cy < 32; cy++)
        for (int cx = 0; cx < 32; cx++) {
            wr_vram16(30 * 0x800 + cy * 64 + cx * 2, (uint32_t)(cx & 3));
            wr_vram16(31 * 0x800 + cy * 64 + cx * 2, (uint32_t)(4 + (cy & 3)));
        }
    for (int a = 0; a < 32; a++) {
        wr_pal16(1 + a, (uint32_t)(ch_a(0, a) | (ch_a(1, a) << 5) | (ch_a(2, a) << 10)));
        wr_pal16(33 + a, (uint32_t)(ch_b(0, a) | (ch_b(1, a) << 5) | (ch_b(2, a) << 10)));
    }
    io16_set(REG_DISPCNT, 0x0C00);
    io16_set(REG_BG0CNT + 4, 0x1E80);      /* BG2: sb30, 8bpp, prio 0 */
    io16_set(REG_BG0CNT + 6, 0x1F81);      /* BG3: sb31, 8bpp, prio 1 */
    io16_set(REG_BLDCNT, 0x0844);
    gba_render_invalidate();
    stub_dirty_mask = 0xFFFFFF;
    int total_bad = 0;
    for (int eva = 0; eva <= 16; eva++)
        for (int evb = 0; evb <= 16; evb++) {
            io16_set(REG_BLDALPHA, (uint16_t)(eva | (evb << 8)));
            uint32_t cpu, rdp;
            render_once(view, saved[0], &cpu, &rdp);
            stub_dirty_mask = 0;
            int bad = 0, err[3] = { 0, 0, 0 }, maxd = 0;
            for (int y = 0; y < 160; y++)
                for (int x = 0; x < 240; x++) {
                    uint16_t px = saved[0][(y + 40) * 320 + x + 40];
                    int got[3] = { px >> 11, (px >> 6) & 31, (px >> 1) & 31 };
                    int pbad = 0;
                    for (int ch = 0; ch < 3; ch++) {
                        int a = ch_a(ch, x & 31), b = ch_b(ch, y & 31);
                        int e = (a * eva + b * evb) >> 4;
                        if (e > 31) e = 31;
                        int d = got[ch] - e;
                        if (d) { err[ch]++; pbad = 1; if (abs(d) > maxd) maxd = abs(d); }
                    }
                    bad += pbad;
                }
            const gba_render_stats_t *s = gba_render_get_stats();
            debugf("CALIB eva=%d evb=%d blend=%lu badpx=%d err_r=%d err_g=%d err_b=%d maxd=%d model_wrong=%lu cpu=%lu rdp=%lu\n",
                   eva, evb, (unsigned long)s->blend_path, bad, err[0], err[1], err[2], maxd,
                   (unsigned long)s->blend_wrong, (unsigned long)cpu, (unsigned long)rdp);
            total_bad += bad;
            if (eva == 8 && evb == 16)
                fb_dump("calib_8_16", saved[0], 320, 240);
        }
    debugf("CALIB_TOTAL badpx=%d\n", total_bad);
}

#ifdef MICROBENCH
static void microbench(void) {
    static uint64_t cbuf[16384] __attribute__((aligned(16)));
    uint64_t *ubuf = malloc_uncached_aligned(16, sizeof cbuf);
    volatile uint64_t sink = 0;
    uint64_t t0, t1;
    t0 = get_ticks();
    for (int i = 0; i < 16384; i++) cbuf[i] = i;
    t1 = get_ticks();
    debugf("BENCH cached seq store 16384x8B: %lu us\n", (unsigned long)TICKS_TO_US(t1 - t0));
    t0 = get_ticks();
    for (int i = 0; i < 16384; i++) cbuf[i] = i;
    t1 = get_ticks();
    debugf("BENCH cached seq store again (hot): %lu us\n", (unsigned long)TICKS_TO_US(t1 - t0));
    t0 = get_ticks();
    for (int i = 0; i < 16384; i++) ubuf[i] = i;
    t1 = get_ticks();
    debugf("BENCH uncached store 16384x8B: %lu us\n", (unsigned long)TICKS_TO_US(t1 - t0));
    t0 = get_ticks();
    for (int i = 0; i < 16384; i++) ubuf[(i * 64) & 16383] = i;
    t1 = get_ticks();
    debugf("BENCH uncached store strided 16384x8B: %lu us\n", (unsigned long)TICKS_TO_US(t1 - t0));
    data_cache_hit_writeback_invalidate(cbuf, sizeof cbuf);
    t0 = get_ticks();
    uint64_t acc = 0;
    for (int i = 0; i < 16384; i += 2) acc += cbuf[i];
    t1 = get_ticks();
    sink = acc;
    debugf("BENCH cached seq load 8192x8B cold: %lu us\n", (unsigned long)TICKS_TO_US(t1 - t0));
    t0 = get_ticks();
    acc = 0;
    for (int i = 0; i < 16384; i++) acc += cbuf[(i * 37) & 16383];
    t1 = get_ticks();
    sink = acc;
    debugf("BENCH cached random load 16384x8B: %lu us\n", (unsigned long)TICKS_TO_US(t1 - t0));
    t0 = get_ticks();
    for (int i = 0; i < 10000; i++) acc += get_ticks();
    t1 = get_ticks();
    sink = acc;
    debugf("BENCH get_ticks x10000: %lu us\n", (unsigned long)TICKS_TO_US(t1 - t0));
    t0 = get_ticks();
    for (volatile int i = 0; i < 100000; i++) {}
    t1 = get_ticks();
    debugf("BENCH empty volatile loop x100000: %lu us\n", (unsigned long)TICKS_TO_US(t1 - t0));
    (void)sink;
    free_uncached(ubuf);
}
#endif

int main(void) {
    debug_init_emulog();
    debugf("n64_render test start\n");
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_DISABLED);
    rdpq_init();
#ifdef RDPQ_VALIDATE
    rdpq_debug_start();
#endif
    dfs_init(DFS_DEFAULT_LOCATION);
#ifdef RDPBENCH
    /* RDP throughput benchmarks for real hardware (rdpbench.c); never returns */
    void rdpbench_run(void);
    rdpbench_run();
#endif
    gba_render_init();
#ifdef MICROBENCH
    microbench();
#endif

    gba_view_t view;
    gba_render_view_native(&view);
#if defined(BENCH_WIDE) && (defined(BENCH) || defined(PROFILE))
    /* bench / profile the 320x240 field view (hooks as in wide_test) */
    view.x0 = -40; view.y0 = -40; view.x1 = 280; view.y1 = 200;
    view.bg_entry = hook_entry;
    view.obj_pos = hook_obj;
#endif

    static char list[4096];
    FILE *f = fopen("rom:/states.txt", "r");
    int n = f ? (int)fread(list, 1, sizeof list - 1, f) : 0;
    if (f) fclose(f);
    list[n] = 0;
    char *save = NULL;
    int nocalib = 0;
#if defined(BENCH) || defined(PROFILE)
#ifdef PROFILE
    prof_begin();
#endif
    for (char *tok = strtok_r(list, " \r\n", &save); tok; tok = strtok_r(NULL, " \r\n", &save)) {
        if (tok[0] == '#') continue;
        bench_state(tok, &view);
    }
#ifdef PROFILE
    prof_dump();
#endif
    nocalib = 1;
#elif defined(CMDLOG)
    for (char *tok = strtok_r(list, " \r\n", &save); tok; tok = strtok_r(NULL, " \r\n", &save)) {
        if (tok[0] == '#') continue;
        cmdlog_state(tok, &view);
    }
    nocalib = 1;
#else
    for (char *tok = strtok_r(list, " \r\n", &save); tok; tok = strtok_r(NULL, " \r\n", &save)) {
        if (tok[0] == '#') { nocalib |= !strcmp(tok, "#nocalib"); continue; }
        run_state(tok, &view);
        dyn_test(tok, &view);
        wide_test(tok, &view);
        seq_test(tok, &view);
    }
#endif

    if (!nocalib)
        calibrate(&view);

    debugf("n64_render test done\n");
    if (emux_detect(1) & EMUX_FEAT1_IOCTL)
        emux_ioctl_exit();
    for (;;) {}
}

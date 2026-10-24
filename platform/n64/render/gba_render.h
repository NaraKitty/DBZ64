/*
 * gba_render.h - GBA PPU renderer for the N64 (libdragon rdpq).
 *
 * Draws the current GBA display state (gba_mem.io / pal / vram / oam and
 * gba_line_regs[]) into a 16-bit libdragon surface.  The reference for every
 * pixel is runtime/ppu_soft.c; see docs at the top of gba_render.c for the
 * pipeline and the documented approximations.
 *
 * Usage contract (per frame):
 *
 *     surface_t *fb = display_get();
 *     rdpq_attach(fb, NULL);              // caller attaches the display
 *     gba_render_frame(fb, &view);        // only enqueues rdpq commands
 *     rdpq_detach_show();                 // caller presents
 *
 *  - gba_render_frame() calls gba_vram_take_dirty() and
 *    gba_vram_take_units() exactly once per call: nothing else may consume
 *    the VRAM dirty bits or the noted units.
 *  - On entry it waits until the RDP has finished the previous
 *    gba_render_frame() (its caches are single-buffered), so the CPU never
 *    rewrites a texture the RDP may still be reading.  Normally the RDP is
 *    long done by then; the wait time is reported in the stats.
 *  - It leaves the RDP attached to 'disp', with the scissor reset to the
 *    full surface and an unspecified render mode.  It may temporarily attach
 *    an offscreen surface (blend frames) and always pops it again.
 *  - 'disp' must be RGBA16.  Pixels outside the view region are filled black.
 *  - Call it from the thread that owns rdpq (it uses rspq syncpoints).
 *  - bg_entry hook: a world cell is queried when it enters the view and when
 *    its hardware map cell changes (the change tracking of the hardware
 *    path).  If the hook's data changes in any other way (e.g. new authored
 *    map chunks become available for cells already in view), call
 *    gba_render_invalidate_bg(bg) for the affected BGs.  obj_pos is queried
 *    every frame.
 *
 * Memory (allocated by gba_render_init): BG layer cache 611KB (4 x 512x256
 * CI8 rings + slot tables; rings 2 and 3 are one block that holds the
 * affine BG texture in modes 1/2), BG VRAM shadow 72KB, OBJ shadow + scratch 64KB, raw RDP
 * command buffer 128KB, scratch 160KB, blend intermediate 300KB + tag buffer
 * 150KB (320x240), blend-path cell masks 34KB, CPU-composited OBJ pixels
 * 32KB (static); affine/bitmap BG raster buffers (75KB each, 150KB for
 * direct colour) on first use.
 */
#ifndef GBA_RENDER_H
#define GBA_RENDER_H

#include <libdragon.h>
#include "gba.h"

typedef struct gba_view {
    int ox, oy;            /* framebuffer position of GBA screen pixel (0,0); native: 40,40 */
    int x0, y0, x1, y1;    /* region to draw, in GBA screen coordinates (native: 0,0,240,160) */
    /* optional widescreen hooks (NULL = hardware behaviour):
       bg_entry: tilemap entry for BG 'bg' world tile cell (cx,cy) (world = full 16-bit
       scroll + screen pos >> 3); return 1 if *entry was provided, 0 = use the hardware map */
    int (*bg_entry)(int bg, int cx, int cy, uint16_t *entry);
    /* obj_pos: full signed screen position of OAM entry i; return 1 if provided */
    int (*obj_pos)(int i, int *x, int *y);
    /* bg_affine: affine parameters of BG 'bg' (2/3) for a line outside 0..159:
       p = {x, y (20.8, sign-extended), pa, pc}; return 1 if provided */
    int (*bg_affine)(int bg, int line, int32_t p[4]);
} gba_view_t;

void gba_render_init(void);
void gba_render_frame(surface_t *disp, const gba_view_t *view);

/* 1 while the RDP is still drawing the previous gba_render_frame: calling
 * gba_render_frame now would wait for it.  Lets a caller skip a frame
 * instead of stalling the game. */
int gba_render_busy(void);

/* ---- extras (test harness / platform diagnostics) ---- */

/* The native view: 240x160 at (40,40), no hooks. */
void gba_render_view_native(gba_view_t *v);

/* Drop every cache: the next frame re-reads all VRAM as if it was all dirty. */
void gba_render_invalidate(void);

/* With a bg_entry hook, the renderer re-queries a world cell only when it
 * enters the view or when its hardware map cell changes.  Call this when the
 * hook's data for BG 'bg' changes in any other way (all of it is re-queried). */
void gba_render_invalidate_bg(int bg);

typedef struct gba_render_stats {
    /* CPU time of the last gba_render_frame, microseconds */
    uint32_t us_total;      /* whole call, including us_wait */
    uint32_t us_wait;       /* waiting for the previous frame's RDP work */
    uint32_t us_track;      /* VRAM dirty tracking (BG chunk compare, OBJ shadow) */
    uint32_t us_bg;         /* BG cache update + BG command generation */
    uint32_t us_obj;        /* OBJ command generation (incl. affine raster) */
    uint32_t us_raster;     /* CPU rasterised layers (affine / bitmap BG) */
    uint32_t us_bg_update;  /* part of us_bg: layer cache update (cell checks + expansion) */
    uint32_t us_obj_parse;  /* OAM parsing (not part of us_obj) */
    /* work counters */
    uint32_t dirty_pages;   /* VRAM pages reported dirty */
    uint32_t chunks_changed;/* 32-byte BG chunks that really changed */
    uint32_t cells_checked, cells_expanded;
    uint32_t obj_drawn, obj_tiles, obj_affine;
    uint32_t rects;         /* texture rectangles enqueued */
    uint32_t loads, load_bytes;   /* TMEM loads by the raw paths (TLUT loads not counted) */
    uint32_t px_copy, px_1cyc, px_2cyc, px_fill;   /* pixels covered per RDP mode */
    uint32_t blend_path;    /* 32-bit blend path with: 1 = the 2x final pass, 2 = the TLUT final
                               pass, 3 = the TLUT final pass in two passes */
    uint32_t tags;          /* 1 = 2nd-target tags (Z buffer) were needed */
    uint32_t blend_wrong;   /* model-predicted wrong (a,b) pairs of the blend config, of 1024 */
    uint32_t dirty_mask;    /* the VRAM pages reported dirty (bit n = 4KB page n) */
    uint32_t obj_units_conv;/* 32-byte OBJ VRAM units converted into the shadows */
    uint32_t aff_cells;     /* affine BG texture: map cells re-resolved */
    uint32_t us_aff_update; /* part of us_raster: affine BG texture update */
    uint32_t blend_px;      /* pixels drawn through the 32-bit blend path */
    uint32_t tag_px;        /* of these, with 2nd-target tags */
    uint32_t blend_rects;   /* rectangles of the blend path */
} gba_render_stats_t;

const gba_render_stats_t *gba_render_get_stats(void);

#endif

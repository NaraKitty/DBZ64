/*
 * render_internal.h - shared state of the N64 GBA renderer (gba_render*.c).
 */
#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "gba_render.h"
#include "io.h"

#if !GBA_BIG_ENDIAN
#error "the N64 renderer assumes the big-endian native-word guest memory layout"
#endif

/* ------------------------------------------------------------ guest memory */

static inline uint32_t r_vram8(uint32_t o) {
    return ((const uint8_t *)gba_mem.vram)[o ^ GBA_BX];
}
static inline uint32_t r_vram16(uint32_t o) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.vram + (o ^ GBA_HX));
}
static inline uint32_t r_oam16(uint32_t o) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.oam + (o ^ GBA_HX));
}
static inline uint32_t r_pal16(uint32_t i) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.pal + ((i * 2) ^ GBA_HX)) & 0x7FFFu;
}
/* VRAM is 96KB mirrored in 128KB blocks, the last 32KB repeating 0x10000.. */
static inline uint32_t r_vram_wrap(uint32_t o) {
    o &= 0x1FFFFu;
    return o >= 0x18000u ? o - 0x8000u : o;
}
static inline int32_t r_sext28(uint32_t v) {
    return (int32_t)(v << 4) >> 4;
}
/* GBA BGR555 -> N64 RGBA5551 (opaque) */
static inline uint16_t r_conv(uint32_t c) {
    return (uint16_t)(((c & 31) << 11) | (((c >> 5) & 31) << 6) | (((c >> 10) & 31) << 1) | 1);
}

/* ------------------------------------------------------------ target */

/* Where the frame is being drawn: the display (16-bit, COPY mode) or the
 * 32-bit blend intermediate (1-cycle mode).  All draw helpers take GBA
 * screen coordinates; (tx,ty) is the target position of GBA pixel (0,0).
 * The region is the whole frame (layer caches and rasters are kept for it);
 * draws are clipped to the draw window inside it, which the blend path
 * narrows to the rectangles it renders. */
typedef struct {
    int tx, ty;
    int gx0, gy0, gx1, gy1;     /* region being drawn, GBA coordinates */
    int wx0, wy0, wx1, wy1;     /* draw window (inside the region) */
    int is32;
} rtarget_t;
extern rtarget_t R;

/* ------------------------------------------------------------ cell masks */

/* One bit per 8x8 cell of the region (cell (i,j) = GBA pixels R.gx0 + 8i..,
 * R.gy0 + 8j.., clipped to the region), a row of cells per word.  Regions of
 * up to 504x256 pixels (63 x CM_ROWS cells); larger ones skip the masks. */
#define CM_ROWS 32
typedef struct { uint64_t r[CM_ROWS]; } cmask_t;
extern int cm_w, cm_h;          /* cells across / down the region this frame */

/* index of the lowest set bit of x != 0 (the VR4300 has no instruction for
 * it and libgcc's __ctzdi2 is a loop) */
static inline int r_ctz64(uint64_t x) {
    static const uint8_t db[64] = {
         0,  1, 48,  2, 57, 49, 28,  3, 61, 58, 50, 42, 38, 29, 17,  4,
        62, 55, 59, 36, 53, 51, 43, 22, 45, 39, 33, 30, 24, 18, 12,  5,
        63, 47, 56, 27, 60, 41, 37, 16, 54, 35, 52, 21, 44, 32, 23, 11,
        46, 26, 40, 15, 34, 20, 31, 10, 25, 14, 19,  9, 13,  8,  7,  6,
    };
    return db[((x & (0 - x)) * 0x03F79D71B4CB0A89ull) >> 58];
}

enum { PAL_BG = 0, PAL_OBJ = 1, PAL_DIRECT = 2 };   /* DIRECT = RGBA16 texels, no TLUT */
enum { FX_PLAIN = 0, FX_BRIGHT = 1, FX_ALPHA = 2 };  /* colour effect applied to a layer */

/* Select pipeline + palette for following draws. */
void r_bind(int pal, int fx);
/* Per-channel 5-bit colour remap for 'fx' (identity for FX_PLAIN); used by
 * CPU-converted direct-colour layers. */
const uint8_t *r_tau(int fx);
/* Scissor in GBA coordinates (clipped to the draw window). */
void r_scissor(int gx0, int gy0, int gx1, int gy1);
void r_scissor_region(void);        /* the whole draw window */
/* Texture rectangle with TILE0 at GBA coordinates; vflip draws rows bottom-up,
 * hflip columns right-to-left (not in COPY mode: OBJ pipelines only). */
void r_texrect(int gx, int gy, int w, int h, int s, int t, int vflip, int hflip);

/* The clip window and pipeline that r_texrect works with (set by r_scissor
 * and r_bind).  Hot loops copy it into a local and emit rectangles with
 * r_rect straight into the raw command buffer. */
typedef struct {
    int tx, ty;                 /* target position of GBA pixel (0,0) */
    int x0, y0, x1, y1;         /* clip rectangle, target coordinates */
    int copy;                   /* COPY mode (else 1-cycle) */
} rclip_t;
extern rclip_t r_clip;

/* Write the TEXTURE_RECTANGLE for r_texrect's arguments at p (2 words the
 * caller has reserved), clipped to c; returns the new write position (p if
 * the rectangle was clipped away) and adds the pixels covered to *px. */
static inline uint64_t *r_rect(uint64_t *p, const rclip_t *c, int gx, int gy, int w, int h,
                               int s, int t, int vflip, int hflip, uint32_t *px) {
    int x = gx + c->tx, y = gy + c->ty;
    if (hflip) s += w - 1;                  /* texel of the leftmost pixel */
    if (x < c->x0) {
        int d = c->x0 - x;
        s += hflip ? -d : d;
        w -= d; x = c->x0;
    }
    if (x + w > c->x1) w = c->x1 - x;
    if (w <= 0) return p;
    if (y < c->y0) {
        int d = c->y0 - y;
        if (!vflip) t += d;                 /* flipped: the clipped rows are the last texels */
        h -= d; y = c->y0;
    }
    if (y + h > c->y1) {
        int d = y + h - c->y1;
        if (vflip) t += d;
        h -= d;
    }
    if (h <= 0) return p;
    /* COPY mode: inclusive right/bottom edge and 4 texels per clock */
    int copy = c->copy;
    int xh = (x + w - copy) * 4, yh = (y + h - copy) * 4;
    uint32_t dsdx = copy ? 0x1000 : hflip ? 0xFC00 : 0x400, dtdy = 0x400;
    if (vflip) { t += h - 1; dtdy = 0xFC00; }
    p[0] = (0x24ull << 56) | ((uint64_t)xh << 44) | ((uint64_t)yh << 32) |
           ((uint64_t)(x * 4) << 12) | (uint64_t)(y * 4);
    p[1] = ((uint64_t)(uint16_t)(s * 32) << 48) | ((uint64_t)(uint16_t)(t * 32) << 32) |
           ((uint64_t)dsdx << 16) | dtdy;
    *px += (uint32_t)(w * h);
    return p + 2;
}

/* Blit a screen-aligned linear image (CI8 or RGBA16) at GBA (gx,gy). */
void r_blit_image(const void *buf, int pitch_bytes, tex_format_t fmt, int gx, int gy, int w, int h);

extern gba_render_stats_t r_stats;
extern uint32_t r_frame;            /* frame counter, starts at 1 */
extern const gba_view_t *r_view;

/* add the work of n rectangles covering px pixels in the current pipeline to the stats */
static inline void r_rect_stats(const rclip_t *c, uint32_t n, uint32_t px) {
    r_stats.rects += n;
    if (c->copy) r_stats.px_copy += px; else r_stats.px_1cyc += px;
}

/* per-frame uncached scratch memory for the RDP (16-byte aligned); when full it
 * waits for the RDP and starts over (so a caller must be done writing a block
 * before it asks for the next one); NULL only for more than the whole scratch */
void *r_scratch(uint32_t bytes);
/* large uncached raster buffers (affine/bitmap BG), one per BG 2/3 */
void *r_raster_buf(int which, uint32_t bytes);

/* ------------------------------------------------------------ BG */

void bg_init(void);
void bg_invalidate(void);
void bg_invalidate_layer(int n);
/* pages 0..15; units: noted 32-byte units of BG VRAM (64 words) or NULL */
void bg_track(uint32_t dirty_pages, const uint32_t *units);
void bg_draw_text(int n, int fx);
/* bring text BG n's cache up to date for the frame and mark the cells where
 * it may have an opaque pixel (touch) and where every pixel is opaque (solid);
 * with mosaic every cell may be touched and none is solid */
void bg_text_cells(int n, cmask_t *touch, cmask_t *solid);
/* largest channel values (BGR555) of the colours text BG n may show in the
 * region (from the palette index range of the tiles in view) */
uint32_t bg_text_colour_max(int n);
void bg_draw_affine(int n, int fx);             /* mode 1/2 rotation-scaling BG */
void bg_draw_bitmap(int mode, uint32_t dispcnt, int fx);   /* modes 3/4/5, BG2 */
#ifdef GBA_RENDER_TRACE
extern uint32_t bg_trace_grp[4];                /* changed BG chunks per 16KB group, this frame */
extern uint32_t bg_trace_cmp;                   /* BG chunks compared with the shadow, this frame */
#endif

/* ------------------------------------------------------------ OBJ */

typedef struct {
    int16_t x, y;           /* hardware position (x signed 9-bit, y 0..255) */
    uint8_t w, h, bw, bh;   /* size and bounding box */
    uint8_t prio, semi, bpp8, hf, vf, affine, ident, own_pos;
    uint16_t tile;
    uint8_t pbank;          /* OBJ palette bank (4bpp) */
    uint8_t mosaic;         /* attr0 mosaic bit */
    int32_t pa, pb, pc, pd;
} objinfo_t;

void obj_init(void);
void obj_invalidate(void);
/* pages 16..23 as bits 0..7; units: noted units of OBJ VRAM (32 words) or NULL */
void obj_track(uint32_t dirty_pages, const uint32_t *units);
/* parse OAM; returns per-priority lists of visible OBJ indices in draw order (127..0) */
int obj_parse(uint32_t dispcnt, uint8_t lists[4][128], int counts[4]);
/* bring the OBJ shadow up to date for the listed sprites (render_obj.c) */
void obj_prepare(uint8_t lists[4][128], const int counts[4]);
const objinfo_t *obj_info(int i);
void obj_draw(int i, int fx);
/* bounding box (GBA coordinates, end exclusive) of OBJ i's positions over
 * the region; 0 if it has none */
int obj_box(int i, int *x0, int *y0, int *x1, int *y1);
/* largest channel values (BGR555) of the colours OBJ i may show */
uint32_t obj_colour_max(int i);
/* The OBJ layer composited on the CPU (render_obj.c).  Painter's order is
 * not the GBA's OBJ layer where an OBJ with a better priority follows an
 * earlier one in OAM order (its transparent texels still pass their
 * priority down, see ppu_soft), where an OBJ uses mosaic, and, with alpha
 * blending, where an OBJ that blends (semi-transparent; with
 * OBJ_CPU_BLEND_ALL every OBJ, the OBJ layer being a first target) covers
 * another OBJ: the GBA blends the OBJ layer's top pixel with the BG
 * beneath, not with the OBJ beneath.  obj_cpu_begin finds the rectangle
 * covering those places; inside it every OBJ pixel comes from a CPU
 * composition (ppu_soft's rules), drawn per (priority, semi) as obj_cpu_draw
 * items, and obj_draw leaves the rectangle out.  Returns the number of
 * items, each a (prio | semi << 2) code in codes[]. */
enum { OBJ_CPU_BLEND_NONE, OBJ_CPU_BLEND_SEMI, OBJ_CPU_BLEND_ALL };
int obj_cpu_begin(uint8_t lists[4][128], const int counts[4], int blend, uint8_t codes[8]);
void obj_cpu_draw(int code, int fx);
void obj_cpu_end(void);
/* the cells a CPU-composited item may cover (the composed rectangle and the
 * boxes of the OBJs with that code; GBA coordinates, end exclusive) and the
 * largest channel values (BGR555) of the colours it may show; 0 if none */
int obj_cpu_box(int code, int *x0, int *y0, int *x1, int *y1);
uint32_t obj_cpu_colour_max(int code);

#endif

/*
 * render_obj.c - OBJ (sprite) layer for the N64 GBA renderer.
 *
 * OBJ VRAM (0x10000-0x17FFF) is drawn from an uncached shadow in the RDP's
 * byte order, converted on the CPU (obj_prepare):
 *   - guest VRAM is stored as native 32-bit words: a row of 8 pixels of an
 *     8bpp tile is the doubleword [w0][w1] = pixels 3,2,1,0, 7,6,5,4.  The
 *     same row with its two words swapped is the row mirrored (7..0): the
 *     shadow holds the mirrored rows (one 64-bit rotate per row).
 *   - sprites are drawn from the mirrored rows: H-flipped sprites as they
 *     are, the others with a negative S step (1-cycle mode; COPY mode cannot
 *     flip, so OBJs are always drawn in 1-cycle mode).
 * Only the 32-byte units of visible sprites whose 4KB page was written since
 * their last conversion are converted.
 *
 * Real hardware (Analogue 3D) rules kept here: every LOAD_BLOCK start row is
 * below 1024 (the texture image is re-based per 32KB region of obj_img, see
 * run_open), and TMEM is used plainly (loaded with dxt, sampled with the
 * same line parity).
 *
 * obj_img layout (uncached):
 *   [0x0000] mirrored shadow of OBJ VRAM (CPU-converted)
 *   [0x8000] per-frame scratch tiles (CPU-written, final orientation):
 *            4bpp tiles expanded to CI8, tiles wrapping at 32KB, bitmap-mode cut
 * A regular sprite is drawn as one 8x8 texture rectangle per tile; runs of
 * consecutive tiles share one LOAD_BLOCK (up to 32 tiles = 2KB of TMEM).
 * V-flip uses a negative T step.  Affine sprites (other than the identity
 * matrix) are rasterised on the CPU with ppu_soft's exact formula into a CI8
 * buffer and blitted.
 */
#include <string.h>
#include <malloc.h>
#include "render_internal.h"
#include "render_raw.h"

#define OBJ_IMG_SCRATCH 0x8000
#define OBJ_IMG_SIZE    0x10000
static uint8_t *obj_img;                /* uncached: shadow | scratch */
/* one bit per 32-byte unit of OBJ VRAM: the shadow holds its current data
 * (cleared when its 4KB page is reported dirty) */
static uint32_t fresh_bits[0x8000 / 32 / 32];
static uint32_t scratch_off;
static objinfo_t info[128];
static uint32_t min_off;                /* OBJ VRAM offset of the first visible tile byte */
static int one_d;

static const uint8_t obj_dim[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

void obj_init(void) {
    obj_img = malloc_uncached_aligned(64, OBJ_IMG_SIZE);
    memset(obj_img, 0, OBJ_IMG_SIZE);
    obj_invalidate();
}

void obj_invalidate(void) {
    memset(fresh_bits, 0, sizeof fresh_bits);
}

void obj_track(uint32_t pages, const uint32_t *units) {
    for (int p = 0; p < 8; p++)
        if (pages & (1u << p))
            fresh_bits[p * 4] = fresh_bits[p * 4 + 1] = fresh_bits[p * 4 + 2] = fresh_bits[p * 4 + 3] = 0;
        else if (units)                 /* units noted as changed */
            for (int w = p * 4; w < p * 4 + 4; w++)
                fresh_bits[w] &= ~units[w];
    scratch_off = OBJ_IMG_SCRATCH;
}

const objinfo_t *obj_info(int i) {
    return &info[i];
}

int obj_parse(uint32_t dispcnt, uint8_t lists[4][128], int counts[4]) {
    int any_semi = 0;
    one_d = (dispcnt & 0x40) != 0;
    min_off = (dispcnt & 7) >= 3 ? 0x4000u : 0;
    int objwin_on = (dispcnt & 0x8000) != 0;
    for (int i = 127; i >= 0; i--) {
        uint32_t a0 = r_oam16(i * 8), a1 = r_oam16(i * 8 + 2), a2 = r_oam16(i * 8 + 4);
        int affine = (a0 & 0x100) != 0;
        if (!affine && (a0 & 0x200)) continue;
        uint32_t mode = (a0 >> 10) & 3;
        /* OBJ-window sprites never draw colour (windows are not implemented) */
        if (mode == 3 || mode == 2) { (void)objwin_on; continue; }
        uint32_t shape = a0 >> 14;
        if (shape == 3) continue;
        objinfo_t *o = &info[i];
        o->w = obj_dim[shape][a1 >> 14][0];
        o->h = obj_dim[shape][a1 >> 14][1];
        o->bw = o->w; o->bh = o->h;
        if (affine && (a0 & 0x200)) { o->bw *= 2; o->bh *= 2; }
        int x = (int)(a1 & 0x1FF);
        if (x & 0x100) x -= 512;
        o->x = (int16_t)x;
        o->y = (int16_t)(a0 & 0xFF);
        o->own_pos = 0;
        if (r_view->obj_pos) {
            int px, py;
            if (r_view->obj_pos(i, &px, &py)) {
                o->x = (int16_t)px; o->y = (int16_t)py; o->own_pos = 1;
            }
        }
        o->prio = (uint8_t)((a2 >> 10) & 3);
        o->semi = mode == 1;
        o->mosaic = (a0 & 0x1000) != 0;
        o->bpp8 = (a0 & 0x2000) != 0;
        o->tile = (uint16_t)(a2 & 0x3FF);
        o->pbank = (uint8_t)(a2 >> 12);
        o->affine = (uint8_t)affine;
        o->hf = !affine && (a1 & 0x1000);
        o->vf = !affine && (a1 & 0x2000);
        o->pa = 256; o->pb = 0; o->pc = 0; o->pd = 256;
        if (affine) {
            uint32_t g = ((a1 >> 9) & 31) * 32;
            o->pa = (int16_t)r_oam16(g + 6);
            o->pb = (int16_t)r_oam16(g + 14);
            o->pc = (int16_t)r_oam16(g + 22);
            o->pd = (int16_t)r_oam16(g + 30);
        }
        o->ident = affine && !(a0 & 0x200) && o->pa == 256 && o->pb == 0 && o->pc == 0 && o->pd == 256;
        lists[o->prio][counts[o->prio]++] = (uint8_t)i;
        any_semi |= o->semi;
    }
    return any_semi;
}

/* ------------------------------------------------------------ shadow */

/* positions at which a sprite is drawn, as in ppu_soft: x is the signed 9-bit
 * value (no wrap: a sprite at x -256..-233 must not appear at 256..279 in a
 * wider view), y wraps with period 256 (a hook position is drawn once,
 * unwrapped); only positions that overlap the region are returned */
static void obj_positions(const objinfo_t *o, int xs[3], int ys[3], int *nx, int *ny) {
    int a = 0, b = 0;
    if (o->x < R.gx1 && o->x + o->bw > R.gx0) xs[a++] = o->x;
    if (o->own_pos) {
        if (o->y < R.gy1 && o->y + o->bh > R.gy0) ys[b++] = o->y;
    } else {
        for (int k = -1; k <= 1; k++) {
            int yy = o->y + 256 * k;
            if (yy < R.gy1 && yy + o->bh > R.gy0) ys[b++] = yy;
        }
    }
    *nx = a; *ny = b;
}

static inline uint32_t row_stride_of(const objinfo_t *o) {
    return one_d ? (uint32_t)(o->w / 8) * (o->bpp8 ? 2u : 1u) : 32u;
}

/* OBJ VRAM offset of tile (c, r) of a regular sprite (8bpp in 2D mapping
 * ignores the tile number's low bit, like ppu_soft) */
static inline uint32_t tile_off(const objinfo_t *o, uint32_t row_stride, int c, int r) {
    uint32_t tile = o->tile;
    if (o->bpp8 && !one_d) tile &= ~1u;
    return ((tile + (uint32_t)r * row_stride + (uint32_t)c * (o->bpp8 ? 2u : 1u)) * 32u) & 0x7FFF;
}

/* is this tile drawn straight from the shadow (else from a scratch tile)? */
static inline int tile_from_shadow(const objinfo_t *o, uint32_t off) {
    return o->bpp8 && off + 64 <= 0x8000 && off >= min_off;
}

/* convert units [u0, u0+n) of OBJ VRAM into the shadow, rows mirrored (see top) */
static void conv_units(uint32_t u0, uint32_t n) {
    const uint64_t *v = (const uint64_t *)(gba_mem.vram + (0x10000 + u0 * 32) / 4);
    uint64_t *d = (uint64_t *)(obj_img + u0 * 32);
    for (uint32_t k = 0; k < n; k++, v += 4, d += 4) {
        uint64_t a = v[0], b = v[1], c = v[2], e = v[3];
        d[0] = (a << 32) | (a >> 32);
        d[1] = (b << 32) | (b >> 32);
        d[2] = (c << 32) | (c >> 32);
        d[3] = (e << 32) | (e >> 32);
    }
    r_stats.obj_units_conv += n;
}

/* make units [u0, u0+n) of the shadow current (converting the stale ones) */
static void fresh_range(uint32_t u0, uint32_t n) {
    while (n) {
        uint32_t w = u0 >> 5, b = u0 & 31, k = 32 - b < n ? 32 - b : n;
        uint32_t m = (k == 32 ? 0xFFFFFFFFu : ((1u << k) - 1)) << b;
        uint32_t stale = m & ~fresh_bits[w];
        fresh_bits[w] |= m;
        while (stale) {
            /* a run of stale units inside this word */
            int b0 = r_ctz64(stale);
            uint32_t x = ~(stale >> b0);
            int len = x ? r_ctz64(x) : 32 - b0;
            stale &= ~((len == 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << b0);
            conv_units(w * 32 + (uint32_t)b0, (uint32_t)len);
        }
        u0 += k; n -= k;
    }
}

void obj_prepare(uint8_t lists[4][128], const int counts[4]) {
    for (int p = 0; p < 4; p++)
        for (int k = 0; k < counts[p]; k++) {
            const objinfo_t *o = &info[lists[p][k]];
            if (!o->bpp8 || (o->affine && !o->ident)) continue;
            int xs[3], ys[3], nx, ny;
            obj_positions(o, xs, ys, &nx, &ny);
            if (!nx || !ny) continue;
            int tw = o->w / 8, th = o->h / 8;
            uint32_t start = ((uint32_t)o->tile * 32u) & 0x7FFF, len = (uint32_t)(tw * th) * 64u;
            if (one_d && start >= min_off && start + len <= 0x8000) {
                fresh_range(start >> 5, len >> 5);      /* 1D: one contiguous run */
                continue;
            }
            uint32_t rs = row_stride_of(o);
            for (int r = 0; r < th; r++)
                for (int c = 0; c < tw; c++) {
                    uint32_t off = tile_off(o, rs, c, r);
                    if (tile_from_shadow(o, off)) fresh_range(off >> 5, 2);
                }
        }
}

/* ------------------------------------------------------------ tile runs */

/* Command emission for regular sprites, kept in locals (no call per tile):
 * runs of consecutive tiles share one LOAD_BLOCK, written before the run's
 * rectangles and completed with the run length when the run ends. */
typedef struct {
    uint64_t *p, *pend;     /* write position; a run may start up to pend */
    uint64_t *load;         /* LOAD_BLOCK word of the open run (NULL: none) */
    uint32_t src, n;        /* image offset of the run's first tile, tiles in it */
    uint32_t px, rects, tiles, loads;
} emit_t;
static uint32_t run_region;             /* obj_img offset of the texture image set for runs */

static inline __attribute__((always_inline)) void run_close(emit_t *e) {
    if (!e->load) return;
    uint32_t region = e->src & ~(uint32_t)(OBJ_IMG_SCRATCH - 1);
    *e->load = raw_load_block(1, 0, (int)((e->src - region) >> 5), (int)e->n * 32, 8);
    e->loads++; e->tiles += e->n;
    e->load = NULL;
}

/* (cold paths out of line, so that the run state stays in registers) */
static __attribute__((noinline)) uint64_t *run_overflow(uint64_t *p) {
    raw_ptr = p;
    raw_overflow();
    return raw_ptr;
}

static __attribute__((noinline)) uint64_t *run_setup(uint64_t *p, uint32_t region) {
    /* image = 16 RGBA16 texels (32 bytes) per row; tile 1 loads, tile 0 draws CI8 */
    p[0] = RAW_SYNC_PIPE;
    p[1] = raw_tex_image(RF_RGBA, RS_16, 16, PhysicalAddr(obj_img + region));
    p[2] = RAW_SYNC_TILE;
    p[3] = raw_set_tile(RF_RGBA, RS_16, 0, 0, 1, 0);
    p[4] = raw_set_tile(RF_CI, RS_8, 8, 0, 0, 0);
    p[5] = raw_set_tile_size(0, 0, 0, 8, 256);
    raw_owner = RAW_OWNER_OBJ;
    run_region = region;
    return p + 6;
}

static inline __attribute__((always_inline)) void run_open(emit_t *e, uint32_t src) {
    uint64_t *p = e->p;
    if (__builtin_expect(p > e->pend, 0))   /* room for the setup, the load and 32 rectangles */
        p = run_overflow(p);
    /* The texture image starts at the 32KB region (shadow / scratch) of the
     * run, so the LOAD_BLOCK start row stays below 1024: real hardware (the
     * Analogue 3D) wraps larger rows. */
    uint32_t region = src & ~(uint32_t)(OBJ_IMG_SCRATCH - 1);
    if (raw_owner != RAW_OWNER_OBJ || region != run_region)
        p = run_setup(p, region);
    p[0] = RAW_SYNC_TILE;
    p[1] = RAW_SYNC_LOAD;
    e->load = &p[2];
    e->p = p + 3;
    e->src = src;
    e->n = 0;
}

/* one 8x8 tile from image offset src at GBA (x,y) */
static inline __attribute__((always_inline)) void run_tile(emit_t *e, const rclip_t *c, uint32_t src,
                                                           int x, int y, int vf, int hf) {
    if (!e->load || src != e->src + e->n * 64 || e->n == 32) {
        run_close(e);
        run_open(e, src);
    }
    uint64_t *q = r_rect(e->p, c, x, y, 8, 8, 0, (int)e->n * 8, vf, hf, &e->px);
    e->rects += q != e->p;
    e->p = q;
    e->n++;
}

/* texel of an OBJ: same addressing as ppu_soft (tile units of 32 bytes, 32KB wrap) */
static inline uint32_t obj_texel(const objinfo_t *o, uint32_t row_stride, int tx, int ty) {
    uint32_t t = o->tile + (uint32_t)(ty >> 3) * row_stride + (uint32_t)(tx >> 3) * (o->bpp8 ? 2 : 1);
    uint32_t inner = o->bpp8 ? (uint32_t)((ty & 7) * 8 + (tx & 7)) : (uint32_t)((ty & 7) * 4 + ((tx & 7) >> 1));
    uint32_t off = (t * 32 + inner) & 0x7FFF;
    if (off < min_off) return 0;
    uint32_t idx = r_vram8(0x10000 + off);
    if (!o->bpp8) {
        idx = (tx & 1) ? idx >> 4 : idx & 15;
        if (idx) idx |= (uint32_t)o->pbank << 4;
    }
    return idx;
}

/* build one 8x8 CI8 tile in the scratch part of the OBJ image with the generic
 * texel fetch (4bpp, 32KB wrap, bitmap-mode cut), in its final orientation;
 * returns its image offset */
static uint32_t tile_scratch(const objinfo_t *o, uint32_t row_stride, int tc, int tr, int hf) {
    uint32_t off = scratch_off;
    scratch_off += 64;
    uint64_t *d = (uint64_t *)(obj_img + off);
    for (int r = 0; r < 8; r++) {
        uint64_t acc = 0;
        for (int c = 0; c < 8; c++) {
            int cc = hf ? 7 - c : c;
            acc = (acc << 8) | obj_texel(o, row_stride, tc * 8 + cc, tr * 8 + r);
        }
        d[r] = acc;
    }
    return off;
}

/* The common case: an 8bpp 1D sprite whose tiles lie in the shadow (one
 * contiguous run in drawing order), entirely inside the clip window.  The
 * same commands as draw_regular's general path, without its per-tile
 * clipping and run bookkeeping. */
static int draw_regular_fast(const objinfo_t *o, int x, int y) {
    int tw = o->w / 8, th = o->h / 8, n = tw * th;
    uint32_t start = ((uint32_t)o->tile * 32u) & 0x7FFF;
    if (!o->bpp8 || !one_d || start < min_off || start + (uint32_t)n * 64u > 0x8000)
        return 0;
    const rclip_t c = r_clip;
    int X = x + c.tx, Y = y + c.ty;
    if (X < c.x0 || X + o->w > c.x1 || Y < c.y0 || Y + o->h > c.y1 || c.copy)
        return 0;
    /* 1-cycle rectangles, shadow rows mirrored: s runs right-to-left unless
     * the sprite is H-flipped */
    int hf = o->hf, vf = o->vf;
    uint64_t w1 = (hf ? 0 : (uint64_t)(7 * 32) << 48) | (vf ? (uint64_t)(7 * 32) << 32 : 0) |
                  ((uint64_t)(hf ? 0x400 : 0xFC00) << 16) | (vf ? 0xFC00 : 0x400);
    int shift = r_ctz64((unsigned)tw);
    uint64_t *p = raw_ptr;
    for (int k0 = 0; k0 < n; k0 += 32) {
        int kn = n - k0 < 32 ? n - k0 : 32;
        if (__builtin_expect(p > raw_end - (6 + 3 + 2 * 32 + 4), 0))
            p = run_overflow(p);
        if (raw_owner != RAW_OWNER_OBJ || run_region != 0)
            p = run_setup(p, 0);
        p[0] = RAW_SYNC_TILE;
        p[1] = RAW_SYNC_LOAD;
        p[2] = raw_load_block(1, 0, (int)((start >> 5) + (uint32_t)k0 * 2), kn * 32, 8);
        p += 3;
        for (int k = 0; k < kn; k++) {
            int t = k0 + k, r = t >> shift, cc = t & (tw - 1);
            int tx = X + (hf ? tw - 1 - cc : cc) * 8, ty = Y + (vf ? th - 1 - r : r) * 8;
            p[0] = (0x24ull << 56) | ((uint64_t)((tx + 8) * 4) << 44) | ((uint64_t)((ty + 8) * 4) << 32) |
                   ((uint64_t)(tx * 4) << 12) | (uint64_t)(ty * 4);
            p[1] = w1 + ((uint64_t)(k * 8 * 32) << 32);
            p += 2;
        }
    }
    raw_ptr = p;
    r_stats.loads += (uint32_t)(n + 31) / 32; r_stats.load_bytes += (uint32_t)n * 64;
    r_stats.obj_tiles += (uint32_t)n;
    r_rect_stats(&c, (uint32_t)n, (uint32_t)n * 64);
    return 1;
}

static void draw_regular(const objinfo_t *o, int x, int y) {
    if (draw_regular_fast(o, x, y))
        return;
    int tw = o->w / 8, th = o->h / 8;
    uint32_t row_stride = row_stride_of(o);
    objinfo_t tmp;
    if (o->bpp8 && !one_d) { tmp = *o; tmp.tile &= ~1u; o = &tmp; }
    const rclip_t c = r_clip;
    /* tiles outside the clip rectangle (inside the draw window: a part of
     * it beside the CPU-composited holes) are not expanded or loaded */
    const int gx0 = c.x0 - c.tx, gy0 = c.y0 - c.ty, gx1 = c.x1 - c.tx, gy1 = c.y1 - c.ty;
    const int hf = o->hf, vf = o->vf;
    emit_t e = { raw_ptr, raw_end - (6 + 3 + 2 * 32 + 4), NULL, 0, 0, 0, 0, 0, 0 };
    for (int r = 0; r < th; r++) {
        int sy = y + (vf ? th - 1 - r : r) * 8;
        if (sy + 8 <= gy0 || sy >= gy1) continue;
        for (int cc = 0; cc < tw; cc++) {
            int sx = x + (hf ? tw - 1 - cc : cc) * 8;
            if (sx + 8 <= gx0 || sx >= gx1) continue;
            uint32_t off = tile_off(o, row_stride, cc, r);
            if (tile_from_shadow(o, off)) {
                /* shadow rows are mirrored: unflipped sprites draw them flipped */
                run_tile(&e, &c, off, sx, sy, vf, !hf);
            } else if (!o->bpp8 || off + 64 > 0x8000 || off + 64 > min_off) {
                if (scratch_off + 64 > OBJ_IMG_SIZE) {
                    /* scratch full: let the RDP consume the frame so far, then reuse */
                    run_close(&e);
                    raw_ptr = e.p;
                    raw_flush();
                    rspq_wait();
                    e.p = raw_ptr;
                    scratch_off = OBJ_IMG_SCRATCH;
                }
                run_tile(&e, &c, tile_scratch(o, row_stride, cc, r, hf), sx, sy, vf, 0);
            }
            /* else: entirely below min_off, invisible */
        }
    }
    run_close(&e);
    raw_ptr = e.p;
    r_stats.loads += e.loads; r_stats.load_bytes += e.tiles * 64;
    r_stats.obj_tiles += e.tiles;
    r_rect_stats(&c, e.rects, e.px);
}

static void draw_affine(const objinfo_t *o, int x, int y) {
    /* visible part of the bounding box: inside the clip rectangle (the draw
     * window, or a part of it beside the CPU-composited rectangle) */
    int cx0 = r_clip.x0 - r_clip.tx, cy0 = r_clip.y0 - r_clip.ty;
    int cx1 = r_clip.x1 - r_clip.tx, cy1 = r_clip.y1 - r_clip.ty;
    int vx0 = cx0 - x, vy0 = cy0 - y, vx1 = cx1 - x, vy1 = cy1 - y;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > o->bw) vx1 = o->bw;
    if (vy1 > o->bh) vy1 = o->bh;
    if (vx1 <= vx0 || vy1 <= vy0) return;
    vx0 &= ~7;                                  /* keep 8-byte aligned rows */
    int w = vx1 - vx0, h = vy1 - vy0;
    int pitch = (w + 7) & ~7;
    uint8_t *buf = r_scratch((uint32_t)(pitch * h));
    if (!buf) return;
    uint32_t row_stride = one_d ? (uint32_t)(o->w / 8) * (o->bpp8 ? 2 : 1) : 32;
    objinfo_t tmp;
    if (o->bpp8 && !one_d) { tmp = *o; tmp.tile &= ~1u; o = &tmp; }
    int hw = o->w / 2, hh = o->h / 2, hbw = o->bw / 2, hbh = o->bh / 2;
    if (o->pb == 0 && o->pc == 0 && o->bpp8 && min_off == 0) {
        /* scale only, 8bpp: separable -> per-column texel offsets, per-row base */
        static int16_t colofs[128];
        for (int lx = vx0; lx < vx0 + pitch; lx++) {
            int tx = ((o->pa * (lx - hbw)) >> 8) + hw;
            colofs[lx - vx0] = (int16_t)((lx < vx1 && (unsigned)tx < (unsigned)o->w) ?
                                         (tx >> 3) * 64 + (tx & 7) : -1);
        }
        const uint8_t *vr = (const uint8_t *)gba_mem.vram + 0x10000;
        for (int row = vy0; row < vy1; row++) {
            int ty = ((o->pd * (row - hbh)) >> 8) + hh;
            uint64_t *dst = (uint64_t *)(buf + (row - vy0) * pitch);
            if ((unsigned)ty >= (unsigned)o->h) {
                for (int k = 0; k < pitch / 8; k++) dst[k] = 0;
                continue;
            }
            uint32_t base = (o->tile + (uint32_t)(ty >> 3) * row_stride) * 32 + (uint32_t)(ty & 7) * 8;
            for (int x = 0; x < pitch; x += 8) {
                uint64_t acc = 0;
                for (int k = 0; k < 8; k++) {
                    int c = colofs[x + k];
                    acc = (acc << 8) | (c < 0 ? 0 : vr[((base + (uint32_t)c) & 0x7FFF) ^ GBA_BX]);
                }
                dst[x >> 3] = acc;
            }
        }
        r_stats.obj_affine++;
        r_blit_image(buf, pitch, FMT_CI8, x + vx0, y + vy0, w, h);
        return;
    }
    for (int row = vy0; row < vy1; row++) {
        int iy = row - hbh;
        int32_t bx = o->pb * iy, by = o->pd * iy;
        uint64_t *dst = (uint64_t *)(buf + (row - vy0) * pitch);
        for (int lx = vx0; lx < vx0 + pitch; lx += 8) {
            uint64_t acc = 0;
            for (int k = 0; k < 8; k++) {
                int ix = lx + k - hbw;
                int tx = ((o->pa * ix + bx) >> 8) + hw;
                int ty = ((o->pc * ix + by) >> 8) + hh;
                uint32_t idx = 0;
                if (lx + k < vx1 && (unsigned)tx < (unsigned)o->w && (unsigned)ty < (unsigned)o->h)
                    idx = obj_texel(o, row_stride, tx, ty);
                acc = (acc << 8) | idx;
            }
            dst[(lx - vx0) >> 3] = acc;
        }
    }
    r_stats.obj_affine++;
    r_blit_image(buf, pitch, FMT_CI8, x + vx0, y + vy0, w, h);
}

/* ------------------------------------------------------------ CPU-composited part */

/* Up to MAX_HOLES pairwise disjoint rectangles ("holes", GBA coordinates,
 * inside the region) whose OBJ pixels come from cpu_compose_line: the
 * places where one OBJ at a time is not the GBA's OBJ layer, gathered into
 * clusters (two conflicts far apart give two small holes, not one box over
 * both).  cpu_list holds the OBJs that meet a hole, in OAM order; each hole
 * lists the ones meeting it (indices into cpu_list, in OAM order too). */
#define MAX_HOLES 4
#define HOLE_GAP  8                     /* holes closer than this merge */
typedef struct {
    int x0, y0, x1, y1;
    int n;
    uint8_t obj[128];
    int comp_off;                       /* its composition in comp_col/comp_ps, -1: composed per line */
    int composed;                       /* done this frame */
} hole_t;
static hole_t holes[MAX_HOLES];
static int nholes;
static uint8_t cpu_list[128];
static int16_t cpu_box[128][3][4];      /* their boxes over the region (obj_boxes) */
static uint8_t cpu_nbox[128];
static int cpu_n;
static uint32_t cpu_cmax_frame, cpu_cmax;   /* obj_cpu_colour_max of this frame */
/* storage of the hole compositions (obj_cpu_draw) */
#define COMP_MAX 16384
static uint8_t comp_col[COMP_MAX], comp_ps[COMP_MAX];

static inline int fmod_i(int a, int m) {
    int r = a % m;
    return r < 0 ? r + m : r;
}

/* the bounding boxes of OBJ o's positions that meet the region, clipped */
static int obj_boxes(const objinfo_t *o, int16_t box[3][4]) {
    int xs[3], ys[3], nx, ny, n = 0;
    obj_positions(o, xs, ys, &nx, &ny);
    for (int a = 0; a < ny; a++)
        for (int b = 0; b < nx; b++) {
            box[n][0] = (int16_t)(xs[b] > R.gx0 ? xs[b] : R.gx0);
            box[n][1] = (int16_t)(ys[a] > R.gy0 ? ys[a] : R.gy0);
            box[n][2] = (int16_t)(xs[b] + o->bw < R.gx1 ? xs[b] + o->bw : R.gx1);
            box[n][3] = (int16_t)(ys[a] + o->bh < R.gy1 ? ys[a] + o->bh : R.gy1);
            n++;
        }
    return n;
}

static inline int box_meets(const int16_t *a, int x0, int y0, int x1, int y1) {
    return a[0] < x1 && x0 < a[2] && a[1] < y1 && y0 < a[3];
}

/* grow the rectangle [*x0, *x1) x [*y0, *y1) to cover [a, c) x [b, d) */
static inline void grow(int *x0, int *y0, int *x1, int *y1, int a, int b, int c, int d) {
    if (a < *x0)
        *x0 = a;
    if (b < *y0)
        *y0 = b;
    if (c > *x1)
        *x1 = c;
    if (d > *y1)
        *y1 = d;
}

/* ---- gathering the conflict rectangles into holes ---- */

typedef struct { int x0, y0, x1, y1; } hrect_t;
static hrect_t hr[MAX_HOLES + 1];
static int nhr;

static inline int hr_near(const hrect_t *a, const hrect_t *b) {
    return a->x0 < b->x1 + HOLE_GAP && b->x0 < a->x1 + HOLE_GAP &&
           a->y0 < b->y1 + HOLE_GAP && b->y0 < a->y1 + HOLE_GAP;
}

static inline int hr_area(const hrect_t *a) {
    return (a->x1 - a->x0) * (a->y1 - a->y0);
}

static inline hrect_t hr_union(const hrect_t *a, const hrect_t *b) {
    hrect_t u = *a;
    grow(&u.x0, &u.y0, &u.x1, &u.y1, b->x0, b->y0, b->x1, b->y1);
    return u;
}

/* add r, absorbing every hole near it (the holes stay pairwise apart) */
static void hr_insert(hrect_t r) {
    for (int k = 0; k < nhr;) {
        if (hr_near(&hr[k], &r)) {
            r = hr_union(&r, &hr[k]);
            hr[k] = hr[--nhr];
            k = 0;                      /* the grown r may now be near an earlier one */
        } else {
            k++;
        }
    }
    hr[nhr++] = r;
}

static void hole_add(int x0, int y0, int x1, int y1) {
    hr_insert((hrect_t){ x0, y0, x1, y1 });
    while (nhr > MAX_HOLES) {
        /* too many: merge the pair whose bounding box adds the least area */
        int ba = 0, bb = 1, best = 1 << 30;
        for (int a = 0; a < nhr; a++)
            for (int b = a + 1; b < nhr; b++) {
                hrect_t u = hr_union(&hr[a], &hr[b]);
                int add = hr_area(&u) - hr_area(&hr[a]) - hr_area(&hr[b]);
                if (add < best) { best = add; ba = a; bb = b; }
            }
        hrect_t u = hr_union(&hr[ba], &hr[bb]);
        hr[bb] = hr[--nhr];                     /* bb > ba: remove bb first */
        hr[ba] = hr[--nhr];
        hr_insert(u);
    }
}

int obj_cpu_begin(uint8_t lists[4][128], const int counts[4], int blend, uint32_t bgpm,
                  uint8_t codes[OBJ_CPU_MAX_ITEMS]) {
    nholes = 0;
    nhr = 0;
    uint32_t mosaic = io16(REG_MOSAIC);
    int omos = (mosaic & 0xFF00) != 0;          /* OBJ mosaic larger than 1x1 */
    static uint8_t listed[128];
    static int16_t box[128][3][4];
    static uint8_t nbox[128];
    uint8_t idx[128];
    int n = 0;
    memset(listed, 0, sizeof listed);
    for (int p = 0; p < 4; p++)
        for (int k = 0; k < counts[p]; k++)
            listed[lists[p][k]] = 1;
    for (int i = 0; i < 128; i++) {
        if (!listed[i])
            continue;
        const objinfo_t *o = &info[i];
        nbox[n] = (uint8_t)obj_boxes(o, box[n]);
        if (!nbox[n])
            continue;
        if (omos && o->mosaic)
            for (int b = 0; b < nbox[n]; b++)
                hole_add(box[n][b][0], box[n][b][1], box[n][b][2], box[n][b][3]);
        idx[n++] = (uint8_t)i;
    }
    /* a later OBJ with a better priority over an earlier one; with alpha
     * blending (OBJ_CPU_BLEND_*), any OBJ over another one that blends */
    uint32_t objpm = 0;                         /* priorities of the OBJs */
    for (int k = 0; k < n; k++)
        objpm |= 1u << info[idx[k]].prio;
    int budget = 4096;                          /* third-OBJ box tests (then: always a hole) */
    for (int a = 0; a < n; a++) {
        int pa = info[idx[a]].prio, sa = info[idx[a]].semi;
        if (!pa && !blend)
            continue;
        for (int b = a + 1; b < n; b++) {
            /* The later OBJ's transparent texels lift the earlier one's
             * pixels to its priority pb.  That only changes the picture
             * where a layer that painter's order draws after the earlier
             * OBJ, a priority in [pb, pa), lies under the lifted pixel: a
             * displayed BG (an OBJ wins a tie with a BG), or a third OBJ
             * meeting the overlap. */
            int pb = info[idx[b]].prio;
            uint32_t lift = pb < pa ? (1u << pa) - (1u << pb) : 0;
            int over = (bgpm & lift) != 0;
            if (blend == OBJ_CPU_BLEND_ALL || (blend == OBJ_CPU_BLEND_SEMI && (sa || info[idx[b]].semi)))
                over = 1;
            if (!over && !(objpm & lift))
                continue;
            for (int i = 0; i < nbox[a]; i++)
                for (int j = 0; j < nbox[b]; j++) {
                    const int16_t *p = box[a][i], *q = box[b][j];
                    int ix0 = p[0] > q[0] ? p[0] : q[0], iy0 = p[1] > q[1] ? p[1] : q[1];
                    int ix1 = p[2] < q[2] ? p[2] : q[2], iy1 = p[3] < q[3] ? p[3] : q[3];
                    if (ix0 >= ix1 || iy0 >= iy1)
                        continue;
                    int third = over;
                    for (int c = 0; c < n && !third; c++) {
                        if (c == a || c == b || !((lift >> info[idx[c]].prio) & 1))
                            continue;
                        if (--budget < 0) {
                            third = 1;
                            break;
                        }
                        for (int e = 0; e < nbox[c]; e++)
                            third |= box_meets(box[c][e], ix0, iy0, ix1, iy1);
                    }
                    if (third)
                        hole_add(ix0, iy0, ix1, iy1);
                }
        }
    }
    if (!nhr)
        return 0;
    /* the holes, their OBJs, and per hole the (priority, semi) combinations
     * of those: a pixel of a composition takes both from one OBJ */
    int off = 0;
    for (int h = 0; h < nhr; h++) {
        hole_t *H = &holes[h];
        H->x0 = hr[h].x0; H->y0 = hr[h].y0; H->x1 = hr[h].x1; H->y1 = hr[h].y1;
        H->n = 0;
        H->composed = 0;
        int area = hr_area(&hr[h]);
        H->comp_off = off + area <= COMP_MAX ? off : -1;
        if (H->comp_off >= 0)
            off += area;
    }
    nholes = nhr;
    cpu_n = 0;
    for (int k = 0; k < n; k++) {
        int in = 0;
        for (int h = 0; h < nholes; h++) {
            hole_t *H = &holes[h];
            int meets = 0;
            for (int b = 0; b < nbox[k]; b++)
                meets |= box_meets(box[k][b], H->x0, H->y0, H->x1, H->y1);
            if (meets) {
                H->obj[H->n++] = (uint8_t)cpu_n;
                in = 1;
            }
        }
        if (!in)
            continue;
        memcpy(cpu_box[cpu_n], box[k], sizeof box[k]);
        cpu_nbox[cpu_n] = nbox[k];
        cpu_list[cpu_n++] = idx[k];
    }
    int nc = 0;
    for (int h = 0; h < nholes; h++) {
        uint8_t seen = 0;
        for (int k = 0; k < holes[h].n; k++) {
            const objinfo_t *o = &info[cpu_list[holes[h].obj[k]]];
            int code = o->prio | (o->semi << 2);
            if (!(seen & (1u << code))) {
                seen |= (uint8_t)(1u << code);
                codes[nc++] = (uint8_t)(code | (h << 3));
            }
        }
    }
    return nc;
}

void obj_cpu_end(void) {
    nholes = 0;
}

/* A pixel of item id (hole id >> 3, code id & 7) lies inside the hole and
 * inside the box of an OBJ with that code (the OBJ whose texel, opaque or
 * transparent, gave the pixel its priority and semi-transparency), so the
 * item stays inside the union of those boxes clipped to the hole. */
int obj_cpu_box(int id, int *x0, int *y0, int *x1, int *y1) {
    int h = id >> 3, code = id & 7;
    if (h >= nholes)
        return 0;
    const hole_t *H = &holes[h];
    int a = H->x1, b = H->y1, c = H->x0, d = H->y0;
    for (int k = 0; k < H->n; k++) {
        int e = H->obj[k];
        const objinfo_t *o = &info[cpu_list[e]];
        if ((o->prio | (o->semi << 2)) != code)
            continue;
        for (int q = 0; q < cpu_nbox[e]; q++) {
            const int16_t *bx = cpu_box[e][q];
            int bx0 = bx[0] > H->x0 ? bx[0] : H->x0, by0 = bx[1] > H->y0 ? bx[1] : H->y0;
            int bx1 = bx[2] < H->x1 ? bx[2] : H->x1, by1 = bx[3] < H->y1 ? bx[3] : H->y1;
            if (bx0 < bx1 && by0 < by1)
                grow(&a, &b, &c, &d, bx0, by0, bx1, by1);
        }
    }
    if (a >= c || b >= d)
        return 0;
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
    return 1;
}

/* the colour of a pixel can come from any OBJ in a hole (a transparent
 * texel passes only priority and semi-transparency down) */
uint32_t obj_cpu_colour_max(int id) {
    (void)id;
    if (cpu_cmax_frame != r_frame) {
        uint32_t m[3] = { 0, 0, 0 };
        for (int k = 0; k < cpu_n; k++) {
            uint32_t c = obj_colour_max(cpu_list[k]);
            for (int ch = 0; ch < 3; ch++, c >>= 5)
                if ((c & 31) > m[ch]) m[ch] = c & 31;
        }
        cpu_cmax = m[0] | (m[1] << 5) | (m[2] << 10);
        cpu_cmax_frame = r_frame;
    }
    return cpu_cmax;
}

/* One line of the OBJ layer inside hole H, columns [sx0, sx1), by ppu_soft's
 * rules (draw_objs): OBJs in OAM order, an opaque texel replaces the pixel
 * when there is none or its priority is better, a transparent texel of a
 * better priority still passes its priority and semi-transparency down;
 * mosaic, affine and the bitmap-mode cut as there.  col = OBJ palette index
 * (0: no pixel), ps = prio | semi << 2, both indexed from sx0. */
static void cpu_compose_line(const hole_t *H, int line, int sx0, int sx1, uint8_t *col, uint8_t *ps) {
    int w = sx1 - sx0;
    uint32_t mosaic = io16(REG_MOSAIC);
    int mh = (int)((mosaic >> 8) & 15) + 1, mv = (int)((mosaic >> 12) & 15) + 1;
    uint8_t pr[320];
    for (int x = 0; x < w; x++) {
        col[x] = 0;
        pr[x] = 4;
        ps[x] = 0xFF;
    }
    for (int e = 0; e < H->n; e++) {
        const objinfo_t *o = &info[cpu_list[H->obj[e]]];
        int bw = o->bw, bh = o->bh, ox = o->x, oy = o->y, row;
        if (o->own_pos) {
            row = line - oy;
            if (row < 0 || row >= bh)
                continue;
        } else {
            row = (line - oy) & 0xFF;
            if (row >= bh)
                continue;
        }
        if (ox >= sx1 || ox + bw <= sx0)
            continue;
        int mos = o->mosaic;
        if (mos && mv > 1) {
            row = (line - fmod_i(line, mv)) - oy;
            if (!o->own_pos)
                row &= 0xFF;
            if (row < 0 || row >= bh)
                row = 0;
        }
        int bpp8 = o->bpp8, ow = o->w, oh = o->h;
        uint32_t tile = o->tile, rs = row_stride_of(o);
        if (bpp8 && !one_d)
            tile &= ~1u;
        uint8_t prio = o->prio, code = (uint8_t)(prio | (o->semi << 2));
        int xa = ox < sx0 ? sx0 : ox, xb = ox + bw < sx1 ? ox + bw : sx1;
        for (int x = xa; x < xb; x++) {
            int lx = x - ox;
            if (mos && mh > 1) {
                int mx = x - fmod_i(x, mh);
                lx = mx < ox ? 0 : mx - ox;
            }
            int tx, ty;
            if (o->affine) {
                int ix = lx - bw / 2, iy = row - bh / 2;
                tx = ((o->pa * ix + o->pb * iy) >> 8) + ow / 2;
                ty = ((o->pc * ix + o->pd * iy) >> 8) + oh / 2;
                if ((unsigned)tx >= (unsigned)ow || (unsigned)ty >= (unsigned)oh)
                    continue;
            } else {
                tx = o->hf ? ow - 1 - lx : lx;
                ty = o->vf ? oh - 1 - row : row;
            }
            uint32_t t = tile + (uint32_t)(ty >> 3) * rs + (uint32_t)(tx >> 3) * (bpp8 ? 2u : 1u);
            uint32_t inner = bpp8 ? (uint32_t)((ty & 7) * 8 + (tx & 7)) : (uint32_t)((ty & 7) * 4 + ((tx & 7) >> 1));
            uint32_t off = (t * 32 + inner) & 0x7FFFu;
            if (off < min_off)
                continue;
            uint32_t v = r_vram8(0x10000 + off);
            if (!bpp8)
                v = (tx & 1) ? v >> 4 : v & 15;
            int b = x - sx0;
            if (!v) {
                if (col[b] && prio < pr[b]) {
                    pr[b] = prio;
                    ps[b] = code;
                }
                continue;
            }
            if (!col[b] || prio < pr[b]) {
                col[b] = (uint8_t)(bpp8 ? v : ((uint32_t)o->pbank << 4) | v);
                pr[b] = prio;
                ps[b] = code;
            }
        }
    }
}

/* The composition of each hole, done once per frame while the holes fit
 * (the items of all (priority, semi) combinations read it); a hole that
 * does not fit is composed again for each item, over the draw window. */
void obj_cpu_draw(int id, int fx) {
    int h = id >> 3, code = id & 7;
    if (h >= nholes)
        return;
    hole_t *H = &holes[h];
    /* the part of the hole inside the draw window (the blend path draws an
     * item once per rectangle) */
    int x0 = H->x0 > R.wx0 ? H->x0 : R.wx0, y0 = H->y0 > R.wy0 ? H->y0 : R.wy0;
    int x1 = H->x1 < R.wx1 ? H->x1 : R.wx1, y1 = H->y1 < R.wy1 ? H->y1 : R.wy1;
    if (x0 >= x1 || y0 >= y1)
        return;
    int hw = H->x1 - H->x0, w = x1 - x0, hh = y1 - y0, pitch = (w + 7) & ~7;
    uint8_t *buf = r_scratch((uint32_t)(pitch * hh));
    if (!buf)
        return;
    int stored = H->comp_off >= 0;
    if (stored && !H->composed) {
        for (int y = 0; y < H->y1 - H->y0; y++)
            cpu_compose_line(H, H->y0 + y, H->x0, H->x1, comp_col + H->comp_off + y * hw,
                             comp_ps + H->comp_off + y * hw);
        H->composed = 1;
    }
    static uint8_t col[320 + 8], ps[320 + 8];
    for (int y = 0; y < hh; y++) {
        const uint8_t *c, *q;
        if (stored) {
            c = comp_col + H->comp_off + (y0 - H->y0 + y) * hw + (x0 - H->x0);
            q = comp_ps + H->comp_off + (y0 - H->y0 + y) * hw + (x0 - H->x0);
        } else {
            cpu_compose_line(H, y0 + y, x0, x1, col, ps);
            c = col;
            q = ps;
        }
        uint64_t *d = (uint64_t *)(buf + y * pitch);     /* uncached: 8 pixels a store */
        for (int x = 0; x < pitch; x += 8) {
            uint64_t acc = 0;
            for (int k = 0; k < 8; k++)
                acc = (acc << 8) | (x + k < w && q[x + k] == code ? c[x + k] : 0);
            d[x >> 3] = acc;
        }
    }
    r_bind(PAL_OBJ, fx);
    r_scissor_region();
    r_blit_image(buf, pitch, FMT_CI8, x0, y0, w, hh);
}

/* The clip windows of an OBJ at (x, y), w x h: the draw window minus the
 * holes that meet the OBJ there, cut into bands at the holes' y edges and
 * each band into the x runs between the holes covering it (at most
 * (2H+1)(H+1) rectangles for H holes). */
#define MAX_CLIPS ((2 * MAX_HOLES + 1) * (MAX_HOLES + 1))
static int obj_clips(int x, int y, int w, int h, int rc[MAX_CLIPS][4]) {
    int hl[MAX_HOLES][4], nh = 0;
    for (int k = 0; k < nholes; k++) {
        const hole_t *H = &holes[k];
        if (x >= H->x1 || x + w <= H->x0 || y >= H->y1 || y + h <= H->y0 ||
            H->x0 >= R.wx1 || H->x1 <= R.wx0 || H->y0 >= R.wy1 || H->y1 <= R.wy0)
            continue;
        hl[nh][0] = H->x0; hl[nh][1] = H->y0; hl[nh][2] = H->x1; hl[nh][3] = H->y1;
        nh++;
    }
    if (!nh) {
        rc[0][0] = R.wx0; rc[0][1] = R.wy0; rc[0][2] = R.wx1; rc[0][3] = R.wy1;
        return 1;
    }
    /* band edges: the window's and the holes' inside it, sorted */
    int ys[2 * MAX_HOLES + 2], ny = 0;
    ys[ny++] = R.wy0;
    ys[ny++] = R.wy1;
    for (int k = 0; k < nh; k++)
        for (int e = 1; e <= 3; e += 2)
            if (hl[k][e] > R.wy0 && hl[k][e] < R.wy1)
                ys[ny++] = hl[k][e];
    for (int a = 1; a < ny; a++)
        for (int b = a; b > 0 && ys[b - 1] > ys[b]; b--) {
            int t = ys[b]; ys[b] = ys[b - 1]; ys[b - 1] = t;
        }
    int n = 0;
    for (int band = 0; band + 1 < ny; band++) {
        int ya = ys[band], yb = ys[band + 1];
        if (ya >= yb || y >= yb || y + h <= ya)
            continue;
        /* the holes covering this band, by x (disjoint holes covering the
         * same rows have disjoint x ranges) */
        int hx[MAX_HOLES][2], nx = 0;
        for (int k = 0; k < nh; k++)
            if (hl[k][1] <= ya && hl[k][3] >= yb) {
                int b = nx++;
                for (; b > 0 && hx[b - 1][0] > hl[k][0]; b--) {
                    hx[b][0] = hx[b - 1][0]; hx[b][1] = hx[b - 1][1];
                }
                hx[b][0] = hl[k][0]; hx[b][1] = hl[k][2];
            }
        int cur = R.wx0;
        for (int k = 0; k <= nx; k++) {
            int xe = k < nx ? hx[k][0] : R.wx1;
            if (xe > R.wx1) xe = R.wx1;
            if (cur < xe && x < xe && x + w > cur) {
                rc[n][0] = cur; rc[n][1] = ya; rc[n][2] = xe; rc[n][3] = yb;
                n++;
            }
            if (k < nx && hx[k][1] > cur)
                cur = hx[k][1];
        }
    }
    return n;
}

void obj_draw(int i, int fx) {
    const objinfo_t *o = &info[i];
    r_bind(PAL_OBJ, fx);
    r_stats.obj_drawn++;
    int xs[3], ys[3], nx, ny;
    obj_positions(o, xs, ys, &nx, &ny);
    for (int a = 0; a < ny; a++)
        for (int b = 0; b < nx; b++) {
            int x = xs[b], y = ys[a];
            if (x >= R.wx1 || x + o->bw <= R.wx0 || y >= R.wy1 || y + o->bh <= R.wy0)
                continue;                       /* outside the draw window */
            /* the clip windows: the draw window, or its parts around the
             * CPU-composited holes */
            int rc[MAX_CLIPS][4];
            int nrc = obj_clips(x, y, o->bw, o->bh, rc);
            for (int k = 0; k < nrc; k++) {
                r_scissor(rc[k][0], rc[k][1], rc[k][2], rc[k][3]);
                if (o->affine && !o->ident)
                    draw_affine(o, x, y);
                else
                    draw_regular(o, x, y);
            }
        }
}

uint32_t obj_colour_max(int i) {
    const objinfo_t *o = &info[i];
    uint32_t lo = o->bpp8 ? 257 : 256 + o->pbank * 16u + 1, hi = o->bpp8 ? 511 : lo + 14;
    uint32_t m[3] = { 0, 0, 0 };
    for (uint32_t k = lo; k <= hi; k++) {
        uint32_t c = r_pal16(k);
        for (int ch = 0; ch < 3; ch++, c >>= 5)
            if ((c & 31) > m[ch]) m[ch] = c & 31;
    }
    return m[0] | (m[1] << 5) | (m[2] << 10);
}

int obj_box(int i, int *x0, int *y0, int *x1, int *y1) {
    const objinfo_t *o = &info[i];
    int xs[3], ys[3], nx, ny;
    obj_positions(o, xs, ys, &nx, &ny);
    if (!nx || !ny) return 0;
    *x0 = xs[0]; *x1 = xs[nx - 1] + o->bw;
    *y0 = ys[0]; *y1 = ys[ny - 1] + o->bh;
    return 1;
}

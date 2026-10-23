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

void obj_track(uint32_t pages) {
    for (int p = 0; p < 8; p++)
        if (pages & (1u << p))
            fresh_bits[p * 4] = fresh_bits[p * 4 + 1] = fresh_bits[p * 4 + 2] = fresh_bits[p * 4 + 3] = 0;
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
            int b0 = __builtin_ctz(stale);
            uint32_t x = ~(stale >> b0);
            int len = x ? __builtin_ctz(x) : 32 - b0;
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
    int shift = __builtin_ctz((unsigned)tw);
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
    const int gx0 = R.gx0, gy0 = R.gy0, gx1 = R.gx1, gy1 = R.gy1;
    const int hf = o->hf, vf = o->vf;
    const rclip_t c = r_clip;
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
    /* visible part of the bounding box */
    int vx0 = R.gx0 - x, vy0 = R.gy0 - y, vx1 = R.gx1 - x, vy1 = R.gy1 - y;
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

void obj_draw(int i, int fx) {
    const objinfo_t *o = &info[i];
    r_bind(PAL_OBJ, fx);
    r_scissor_region();
    r_stats.obj_drawn++;
    int xs[3], ys[3], nx, ny;
    obj_positions(o, xs, ys, &nx, &ny);
    for (int a = 0; a < ny; a++)
        for (int b = 0; b < nx; b++) {
            if (o->affine && !o->ident) draw_affine(o, xs[b], ys[a]);
            else draw_regular(o, xs[b], ys[a]);
        }
}

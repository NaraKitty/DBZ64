/*
 * gba_render.c - GBA PPU -> N64 RDP renderer: frame orchestration.
 *
 * Pipeline per frame (reference semantics: runtime/ppu_soft.c):
 *
 *  1. Wait for the RDP to finish the previous frame (all caches are single
 *     buffered), take the VRAM dirty pages, compare dirty BG pages against a
 *     shadow in 32-byte chunks and mark dirty OBJ pages (render_bg.c,
 *     render_obj.c).
 *  2. Fill the area outside the view region black and the region with the
 *     backdrop (BG palette 0); forced blank fills the region white.
 *  3. Draw the layers in painter's order: for priority 3..0 the BGs of that
 *     priority (BG3..BG0), then the OBJs of that priority (OAM 127..0).  This
 *     reproduces ppu_soft's per-pixel "topmost opaque layer" exactly.
 *       - text BGs: CI8 layer cache (512x256 ring per BG in world tile space),
 *         updated incrementally, blitted in 64x32 chunks (fully transparent
 *         chunks are skipped);
 *       - affine / bitmap BGs: rasterised on the CPU into a CI8 (RGBA16 for
 *         modes 3/5) screen buffer, then blitted;
 *       - OBJs: one 8x8 texture rectangle per tile from a CPU-converted
 *         shadow of OBJ VRAM holding the rows mirrored, drawn in 1-cycle mode
 *         (unflipped sprites with a negative S step: COPY mode cannot flip
 *         horizontally); affine OBJs are rasterised on the CPU.
 *       - where one OBJ at a time is not the GBA's OBJ layer, the OBJ pixels
 *         of up to 4 rectangles ("holes") are composited on the CPU with
 *         ppu_soft's rules and drawn per priority (render_obj.c,
 *         obj_cpu_begin): a later OBJ with a better priority over an earlier
 *         one where a displayed BG or a third OBJ has a priority between
 *         theirs (its transparent texels still pass their priority down),
 *         OBJ mosaic, and with alpha
 *         blending an OBJ that blends over another OBJ (the GBA blends the
 *         OBJ layer's top pixel with the BG beneath).
 *       - BG mosaic: text BGs with mosaic are rasterised on the CPU, affine
 *         and bitmap BGs apply it in their CPU raster.
 *     Texture loads and rectangles go to the RDP as raw command lists
 *     (render_raw.c); modes and TLUTs through rdpq.  COPY mode is used
 *     whenever no blending is needed (the RDP scissor is never used: in COPY
 *     mode a non-zero left bound hangs the RDP, rectangles are clipped on
 *     the CPU instead).
 *  4. Colour effects (BLDCNT):
 *       - brighten/darken and alpha with EVB=0 (fades) depend only on the top
 *         pixel: the first-target layers get a remapped TLUT (exact).
 *       - alpha with EVB>0 (and semi-transparent OBJs): where a first target
 *         may have an opaque pixel, the frame is drawn in 1-cycle mode into a
 *         32-bit intermediate where a colour c is stored as 4c (the sum of
 *         two layers cannot wrap the 8-bit channel), and the first-target
 *         layers are blended over it with a per-(EVA,EVB) blender/combiner
 *         configuration plus a TLUT remap (blend_table.h).  A final pass
 *         converts to the display: a 2-cycle "2x" pass (v -> min(31, v>>2),
 *         exact for v <= 191) or, when the first-target colours in view can
 *         exceed that, a TLUT pass that reads the intermediate as bytes and
 *         maps every value through a table (saturation included).
 *       - BLDCNT's rule that a pixel blends only when the layer under it is a
 *         2nd target: where a layer that is not a 2nd target (or the backdrop)
 *         can lie under a first target, every draw also writes a Z "tag"
 *         (2nd target or not) and first-target layers are drawn twice with a
 *         DECAL depth test (blended over tagged 2nd targets, plain elsewhere).
 *       The 32-bit path is slow on hardware (docs/RDP_COST.md), so it only
 *       covers rectangles of 8x8 cells of the region: cells where a first
 *       target (or semi-transparent OBJ) may have an opaque pixel over a
 *       2nd target, from the layer cache's per-tile flags (all cells of a BG
 *       with mosaic) and OBJ boxes (for the CPU-composited OBJ pixels of one
 *       hole and priority/semi code, the boxes of its OBJs inside the hole),
 *       and with EVB = 0 only the cells that need tags.  Every item is
 *       clipped to the draw window of the pass and rectangle being drawn
 *       (the composed holes in the other OBJs as well).  Walking the layers
 *       bottom-up (a tile without transparent pixels covers what is under
 *       it), tags are written only in the cells where a pixel under a first
 *       target may not be a 2nd target, and a cell where no pixel under any
 *       first target can be a 2nd target needs no 32-bit work at all.
 *       Everything else (and every layer above the last first target,
 *       after the final pass) is drawn straight into the display with the
 *       normal COPY / 1-cycle path: a first target there has no pixel, or
 *       only non-targets under it and is drawn without its effect (with
 *       EVB = 0, faded where only 2nd targets lie under it).
 *       The game sets BLDCNT for an empty BG0 in its field maps and during
 *       dialogues, so most "blend" frames end up with no 32-bit work at all.
 *       Approximations (measured by tests/n64_render):
 *         * 132 of the 272 (EVA,EVB>0) pairs have no exact RDP configuration;
 *           they are off by one (at most 2) in some channels for up to 7% of
 *           the (a,b) colour value pairs ('wrong' in blend_table.h).  Of the
 *           game's combinations only 14/6 is affected; 9/16, 16/16, 16/6,
 *           8/16, 6/16, all EVA+EVB=16 crossfades and all EVB=0 fades are exact.
 *         * the beneath colour of a blend is the colour already in the
 *           intermediate, i.e. a blended result when two first-target layers
 *           stack (ppu_soft uses the raw colour of the second layer); with
 *           EVB=0 (the game's only multi-target case) this makes no difference.
 *           (Brighten/darken is exact here: when a semi-transparent OBJ may
 *           blend with a layer that is both a 1st and a 2nd target, that layer
 *           is drawn raw with its own tag and brightened/darkened at the end
 *           where it is still on top.)
 *         * windows and the OBJ window are not implemented (neither game
 *           enables them).
 */
#include <string.h>
#include <malloc.h>
#include "render_internal.h"
#include "render_raw.h"
#include "blend_table.h"

rtarget_t R;
rclip_t r_clip;
gba_render_stats_t r_stats;
uint32_t r_frame;
const gba_view_t *r_view;

/* 2x final pass constants: X = v + v*FIN_C0/256, Y = X + X*FIN_C1/256 (exact for v <= 191,
 * see tools/gen_blend_table.py) */
#define FIN_C0 85
#define FIN_C1 128
/* combiner constant that maps a TLUT texel c (5 bit) to 4c in the intermediate */
#define QUARTER_C 124

/* ------------------------------------------------------------ memory */

#define SCRATCH_SIZE (160 * 1024)
static uint8_t *scratch_mem;        /* uncached */
static uint32_t scratch_used;
static uint8_t *raster_mem[2];      /* uncached */
static uint32_t raster_size[2];

void *r_scratch(uint32_t bytes) {
    bytes = (bytes + 15) & ~15u;
    if (bytes > SCRATCH_SIZE)
        return NULL;
    if (scratch_used + bytes > SCRATCH_SIZE) {
        /* full: let the RDP finish everything drawn from it so far, then reuse */
        raw_flush();
        rspq_wait();
        scratch_used = 0;
    }
    void *p = scratch_mem + scratch_used;
    scratch_used += bytes;
    return p;
}

void *r_raster_buf(int which, uint32_t bytes) {
    if (bytes > raster_size[which]) {
        if (raster_mem[which])
            free_uncached(raster_mem[which]);
        raster_mem[which] = malloc_uncached_aligned(64, bytes);
        raster_size[which] = bytes;
    }
    return raster_mem[which];
}

/* ------------------------------------------------------------ palettes */

static uint16_t *tlut_mem;          /* uncached [3 fx][2 pal][256] */
static uint16_t *fin_tlut;          /* uncached [3][256]: final pass TLUTs */
static uint32_t tlut_built[3][2];   /* frame number of the last build */
static uint8_t tau_id[32], tau_bright[32], tau_alpha[32];
static const blend_cfg_t *bcfg;     /* alpha config for this frame (EVB>0) */
static int alpha_needs_blend;       /* FX_ALPHA uses the blender (EVB>0) */

const uint8_t *r_tau(int fx) {
    return fx == FX_BRIGHT ? tau_bright : fx == FX_ALPHA ? tau_alpha : tau_id;
}

static void build_tlut(int pal, int fx) {
    uint16_t *dst = tlut_mem + (fx * 2 + pal) * 256;
    const uint8_t *tau = r_tau(fx);
    uint32_t base = pal == PAL_OBJ ? 256 : 0;
    /* uncached destination: assemble and write 4 entries (64 bits) at a time */
    uint64_t *d = (uint64_t *)dst;
    if (fx == FX_PLAIN) {
        /* 4 entries per 64-bit load: the native words hold the guest
         * halfwords 1,0,3,2 (GBA_HX = 2); BGR555 -> RGBA5551 in all lanes */
        const uint64_t *src = (const uint64_t *)gba_mem.pal + base / 4;
        for (int q = 0; q < 64; q++) {
            uint64_t x = src[q];
            x = ((x << 16) & 0xFFFF0000FFFF0000ull) | ((x >> 16) & 0x0000FFFF0000FFFFull);
            uint64_t w = ((x & 0x001F001F001F001Full) << 11) | ((x & 0x03E003E003E003E0ull) << 1) |
                         ((x >> 9) & 0x003E003E003E003Eull) | 0x0001000100010001ull;
            if (q == 0)
                w &= 0x0000FFFFFFFFFFFFull;     /* colour 0 is transparent */
            d[q] = w;
        }
        tlut_built[fx][pal] = r_frame;
        return;
    }
    for (int i = 0; i < 256; i += 4) {
        uint64_t w = 0;
        for (int k = 0; k < 4; k++) {
            uint32_t c = r_pal16(base + i + k);
            if (fx != FX_PLAIN)
                c = tau[c & 31] | (tau[(c >> 5) & 31] << 5) | (tau[(c >> 10) & 31] << 10);
            w = (w << 16) | (i + k ? r_conv(c) : 0);   /* colour 0 is transparent */
        }
        d[i >> 2] = w;
    }
    tlut_built[fx][pal] = r_frame;
}

/* ------------------------------------------------------------ pipeline */

/* COPY: BGs on the display; ONECYC: OBJs on the display (they may need an
 * H-flip, which COPY mode cannot do); QUARTER / BLEND: the 32-bit path */
enum { PIPE_NONE, PIPE_COPY, PIPE_ONECYC, PIPE_QUARTER, PIPE_BLEND };
static int pipe_mode;
/* Z "tag" state for the 32-bit path when BLDCNT's 2nd-target rule needs the
 * identity of the layer beneath: every draw writes a tag (prim depth) that
 * says whether its layer is a 2nd target; first-target layers are drawn with
 * a DECAL depth test against each tag value (see gba_render_frame). */
enum { ZT_NONE, ZT_WRITE, ZT_DECAL, ZT_KEEP };   /* KEEP: write the tag, keep the colour */
#define TAG_NONTARGET 0x1000
#define TAG_TARGET    0x2000
#define TAG_DEFERRED  0x3000            /* 2nd target, its brighten/darken still to do */
static int z_state = ZT_NONE, z_tag;
static int pipe_z = -1;                 /* z_state*0x10000 + z_tag of the current pipeline */
static int pipe_tlut;               /* fx*2+pal of the loaded TLUT, -1 none, -2 = TLUT off */

/* no clip window: nothing is drawn until the next r_scissor */
static void clip_reset(void) {
    r_clip.x0 = r_clip.y0 = r_clip.x1 = r_clip.y1 = -1;
}

static void set_pipe(int mode, int tlut_on) {
    if (mode == PIPE_COPY) {
        rdpq_set_mode_copy(true);
        rdpq_mode_tlut(tlut_on ? TLUT_RGBA16 : TLUT_NONE);
        pipe_z = ZT_NONE * 0x10000;
    } else {
        rdpq_set_mode_standard();
        rdpq_mode_tlut(tlut_on ? TLUT_RGBA16 : TLUT_NONE);
        rdpq_mode_alphacompare(1);
        rdpq_mode_dithering(DITHER_NONE_NONE);
        if (mode == PIPE_ONECYC) {
            /* texel straight to the 16-bit display (8-bit expansion of a
             * 5-bit channel truncates back to the same value) */
            rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, TEX0), (0, 0, 0, TEX0)));
        } else if (mode == PIPE_QUARTER) {
            rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (0, 0, 0, TEX0)));
            rdpq_set_prim_color(RGBA32(QUARTER_C, QUARTER_C, QUARTER_C, 255));
        } else {
            const blend_cfg_t *b = bcfg;
            if (b->comb == 0)
                rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (0, 0, 0, TEX0)));
            else
                rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, TEX0), (0, 0, 0, TEX0)));
            rdpq_set_prim_color(RGBA32(b->C, b->C, b->C, 255));
            switch (b->variant) {
            case 0: rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, FOG_ALPHA, MEMORY_RGB, INV_MUX_ALPHA))); break;
            case 1: rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, FOG_ALPHA, MEMORY_RGB, ONE))); break;
            case 2: rdpq_mode_blender(RDPQ_BLENDER((MEMORY_RGB, FOG_ALPHA, IN_RGB, INV_MUX_ALPHA))); break;
            default: rdpq_mode_blender(RDPQ_BLENDER((MEMORY_RGB, FOG_ALPHA, IN_RGB, ONE))); break;
            }
            rdpq_set_fog_color(RGBA32(0, 0, 0, b->a1 * 8));
        }
        if (z_state != ZT_NONE) {
            rdpq_mode_zoverride(true, 0, 0);
            rdpq_set_prim_depth_raw((uint16_t)z_tag, 0);
            if (z_state == ZT_DECAL) {
                rdpq_mode_zbuf(true, false);
                rdpq_mode_zmode(ZMODE_DECAL);
            } else {
                rdpq_mode_zbuf(false, true);
            }
            if (z_state == ZT_KEEP)     /* (P*0 + M*1) */
                rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, ZERO, MEMORY_RGB, ONE)));
        }
        pipe_z = z_state * 0x10000 + z_tag;
    }
}

void r_bind(int pal, int fx) {
    int mode;
    if (!R.is32)
        mode = pal == PAL_OBJ ? PIPE_ONECYC : PIPE_COPY;
    else
        mode = (fx == FX_ALPHA && alpha_needs_blend) ? PIPE_BLEND : PIPE_QUARTER;
    int tlut_on = pal != PAL_DIRECT;
    int want = tlut_on ? fx * 2 + pal : -2;
    if (mode != pipe_mode || (pipe_tlut == -2) != (want == -2) ||
        (R.is32 && pipe_z != z_state * 0x10000 + z_tag)) {
        raw_flush();
        set_pipe(mode, tlut_on);
        pipe_mode = mode;
        r_clip.copy = mode == PIPE_COPY;
        if (want == -2)
            pipe_tlut = -2;
        else if (pipe_tlut == -2)
            pipe_tlut = -1;
    }
    if (tlut_on && pipe_tlut != want) {
        if (tlut_built[fx][pal] != r_frame)
            build_tlut(pal, fx);
        raw_flush();
        rdpq_tex_upload_tlut(tlut_mem + want * 256, 0, 256);
        pipe_tlut = want;
    }
}

/* Clip rectangle (target coordinates).  The RDP scissor is never used for
 * this: in COPY mode a scissor with a non-zero left bound hangs the RDP, so
 * the hardware scissor stays at the whole surface and every rectangle is
 * clipped here (which also saves the RDP the hidden pixels). */
void r_scissor(int gx0, int gy0, int gx1, int gy1) {
    if (gx0 < R.wx0) gx0 = R.wx0;
    if (gy0 < R.wy0) gy0 = R.wy0;
    if (gx1 > R.wx1) gx1 = R.wx1;
    if (gy1 > R.wy1) gy1 = R.wy1;
    r_clip.tx = R.tx; r_clip.ty = R.ty;
    r_clip.x0 = gx0 + R.tx; r_clip.y0 = gy0 + R.ty;
    r_clip.x1 = gx1 + R.tx; r_clip.y1 = gy1 + R.ty;
}

void r_scissor_region(void) {
    r_scissor(R.wx0, R.wy0, R.wx1, R.wy1);
}

static void set_window(int x0, int y0, int x1, int y1) {
    R.wx0 = x0 > R.gx0 ? x0 : R.gx0;
    R.wy0 = y0 > R.gy0 ? y0 : R.gy0;
    R.wx1 = x1 < R.gx1 ? x1 : R.gx1;
    R.wy1 = y1 < R.gy1 ? y1 : R.gy1;
}

/* ------------------------------------------------------------ cell masks */

int cm_w, cm_h;


void r_texrect(int gx, int gy, int w, int h, int s, int t, int vflip, int hflip) {
    raw_reserve(2);
    uint32_t px = 0;
    uint64_t *p = r_rect(raw_ptr, &r_clip, gx, gy, w, h, s, t, vflip, hflip, &px);
    r_rect_stats(&r_clip, p != raw_ptr, px);
    raw_ptr = p;
}

void r_blit_image(const void *buf, int pitch_bytes, tex_format_t fmt, int gx, int gy, int w, int h) {
    int ci = fmt == FMT_CI8;
    int rows = 2048 / pitch_bytes;              /* lower TMEM half (TLUT may be loaded) */
    if (rows < 1) return;
    int f = ci ? RF_CI : RF_RGBA, sz = ci ? RS_8 : RS_16;
    /* tile 1 loads, tile 0 draws with texel coordinates relative to each load */
    raw_reserve(7);
    raw_put(RAW_SYNC_PIPE);
    raw_put(raw_tex_image(f, sz, pitch_bytes / (ci ? 1 : 2), PhysicalAddr(buf)));
    raw_put(RAW_SYNC_TILE);
    raw_put(raw_set_tile(f, sz, pitch_bytes, 0, 1, 0));
    raw_put(raw_set_tile(f, sz, pitch_bytes, 0, 0, 0));
    raw_put(raw_set_tile_size(0, 0, 0, w, rows));
    raw_owner = RAW_OWNER_NONE;
    for (int r0 = 0; r0 < h; r0 += rows) {
        int r1 = r0 + rows > h ? h : r0 + rows;
        if (gy + r1 <= R.wy0 || gy + r0 >= R.wy1)
            continue;
        raw_reserve(3);
        raw_put(RAW_SYNC_TILE);
        raw_put(RAW_SYNC_LOAD);
        raw_put(raw_load_tile(1, 0, r0, w, r1));
        r_stats.loads++; r_stats.load_bytes += (uint32_t)(pitch_bytes * (r1 - r0));
        r_texrect(gx, gy + r0, w, r1 - r0, 0, 0, 0, 0);
    }
}

/* ------------------------------------------------------------ init */

static rspq_syncpoint_t last_sp;
static int have_sp;
static int force_all;
static surface_t blend_surf;
static surface_t zbuf_surf;             /* tag buffer for the 32-bit path (allocated on demand) */

void gba_render_view_native(gba_view_t *v) {
    memset(v, 0, sizeof *v);
    v->ox = 40; v->oy = 40;
    v->x0 = 0; v->y0 = 0; v->x1 = 240; v->y1 = 160;
}

void gba_render_invalidate(void) {
    force_all = 1;
}

void gba_render_invalidate_bg(int bg) {
    if (bg >= 0 && bg < 4)
        bg_invalidate_layer(bg);
}

void gba_render_init(void) {
    tlut_mem = malloc_uncached_aligned(64, 3 * 2 * 256 * 2);
    fin_tlut = malloc_uncached_aligned(64, 3 * 256 * 2);
    raw_init();
    scratch_mem = malloc_uncached_aligned(64, SCRATCH_SIZE);
    for (int i = 0; i < 32; i++)
        tau_id[i] = (uint8_t)i;
    bg_init();
    obj_init();
    /* blend intermediate and tag buffer for a 320x240 display, allocated up
     * front so blend scenes never allocate during gameplay (a larger display
     * reallocates on first use) */
    blend_surf = surface_alloc(FMT_RGBA32, 320, 240);
    zbuf_surf = surface_alloc(FMT_RGBA16, 320, 240);
    r_frame = 1;
    force_all = 1;
}

const gba_render_stats_t *gba_render_get_stats(void) {
    return &r_stats;
}

/* ------------------------------------------------------------ frame */

static inline uint32_t us_since(uint64_t t) {
    return (uint32_t)TICKS_TO_US(get_ticks() - t);
}

static void fill_g(int gx0, int gy0, int gx1, int gy1) {
    raw_flush();
    int x0 = gx0 + R.tx, y0 = gy0 + R.ty, x1 = gx1 + R.tx, y1 = gy1 + R.ty;
    if (x1 <= x0 || y1 <= y0) return;
    rdpq_fill_rectangle(x0, y0, x1, y1);
    r_stats.px_fill += (uint32_t)((x1 - x0) * (y1 - y0));
}

/* which BGs exist in each mode: 1 text, 2 affine, 3 bitmap */
static const uint8_t bg_kind[8][4] = {
    { 1, 1, 1, 1 }, { 1, 1, 2, 0 }, { 0, 0, 2, 2 }, { 0, 0, 3, 0 },
    { 0, 0, 3, 0 }, { 0, 0, 3, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
};

static void draw_item(int kind, int id, int fx, int mode, uint32_t dispcnt) {
    switch (kind) {
    case 0: bg_draw_text(id, fx); break;
    case 1: bg_draw_affine(id, fx); break;
    case 2: bg_draw_bitmap(mode, dispcnt, fx); break;
    case 3: obj_draw(id, fx); break;
    default: obj_cpu_draw(id, fx); break;
    }
}

/* draw-list items that are OBJ pixels, and of those the semi-transparent
 * ones (kind 3: one OBJ, kind 4: the CPU-composited OBJ pixels of one
 * hole and priority, id = priority | semi << 2 | hole << 3) */
static inline int item_is_obj(int kind) {
    return kind >= 3;
}
static inline int item_obj_semi(int kind, int id) {
    return kind == 3 ? obj_info(id)->semi : kind == 4 ? (id >> 2) & 1 : 0;
}

/* Rectangles of the 32-bit blend path, in cells of the region (end exclusive). */
typedef struct { int16_t i0, j0, i1, j1; } rrect_t;
#define MAX_RECTS 160

/* GBA coordinates of a rectangle of cells (clipped to the region) */
static inline void rect_px(const rrect_t *r, int *x0, int *y0, int *x1, int *y1) {
    *x0 = R.gx0 + 8 * r->i0; *y0 = R.gy0 + 8 * r->j0;
    *x1 = R.gx0 + 8 * r->i1; *y1 = R.gy0 + 8 * r->j1;
    if (*x1 > R.gx1) *x1 = R.gx1;
    if (*y1 > R.gy1) *y1 = R.gy1;
}

/* Copy the intermediate (pixel (x,y) of the region at x - R.gx0, y - R.gy0)
 * to the display through the current render mode, in texture loads of
 * cw x ch pixels: 'bpp' 4 loads RGBA32 texels, 2 loads each pixel as two
 * RGBA16 texels (for CI8 byte access); s0 = texel offset of the rectangle. */
static void final_rects(const rrect_t *rl, int nr, int cw, int ch, int as16, int s0, uint32_t dsdx) {
    const uint64_t w1 = ((uint64_t)(s0 * 32) << 48) | ((uint64_t)dsdx << 16) | 0x400;
    uint64_t *p = raw_ptr;
    uint32_t n = 0, bytes = 0;
    for (int k = 0; k < nr; k++) {
        int x0, y0, x1, y1;
        rect_px(&rl[k], &x0, &y0, &x1, &y1);
        for (int y = y0; y < y1; y += ch) {
            int ye = y + ch > y1 ? y1 : y + ch;
            int ly = y - R.gy0, fy = y + R.ty;
            uint64_t yw = ((uint64_t)((fy + ye - y) * 4) << 32) | (uint64_t)(fy * 4);
            for (int x = x0; x < x1; x += cw) {
                int xe = x + cw > x1 ? x1 : x + cw;
                int lx = x - R.gx0, fx = x + R.tx;
                if (__builtin_expect(p > raw_end - (5 + 4), 0)) {
                    raw_ptr = p;
                    raw_overflow();
                    p = raw_ptr;
                }
                p[0] = RAW_SYNC_TILE;
                p[1] = RAW_SYNC_LOAD;
                p[2] = as16 ? raw_load_tile(2, lx * 2, ly, (lx + xe - x) * 2, ly + ye - y)
                            : raw_load_tile(1, lx, ly, lx + xe - x, ly + ye - y);
                p[3] = (0x24ull << 56) | ((uint64_t)((fx + xe - x) * 4) << 44) | yw | ((uint64_t)(fx * 4) << 12);
                p[4] = w1;
                p += 5;
                n++;
                bytes += (uint32_t)((xe - x) * 4 * (ye - y));
            }
        }
    }
    raw_ptr = p;
    r_stats.rects += n;
    r_stats.loads += n; r_stats.load_bytes += bytes;
}

static uint32_t rects_area(const rrect_t *rl, int nr) {
    uint32_t a = 0;
    for (int k = 0; k < nr; k++) {
        int x0, y0, x1, y1;
        rect_px(&rl[k], &x0, &y0, &x1, &y1);
        a += (uint32_t)((x1 - x0) * (y1 - y0));
    }
    return a;
}

static void final_pass(const rrect_t *rl, int nr) {
    /* 32-bit intermediate (v = 4c, up to 255) -> display: min(31, v>>2) */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, 0, PRIM, TEX0), (0, 0, 0, 1),
                                      (COMBINED, 0, ENV, COMBINED), (0, 0, 0, 1)));
    rdpq_set_prim_color(RGBA32(FIN_C0, FIN_C0, FIN_C0, 255));
    rdpq_set_env_color(RGBA32(FIN_C1, FIN_C1, FIN_C1, 255));
    rdpq_mode_dithering(DITHER_NONE_NONE);
    pipe_mode = PIPE_NONE;
    pipe_tlut = -1;
    const int CWF = 64, CHF = 16;               /* 1024 RGBA32 texels = full TMEM (hi/lo halves) */
    raw_reserve(7);
    raw_put(RAW_SYNC_PIPE);
    raw_put(raw_tex_image(RF_RGBA, RS_32, blend_surf.stride / 4, PhysicalAddr(blend_surf.buffer)));
    raw_put(RAW_SYNC_TILE);
    raw_put(raw_set_tile(RF_RGBA, RS_32, CWF * 2, 0, 1, 0));   /* RGBA32: TMEM line = width*2 */
    raw_put(raw_set_tile(RF_RGBA, RS_32, CWF * 2, 0, 0, 0));
    raw_put(raw_set_tile_size(0, 0, 0, CWF, CHF));
    final_rects(rl, nr, CWF, CHF, 0, 0, 0x400);
    raw_flush();
    r_stats.px_2cyc += rects_area(rl, nr);
}

/* ---- exact model of the blend configuration (see gen_blend_table.py) ---- */

static inline int comb9(int a, int b, int c, int d) {
    int v = ((((a - b) * c + (d << 8) + 0x80) & 0x1FFFF) >> 8) & 0x1FF;
    return v < 256 ? v : (v < 384 ? 255 : 0);
}
static int cfg_pixel(const blend_cfg_t *b, int t) {
    int t8 = (t << 3) | (t >> 2);
    return b->comb == 0 ? comb9(t8, 0, b->C, 0) : comb9(t8, 0, b->C, t8);
}
static int cfg_blend(const blend_cfg_t *b, int p, int m) {
    int a1 = b->a1, mulb = (b->variant == 0 || b->variant == 2) ? 32 - a1 : 32;
    int s = b->variant < 2 ? p * a1 + m * mulb : m * a1 + p * mulb;
    return (s >> 5) & 0xFF;
}

/* TLUT final pass: map every intermediate value v (0..255) to its 5-bit
 * result F[v].  Plain pixels are v = 4c; blended values come from the config
 * arithmetic.  Returns the largest blended value. */
static uint8_t fin_F[256];
static uint8_t fin_amax[32];            /* largest F of a pixel showing a first target with channel <= a */
static const blend_cfg_t *fin_cfg;      /* configuration fin_F / fin_amax were built for */
static int fin_eva, fin_evb;
static int build_fin_map(const blend_cfg_t *b, int eva, int evb) {
    uint8_t set[256];
    memset(set, 0, sizeof set);
    for (int v = 0; v < 256; v++) fin_F[v] = (uint8_t)(v >> 2 > 31 ? 31 : v >> 2);
    for (int c = 0; c < 32; c++) set[4 * c] = 1;            /* plain colours win */
    int vmax = 0;
    for (int a = 0; a < 32; a++) {
        int p = cfg_pixel(b, b->tau[a]);
        for (int bb = 0; bb < 32; bb++) {
            int v = cfg_blend(b, p, 4 * bb);
            if (v > vmax) vmax = v;
            if (set[v]) continue;
            int t = (a * eva + bb * evb) >> 4;
            fin_F[v] = (uint8_t)(t > 31 ? 31 : t);
            set[v] = 1;
        }
    }
    return vmax;
}

/* Exact final pass for intermediates that may exceed the 2x pass range:
 * the intermediate is read as CI8 bytes (R, G, B = bytes 0, 1, 2 of a pixel)
 * through a TLUT holding F.  With bc the "blend channel":
 *   A (2-cycle): TEX0, TEX1 = the bytes of the two other channels -> writes
 *                their F, 0 in channel bc
 *   B (1-cycle): byte bc, the blender adds F (at most 30) into channel bc
 *   C (1-cycle): the blender adds 1 more where F = 31 (only if F can be 31
 *                in channel bc: npass = 3)
 * (the blender can add at most 247/8 to an empty channel: P*31/32 with P<=255) */
static void final_pass_tlut(const rrect_t *rl, int nr, int bc, int npass) {
    /* increments: smallest texel t whose T8(t)*31/32 lands in [8k, 8k+7] */
    static uint8_t tk[32];
    if (!tk[1]) {
        for (int k = 0; k < 31; k++)
            for (int t = 0; t < 32; t++) {
                int q = (((t << 3) | (t >> 2)) * 31) >> 5;
                if (q >= 8 * k && q <= 8 * k + 7) { tk[k] = (uint8_t)t; break; }
            }
    }
    static const uint8_t shift[3] = { 11, 6, 1 };   /* RGBA5551 position of R, G, B */
    int c0 = bc == 0 ? 1 : 0, c1 = bc == 2 ? 1 : 2;  /* the two looked-up channels */
    uint64_t *ta = (uint64_t *)fin_tlut, *tb = (uint64_t *)(fin_tlut + 256), *tc = (uint64_t *)(fin_tlut + 512);
    for (int v = 0; v < 256; v += 4) {
        uint64_t wa = 0, wb = 0, wc = 0;
        for (int k = 0; k < 4; k++) {
            uint32_t f = fin_F[v + k];
            uint32_t fb = f > 30 ? 30 : f;
            wa = (wa << 16) | (uint16_t)((f << 11) | (f << 6) | (f << 1) | 1);
            wb = (wb << 16) | (uint16_t)(f ? ((uint32_t)tk[fb] << shift[bc]) | 1 : 0);
            wc = (wc << 16) | (uint16_t)(f == 31 ? ((uint32_t)tk[1] << shift[bc]) | 1 : 0);
        }
        ta[v >> 2] = wa; tb[v >> 2] = wb; tc[v >> 2] = wc;
    }
    uint32_t img = PhysicalAddr(blend_surf.buffer);
    /* loaded as RGBA16 texels (an image is at most 1024 texels wide), drawn as CI8 bytes */
    int img_w = blend_surf.stride / 2;
    const int CW = 64, CH = 8;                      /* 64 pixels = 256 bytes x 8 rows = 2KB */
    uint32_t area = rects_area(rl, nr);
    for (int pass = 0; pass < npass; pass++) {
        raw_flush();
        rdpq_set_mode_standard();
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_dithering(DITHER_NONE_NONE);
        if (pass == 0) {
            rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, 0, PRIM, 0), (0, 0, 0, 1),
                                              (TEX1, 0, ENV, COMBINED), (0, 0, 0, 1)));
            rdpq_set_prim_color(RGBA32(c0 == 0 ? 255 : 0, c0 == 1 ? 255 : 0, 0, 255));
            rdpq_set_env_color(RGBA32(0, c1 == 1 ? 255 : 0, c1 == 2 ? 255 : 0, 255));
        } else {
            rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, TEX0), (0, 0, 0, TEX0)));
            rdpq_mode_alphacompare(1);
            rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, FOG_ALPHA, MEMORY_RGB, ONE)));
            rdpq_set_fog_color(RGBA32(0, 0, 0, 255));
        }
        rdpq_tex_upload_tlut(fin_tlut + pass * 256, 0, 256);
        /* texel s of the rectangle: S; TEX0 reads byte S - sl0, TEX1 byte S - sl1 */
        int S = pass == 0 ? 2 : bc;
        raw_reserve(8);
        raw_put(RAW_SYNC_PIPE);
        raw_put(raw_tex_image(RF_RGBA, RS_16, img_w, img));
        raw_put(RAW_SYNC_TILE);
        raw_put(raw_set_tile(RF_RGBA, RS_16, CW * 4, 0, 2, 0));       /* load tile */
        raw_put(raw_set_tile(RF_CI, RS_8, CW * 4, 0, 0, 0));
        raw_put(raw_set_tile(RF_CI, RS_8, CW * 4, 0, 1, 0));
        raw_put(raw_set_tile_size(0, pass == 0 ? 2 - c0 : 0, 0, CW * 4, CH));
        raw_put(raw_set_tile_size(1, pass == 0 ? 2 - c1 : 0, 0, CW * 4, CH));
        final_rects(rl, nr, CW, CH, 1, S, 0x1000);
        if (pass == 0) r_stats.px_2cyc += area; else r_stats.px_1cyc += area;
    }
    raw_flush();
    pipe_mode = PIPE_NONE;
    pipe_tlut = -1;
}

/* the largest intermediate value of configuration b for first-target
 * colours with channels <= amax over 2nd targets up to 31 */
static int cfg_vmax(const blend_cfg_t *b, int amax) {
    int m = 0;
    for (int a = 0; a <= amax; a++) {
        int v = cfg_blend(b, cfg_pixel(b, b->tau[a]), 4 * 31);
        if (v > m) m = v;
    }
    return m;
}

/* fin_F and fin_amax for configuration b (kept while it does not change) */
static void fin_prepare(const blend_cfg_t *b, int eva, int evb) {
    if (b == fin_cfg && eva == fin_eva && evb == fin_evb)
        return;
    build_fin_map(b, eva, evb);
    int m = 0;
    for (int a = 0; a < 32; a++) {
        int p = cfg_pixel(b, b->tau[a]);
        if (fin_F[4 * a] > m) m = fin_F[4 * a];
        for (int bb = 0; bb < 32; bb++) {
            int f = fin_F[cfg_blend(b, p, 4 * bb)];
            if (f > m) m = f;
        }
        fin_amax[a] = (uint8_t)m;
    }
    fin_cfg = b; fin_eva = eva; fin_evb = evb;
}

/* ------------------------------------------------------------ frame state */

/* kind: 0..2 BG kind-1, 3 OBJ, 4 the CPU-composited OBJ pixels of one hole
 * and code (priority | semi << 2 | hole << 3, render_obj.c obj_cpu_begin) */
typedef struct { uint8_t kind, id, fx, l2; } item_t;
#define MAX_ITEMS (4 + OBJ_CPU_MAX_ITEMS + 128)
static item_t items[MAX_ITEMS];         /* draw list in painter's order */
static int nitems, last_alpha, defer;
static int n32;                         /* items drawn by the 32-bit path: the rest is above every
                                         * first target and goes straight to the display */
/* the cells each item may cover: a bounding box of cells (end exclusive,
 * empty if i0 >= i1) and, for BG items, an exact mask (bg_touch[item_bg[k]])
 * (small, so that the masks stay in the 8KB data cache) */
typedef struct { int16_t i0, j0, i1, j1; } cbox_t;
static cbox_t item_box[MAX_ITEMS];
static int8_t item_bg[MAX_ITEMS];
static cmask_t bg_touch[4], bg_solid[4];
static cmask_t fr_cov;                  /* cells where the backdrop is hidden under the first targets */
static int fr_nalpha;                   /* first-target (FX_ALPHA) items */
static const cmask_t *fr_asolid;        /* with one: the cells it covers entirely (NULL: none) */
static int masks_ok;                    /* the region fits the cell masks */
static uint32_t fr_bd, fr_bd_bright, fr_dispcnt;
static int fr_bd_tag, fr_bd_defer, fr_mode, fr_obj_bright;

static inline uint64_t span_bits(int i0, int i1) {
    return (i1 - i0 >= 64 ? ~0ull : (1ull << (i1 - i0)) - 1) << i0;
}

/* the cells of row j that item k may cover */
static inline uint64_t item_row(int k, int j) {
    const cbox_t *b = &item_box[k];
    if (j < b->j0 || j >= b->j1)
        return 0;
    return item_bg[k] >= 0 ? bg_touch[item_bg[k]].r[j] : span_bits(b->i0, b->i1);
}

/* Cell masks of the frame (masks_ok): reg = cells where a first target may
 * have an opaque pixel; tg = cells of reg where a pixel under a first target
 * may belong to a layer that is not a 2nd target (or to the backdrop), i.e.
 * where the 2nd-target rule needs tags; need = cells of reg where a pixel
 * under a first target may belong to a 2nd target (outside it every first
 * target there lies over non-targets only and shows without its effect).
 * Walking the items bottom-up, s is "every pixel's top layer is a 2nd
 * target" and nt "no pixel's top layer is a 2nd target" per cell, from the
 * cells each layer may touch and the cells it covers entirely (text BG
 * tiles without a transparent pixel). */
/* the cells item k may cover (item_box, bg_touch) and, for text BGs, the
 * cells it covers entirely (bg_solid) */
static void item_masks(int k, int *nbg) {
    const item_t *it = &items[k];
    cbox_t *b = &item_box[k];
    *b = (cbox_t){ 0, 0, 0, 0 };
    item_bg[k] = -1;
    if (item_is_obj(it->kind)) {
        int x0, y0, x1, y1;
        if (it->kind == 3 ? obj_box(it->id, &x0, &y0, &x1, &y1) : obj_cpu_box(it->id, &x0, &y0, &x1, &y1)) {
            if (x0 < R.gx0) x0 = R.gx0;
            if (y0 < R.gy0) y0 = R.gy0;
            if (x1 > R.gx1) x1 = R.gx1;
            if (y1 > R.gy1) y1 = R.gy1;
            if (x0 < x1 && y0 < y1)
                *b = (cbox_t){ (int16_t)((x0 - R.gx0) >> 3), (int16_t)((y0 - R.gy0) >> 3),
                               (int16_t)(((x1 - 1 - R.gx0) >> 3) + 1), (int16_t)(((y1 - 1 - R.gy0) >> 3) + 1) };
        }
        return;
    }
    cmask_t *t = &bg_touch[*nbg], *so = &bg_solid[*nbg];
    item_bg[k] = (int8_t)(*nbg)++;
    for (int j = 0; j < cm_h; j++) t->r[j] = so->r[j] = 0;
    if (it->kind == 0)
        bg_text_cells(it->id, t, so);
    else
        for (int j = 0; j < cm_h; j++) t->r[j] = (1ull << cm_w) - 1;
    uint64_t any = 0;
    int j0 = cm_h, j1 = 0;
    for (int j = 0; j < cm_h; j++)
        if (t->r[j]) { any |= t->r[j]; if (j < j0) j0 = j; j1 = j + 1; }
    if (any) {
        int i0 = 0, i1 = 64;
        while (!((any >> i0) & 1)) i0++;
        while (!((any >> (i1 - 1)) & 1)) i1--;
        *b = (cbox_t){ (int16_t)i0, (int16_t)j0, (int16_t)i1, (int16_t)j1 };
    }
}

static void compute_masks(uint32_t l2, cmask_t *reg, cmask_t *tg, cmask_t *need) {
    uint64_t full = (1ull << cm_w) - 1;
    cmask_t s, nt, seen;
    int nbg = 0;
    fr_nalpha = 0;
    fr_asolid = NULL;
    /* the first targets first: without a cell of theirs nothing else is needed */
    uint64_t any = 0;
    for (int j = 0; j < cm_h; j++) reg->r[j] = tg->r[j] = need->r[j] = 0;
    for (int k = 0; k < nitems; k++) {
        if (items[k].fx != FX_ALPHA) continue;
        item_masks(k, &nbg);
        if (fr_nalpha++ == 0)
            fr_asolid = item_bg[k] >= 0 ? &bg_solid[item_bg[k]] : NULL;
        for (int j = item_box[k].j0; j < item_box[k].j1; j++) {
            reg->r[j] |= item_row(k, j);
            any |= reg->r[j];
        }
    }
    if (!any)
        return;
    for (int k = 0; k < nitems; k++)
        if (items[k].fx != FX_ALPHA) item_masks(k, &nbg);
    for (int j = 0; j < cm_h; j++) {
        seen.r[j] = fr_cov.r[j] = 0;
        s.r[j] = ((l2 >> 5) & 1) ? full : 0;
        nt.r[j] = ((l2 >> 5) & 1) ? 0 : full;
    }
    for (int k = 0; k < nitems; k++) {
        const item_t *it = &items[k];
        const cmask_t *so = item_bg[k] >= 0 ? &bg_solid[item_bg[k]] : NULL;
        int alpha = it->fx == FX_ALPHA;
        /* (an OBJ only matters in its box: it covers no cell entirely) */
        int ja = so ? 0 : item_box[k].j0, jb = so ? cm_h : item_box[k].j1;
        for (int j = ja; j < jb; j++) {
            uint64_t tj = item_row(k, j), sj = so ? so->r[j] : 0;
            if (alpha) {
                tg->r[j] |= tj & ~s.r[j];
                need->r[j] |= tj & ~nt.r[j];
                seen.r[j] |= tj;
            }
            fr_cov.r[j] |= sj & ~seen.r[j];
            if (it->l2) {
                s.r[j] |= sj;
                nt.r[j] &= ~tj;
            } else {
                s.r[j] &= ~tj;
                nt.r[j] |= sj;
            }
        }
    }
}

/* Rectangles covering the set cells of m and no cell of 'avoid': runs of each
 * cell row (a one-cell gap is bridged unless it is in 'avoid'), stacked into
 * taller rectangles where consecutive rows have the same run.  -1 if there
 * would be more than 'max'. */
static int mask_rects(const cmask_t *m, const cmask_t *avoid, rrect_t *out, int max) {
    int n = 0;
    for (int j = 0; j < cm_h; j++) {
        uint64_t b = m->r[j];
        b |= ~b & (b << 1) & (b >> 1) & ~avoid->r[j];
        while (b) {
            int i0 = r_ctz64(b);
            uint64_t x = ~(b >> i0);
            int len = x ? r_ctz64(x) : 64 - i0, i1 = i0 + len;
            b &= ~((len >= 64 ? ~0ull : (1ull << len) - 1) << i0);
            int k;
            for (k = 0; k < n; k++)
                if (out[k].j1 == j && out[k].i0 == i0 && out[k].i1 == i1) break;
            if (k < n) {
                out[k].j1 = (int16_t)(j + 1);
                continue;
            }
            if (n == max) return -1;
            out[n++] = (rrect_t){ (int16_t)i0, (int16_t)j, (int16_t)i1, (int16_t)(j + 1) };
        }
    }
    return n;
}

static inline uint64_t rect_bits(const rrect_t *r) {
    int w = r->i1 - r->i0;
    return (w >= 64 ? ~0ull : (1ull << w) - 1) << r->i0;
}

/* Coarse rectangles: in each band of 4 cell rows (32 pixels), one rectangle
 * from the first to the last set cell of the rows of m, split where a row
 * of 'other' (whose rows m does not share) comes between. */
static int band_rects(const cmask_t *m, const cmask_t *other, rrect_t *out) {
    int n = 0;
    for (int g = 0; g < cm_h; g += 4) {
        int ge = g + 4 < cm_h ? g + 4 : cm_h;
        for (int j = g; j < ge;) {
            if (!m->r[j]) { j++; continue; }
            int ja = j, jb = j;
            uint64_t u = 0;
            for (; j < ge && !other->r[j]; j++)
                if (m->r[j]) { u |= m->r[j]; jb = j + 1; }
            int i0 = r_ctz64(u), i1 = 64;
            while (!((u >> (i1 - 1)) & 1)) i1--;
            out[n++] = (rrect_t){ (int16_t)i0, (int16_t)ja, (int16_t)i1, (int16_t)jb };
        }
    }
    return n;
}

/* may item k cover a cell of rectangle r? */
static int item_hits(int k, const rrect_t *r) {
    const cbox_t *b = &item_box[k];
    if (b->i1 <= r->i0 || b->i0 >= r->i1 || b->j1 <= r->j0 || b->j0 >= r->j1)
        return 0;
    if (item_bg[k] < 0)
        return 1;
    uint64_t m = rect_bits(r);
    int j0 = r->j0 > b->j0 ? r->j0 : b->j0, j1 = r->j1 < b->j1 ? r->j1 : b->j1;
    for (int j = j0; j < j1; j++)
        if (bg_touch[item_bg[k]].r[j] & m) return 1;
    return 0;
}

static int mask_covers(const cmask_t *m, const rrect_t *r) {
    uint64_t b = rect_bits(r);
    for (int j = r->j0; j < r->j1; j++)
        if ((m->r[j] & b) != b) return 0;
    return 1;
}

/* draw item k with effect fx in every rectangle it may touch */
static void draw_rects(int k, int fx, const rrect_t *rl, int nr) {
    const item_t *it = &items[k];
    for (int q = 0; q < nr; q++) {
        if (masks_ok && !item_hits(k, &rl[q])) continue;
        int x0, y0, x1, y1;
        rect_px(&rl[q], &x0, &y0, &x1, &y1);
        set_window(x0, y0, x1, y1);
        draw_item(it->kind, it->id, fx, fr_mode, fr_dispcnt);
    }
}

/* Plain pass, a first target in the cell rows [j0, j1) outside 'cover' where
 * both kinds of its cells occur: drawn with fxp where no 2nd target can lie
 * under it (not in 'need') and with fxf (the EVB = 0 fade) elsewhere, one
 * draw window per rectangle (a rectangle of one kind never covers a cell of
 * the other; cells of 'cover' are redrawn by the final pass anyway). */
static void draw_cells(const item_t *it, int fx, const rrect_t *rl, int n) {
    for (int q = 0; q < n; q++) {
        int x0, y0, x1, y1;
        rect_px(&rl[q], &x0, &y0, &x1, &y1);
        set_window(x0, y0, x1, y1);
        draw_item(it->kind, it->id, fx, fr_mode, fr_dispcnt);
    }
}

static void draw_split(int k, const cmask_t *cover, const cmask_t *need, int j0, int j1, int fxp, int fxf) {
    const item_t *it = &items[k];
    static cmask_t m[2], one;
    static rrect_t rl[MAX_RECTS];
    for (int j = 0; j < cm_h; j++) {
        uint64_t o = j >= j0 && j < j1 ? item_row(k, j) & ~cover->r[j] : 0;
        m[0].r[j] = o & ~need->r[j];
        m[1].r[j] = o & need->r[j];
    }
    for (int s = 0; s < 2; s++) {
        int fx = s ? fxf : fxp;
        int n = mask_rects(&m[s], &m[s ^ 1], rl, MAX_RECTS);
        if (n >= 0) {
            draw_cells(it, fx, rl, n);
            continue;
        }
        /* too fragmented: one cell row at a time (at most 32 runs each) */
        for (int j = j0; j < j1; j++) {
            for (int q = 0; q < cm_h; q++) one.r[q] = q == j ? m[s].r[q] : 0;
            n = mask_rects(&one, &m[s ^ 1], rl, MAX_RECTS);
            if (n > 0) draw_cells(it, fx, rl, n);
        }
    }
    set_window(R.gx0, R.gy0 + 8 * j0, R.gx1, R.gy0 + 8 * j1);
}

static inline color_t c4(uint32_t c) {
    return RGBA32((c & 31) * 4, ((c >> 5) & 31) * 4, ((c >> 10) & 31) * 4, 255);
}

/* The 32-bit path over the rectangles rl (target attached, R.is32): the
 * backdrop, then every item in painter's order; with tg, under the tag
 * scheme (see the top of this file). */
static void pass32(const rrect_t *rl, int nr, int tg, uint64_t *t_bg, uint64_t *t_obj) {
    raw_flush();
    if (tg) {
        /* backdrop colour and its tag in one 1-cycle fill */
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_set_prim_color(c4(fr_bd));
        rdpq_mode_zoverride(true, 0, 0);
        rdpq_set_prim_depth_raw((uint16_t)fr_bd_tag, 0);
        rdpq_mode_zbuf(false, true);
        for (int q = 0; q < nr; q++) {
            int x0, y0, x1, y1;
            rect_px(&rl[q], &x0, &y0, &x1, &y1);
            rdpq_fill_rectangle(x0 + R.tx, y0 + R.ty, x1 + R.tx, y1 + R.ty);
            r_stats.px_1cyc += (uint32_t)((x1 - x0) * (y1 - y0));
        }
    } else {
        rdpq_set_mode_fill(c4(fr_bd));
        for (int q = 0; q < nr; q++) {
            if (masks_ok && mask_covers(&fr_cov, &rl[q])) continue;
            int x0, y0, x1, y1;
            rect_px(&rl[q], &x0, &y0, &x1, &y1);
            fill_g(x0, y0, x1, y1);
        }
    }
    pipe_mode = PIPE_NONE;
    pipe_z = -1;
    z_state = ZT_NONE; z_tag = 0;
    cmask_t cells;
    if (masks_ok) {
        for (int j = 0; j < cm_h; j++) cells.r[j] = 0;
        for (int q = 0; q < nr; q++)
            for (int j = rl[q].j0; j < rl[q].j1; j++) cells.r[j] |= rect_bits(&rl[q]);
    }
    for (int k = 0; k < n32; k++) {
        const item_t *it = &items[k];
        if (masks_ok) {
            int hit = 0;
            for (int j = item_box[k].j0; j < item_box[k].j1 && !hit; j++) hit = (item_row(k, j) & cells.r[j]) != 0;
            if (!hit) continue;
        }
        uint64_t ta = get_ticks();
        int tag = it->l2 ? TAG_TARGET : TAG_NONTARGET;
        if (!tg) {
            draw_rects(k, it->fx, rl, nr);
        } else if (it->fx != FX_ALPHA) {
            int dfr = defer && it->fx == FX_BRIGHT && it->l2;   /* brightened at the end */
            z_state = ZT_WRITE; z_tag = dfr ? TAG_DEFERRED : tag;
            draw_rects(k, dfr ? FX_PLAIN : it->fx, rl, nr);
        } else {
            /* first target: blend where the tag under it says 2nd target ... */
            z_state = ZT_DECAL; z_tag = TAG_TARGET;
            draw_rects(k, FX_ALPHA, rl, nr);
            if (defer) {                        /* (raw deferred 2nd targets too) */
                z_state = ZT_DECAL; z_tag = TAG_DEFERRED;
                draw_rects(k, FX_ALPHA, rl, nr);
            }
            /* ... draw it unblended where it does not (a semi-transparent OBJ
             * keeps the OBJ layer's brighten/darken there) ... */
            int nfx = item_obj_semi(it->kind, it->id) && fr_obj_bright ? FX_BRIGHT : FX_PLAIN;
            z_state = ZT_DECAL; z_tag = TAG_NONTARGET;
            draw_rects(k, nfx, rl, nr);
            /* ... and leave its own tag for first targets drawn later (and
             * for the deferred brighten, which must not touch it) */
            if (k < last_alpha || defer) {
                z_state = ZT_KEEP; z_tag = tag;
                draw_rects(k, FX_PLAIN, rl, nr);
            }
        }
        if (item_is_obj(it->kind)) *t_obj += get_ticks() - ta; else *t_bg += get_ticks() - ta;
    }
    if (tg && defer) {
        /* deferred brighten/darken where a deferred layer is still on top: the
         * backdrop, then the deferred layers in painter's order (at a pixel
         * the last opaque one is the top layer) */
        uint64_t ta = get_ticks();
        if (fr_bd_defer) {
            raw_flush();
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
            rdpq_set_prim_color(c4(fr_bd_bright));
            rdpq_mode_zoverride(true, 0, 0);
            rdpq_set_prim_depth_raw(TAG_DEFERRED, 0);
            rdpq_mode_zbuf(true, false);
            rdpq_mode_zmode(ZMODE_DECAL);
            for (int q = 0; q < nr; q++) {
                int x0, y0, x1, y1;
                rect_px(&rl[q], &x0, &y0, &x1, &y1);
                rdpq_fill_rectangle(x0 + R.tx, y0 + R.ty, x1 + R.tx, y1 + R.ty);
                r_stats.px_1cyc += (uint32_t)((x1 - x0) * (y1 - y0));
            }
            pipe_mode = PIPE_NONE;
            pipe_z = -1;
        }
        z_state = ZT_DECAL; z_tag = TAG_DEFERRED;
        for (int k = 0; k < nitems; k++)
            if (items[k].fx == FX_BRIGHT && items[k].l2)
                draw_rects(k, FX_BRIGHT, rl, nr);
        *t_bg += get_ticks() - ta;
    }
    z_state = ZT_NONE;
    set_window(R.gx0, R.gy0, R.gx1, R.gy1);
}

int gba_render_busy(void) {
    return have_sp && !rspq_syncpoint_check(last_sp);
}

void gba_render_frame(surface_t *disp, const gba_view_t *view) {
    uint64_t t_start = get_ticks();
    memset(&r_stats, 0, sizeof r_stats);
    if (have_sp)
        rspq_syncpoint_wait(last_sp);
    r_stats.us_wait = us_since(t_start);

    r_frame++;
    r_view = view;
    scratch_used = 0;
    raw_frame_begin();
    pipe_mode = PIPE_NONE;
    pipe_tlut = -1;
    pipe_z = -1;
    clip_reset();

    /* ---- dirty tracking ---- */
    uint64_t t0 = get_ticks();
    uint32_t dirty = gba_vram_take_dirty() & 0xFFFFFFu;
    /* the 32-byte units writers noted precisely (pages they dirtied as a
     * whole are in 'dirty') */
    static uint32_t units[96];
    int have_units = gba_vram_take_units(units);
    if (force_all) {
        dirty = 0xFFFFFFu;
        bg_invalidate();
        obj_invalidate();
        force_all = 0;
    }
    r_stats.dirty_pages = (uint32_t)__builtin_popcount(dirty);
    r_stats.dirty_mask = dirty;
    bg_track(dirty & 0xFFFFu, have_units ? units : NULL);
    obj_track(dirty >> 16, have_units ? units + 64 : NULL);
    r_stats.us_track = us_since(t0);

    /* ---- region ---- */
    int W = disp->width, H = disp->height;
    R.tx = view->ox; R.ty = view->oy;
    R.gx0 = view->x0; R.gy0 = view->y0; R.gx1 = view->x1; R.gy1 = view->y1;
    if (R.gx0 + R.tx < 0) R.gx0 = -R.tx;
    if (R.gy0 + R.ty < 0) R.gy0 = -R.ty;
    if (R.gx1 + R.tx > W) R.gx1 = W - R.tx;
    if (R.gy1 + R.ty > H) R.gy1 = H - R.ty;
    if (R.gx1 < R.gx0) R.gx1 = R.gx0;
    if (R.gy1 < R.gy0) R.gy1 = R.gy0;
    R.is32 = 0;
    set_window(R.gx0, R.gy0, R.gx1, R.gy1);
    cm_w = (R.gx1 - R.gx0 + 7) >> 3;
    cm_h = (R.gy1 - R.gy0 + 7) >> 3;
    masks_ok = cm_w <= 63 && cm_h <= CM_ROWS;
    int fx0 = R.gx0 + R.tx, fy0 = R.gy0 + R.ty, fx1 = R.gx1 + R.tx, fy1 = R.gy1 + R.ty;

    /* ---- border ---- */
    rdpq_set_scissor(0, 0, W, H);
    rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
    if (fy0 > 0) rdpq_fill_rectangle(0, 0, W, fy0);
    if (fy1 < H) rdpq_fill_rectangle(0, fy1, W, H);
    if (fx0 > 0) rdpq_fill_rectangle(0, fy0, fx0, fy1);
    if (fx1 < W) rdpq_fill_rectangle(fx1, fy0, W, fy1);
    r_stats.px_fill += (uint32_t)(W * H - (fx1 - fx0) * (fy1 - fy0));
    if (fx1 <= fx0 || fy1 <= fy0)
        goto done;

    uint32_t dispcnt = io16(REG_DISPCNT);
    if (dispcnt & 0x80) {                       /* forced blank: white */
        rdpq_set_mode_fill(RGBA32(255, 255, 255, 255));
        fill_g(R.gx0, R.gy0, R.gx1, R.gy1);
        goto done;
    }
    int mode = (int)(dispcnt & 7);

    /* ---- colour effects ---- */
    uint32_t bldcnt = io16(REG_BLDCNT) & 0x3FFF, bldalpha = io16(REG_BLDALPHA);
    uint32_t eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = io16(REG_BLDY) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    uint32_t effect = (bldcnt >> 6) & 3;
    uint32_t l1 = bldcnt & 0x3F, l2 = (bldcnt >> 8) & 0x3F;
    /* alpha: identity (16/0) or no possible 2nd target -> nothing to do */
    int alpha_live = l2 != 0 && !(eva == 16 && evb == 0);
    alpha_needs_blend = alpha_live && evb > 0;
    bcfg = NULL;
    for (int i = 0; i < 32; i++) {
        if (effect == 2) tau_bright[i] = (uint8_t)(i + (((31 - i) * evy) >> 4));
        else if (effect == 3) tau_bright[i] = (uint8_t)(i - ((i * evy) >> 4));
        else tau_bright[i] = (uint8_t)i;
    }
    int tlut_final = 0;
    if (alpha_needs_blend) {
        /* prefer the cheap 2x final pass when its configuration is exact and
         * cannot exceed its range; else an exact configuration for the TLUT
         * final pass; else the best approximation (TLUT pass if it may wrap) */
        const blend_cfg_t *c2 = &blend_table[eva][evb - 1], *ct = &blend_table_t[eva][evb - 1];
        if (c2->wrong == 0 && c2->vmax <= 191) {
            bcfg = c2;
        } else if (ct->valid) {
            bcfg = ct;
            tlut_final = 1;
        } else {
            bcfg = c2;
            tlut_final = c2->vmax > 191;
        }
        memcpy(tau_alpha, bcfg->tau, 32);
        r_stats.blend_wrong = bcfg->wrong;
    } else {
        for (int i = 0; i < 32; i++)
            tau_alpha[i] = (uint8_t)((i * eva) >> 4);
    }
    /* fx per layer: BG0..3, OBJ (non-semi), backdrop */
    int lfx[6];
    for (int l = 0; l < 6; l++) {
        lfx[l] = FX_PLAIN;
        if (!((l1 >> l) & 1)) continue;
        if (effect == 1 && alpha_live && l != 5) lfx[l] = FX_ALPHA;
        else if (effect >= 2) lfx[l] = FX_BRIGHT;
    }
    /* semi-transparent OBJs blend with any 2nd target whatever the effect; over
     * anything else they get the OBJ layer's own effect.  An identity blend
     * (16/0) still differs from a brightened/darkened OBJ, so it stays an
     * alpha item (identity TLUT) and the 2nd-target tags tell the two apart. */
    int semi_fx = (alpha_live || (l2 != 0 && lfx[4] == FX_BRIGHT)) ? FX_ALPHA : lfx[4];

    /* ---- OBJ list ---- */
    uint8_t olist[4][128];
    int ocount[4] = { 0, 0, 0, 0 };
    uint8_t ocodes[OBJ_CPU_MAX_ITEMS];
    int ncodes = 0;
    /* BG priorities, and the set of them among the displayed BGs */
    int bgprio[4];
    uint32_t bgpm = 0;
    for (int n = 0; n < 4; n++) {
        bgprio[n] = (int)(io16(REG_BG0CNT + 2 * n) & 3);
        if (bg_kind[mode][n] && (dispcnt & (0x100u << n)))
            bgpm |= 1u << bgprio[n];
    }
    if (dispcnt & 0x1000) {
        uint64_t tp = get_ticks();
        obj_parse(dispcnt, olist, ocount);
        /* OBJs drawn as alpha items blend with (or, by the 2nd-target
         * rule, check) what is under them: under the GBA's OBJ layer that is
         * a BG, never another OBJ */
        ncodes = obj_cpu_begin(olist, ocount,
                               lfx[4] == FX_ALPHA ? OBJ_CPU_BLEND_ALL :
                               semi_fx == FX_ALPHA ? OBJ_CPU_BLEND_SEMI : OBJ_CPU_BLEND_NONE, bgpm, ocodes);
        uint64_t tc = get_ticks();
        r_stats.us_obj_parse = (uint32_t)TICKS_TO_US(tc - tp);
        obj_prepare(olist, ocount);             /* OBJ shadow: part of us_track */
        r_stats.us_track += (uint32_t)TICKS_TO_US(get_ticks() - tc);
    }

    /* ---- draw list in painter's order: priority 3..0, BGs (3..0), then the
     * CPU-composited OBJ pixels of that priority, then OBJs (127..0) ---- */
    nitems = 0;
    for (int p = 3; p >= 0; p--) {
        for (int n = 3; n >= 0; n--) {
            if (!bg_kind[mode][n] || !(dispcnt & (0x100u << n)) || bgprio[n] != p) continue;
            items[nitems++] = (item_t){ (uint8_t)(bg_kind[mode][n] - 1), (uint8_t)n, (uint8_t)lfx[n],
                                        (uint8_t)((l2 >> n) & 1) };
        }
        for (int k = 0; k < ncodes; k++) {
            if ((ocodes[k] & 3) != p) continue;
            items[nitems++] = (item_t){ 4, ocodes[k], (uint8_t)((ocodes[k] & 4) ? semi_fx : lfx[4]),
                                        (uint8_t)((l2 >> 4) & 1) };
        }
        for (int k = 0; k < ocount[p]; k++) {
            int i = olist[p][k];
            items[nitems++] = (item_t){ 3, (uint8_t)i, (uint8_t)(obj_info(i)->semi ? semi_fx : lfx[4]),
                                        (uint8_t)((l2 >> 4) & 1) };
        }
    }
    /* Alpha blending needs the 32-bit path; the 2nd-target rule needs tags when a
     * layer that is not a 2nd target (or the backdrop) may lie under a first target. */
    int blend32 = 0, tags = 0;
    last_alpha = -1;
    {
        int nontarget_below = !((l2 >> 5) & 1);
        for (int k = 0; k < nitems; k++) {
            if (items[k].fx == FX_ALPHA) {
                last_alpha = k;
                if (alpha_needs_blend) blend32 = 1;
                if (nontarget_below) tags = 1;
            }
            if (!items[k].l2) nontarget_below = 1;
        }
        if (tags) blend32 = 1;
    }
    /* Brighten/darken with semi-transparent OBJs: such an OBJ blends with the
     * raw colour of the 2nd target beneath it, so a layer that is both a 1st
     * target (brightened) and a 2nd target is drawn raw ("deferred", tag
     * TAG_DEFERRED) when a semi-transparent OBJ may lie over it, and is
     * brightened at the end where it is still the top layer.  With effect 2/3
     * the only FX_ALPHA items are semi-transparent OBJs. */
    defer = 0;
    int bd_defer = 0;
    if (effect >= 2) {
        int below = lfx[5] == FX_BRIGHT && ((l2 >> 5) & 1);
        for (int k = 0; k < nitems; k++) {
            if (items[k].fx == FX_ALPHA && below) defer = 1;
            if (items[k].fx == FX_BRIGHT && items[k].l2) below = 1;
        }
        if (defer) {
            tags = blend32 = 1;
            bd_defer = lfx[5] == FX_BRIGHT && ((l2 >> 5) & 1);
        }
    }
    /* Items above the last first target are not blended and nothing above
     * them is: the 32-bit path stops there and they are drawn on the display
     * after the final pass (not with deferred brightening, which needs every
     * deferred layer in the 32-bit path). */
    n32 = defer ? nitems : last_alpha + 1;

    /* ---- backdrop ---- */
    uint32_t bd = r_pal16(0), bd_bright = bd;
    if (lfx[5] == FX_BRIGHT) {
        const uint8_t *t = tau_bright;
        bd_bright = t[bd & 31] | (t[(bd >> 5) & 31] << 5) | (t[(bd >> 10) & 31] << 10);
    }
    fr_bd = bd_defer ? bd : bd_bright;      /* backdrop colour of the 32-bit path */
    fr_bd_bright = bd_bright;
    fr_bd_tag = bd_defer ? TAG_DEFERRED : ((l2 >> 5) & 1) ? TAG_TARGET : TAG_NONTARGET;
    fr_bd_defer = bd_defer;
    fr_mode = mode; fr_dispcnt = dispcnt; fr_obj_bright = lfx[4] == FX_BRIGHT;

    /* ---- where the 32-bit path is needed ----
     * Only cells where a first target (or semi-transparent OBJ) may have an
     * opaque pixel over a 2nd target go through it (with EVB = 0 only the
     * cells that need tags: elsewhere the plain pass applies the fade);
     * everything else is drawn by the plain pass straight into the display,
     * where a first target is drawn without its effect in the cells where
     * only non-targets can lie under it (and has no pixel elsewhere).  Tags
     * are written only in the cells where a layer that is not a 2nd target
     * may lie under a first target. */
    static rrect_t rl_n[MAX_RECTS], rl_t[MAX_RECTS], rl_all[2 * MAX_RECTS];
    int nrn = 0, nrt = 0, skip_alpha = 0, amask = 0;
    /* reg: cells of the 32-bit path; need: cells where a first target may be
     * drawn with its effect (amask: the masks were computed) */
    cmask_t reg, need;
    uint64_t full_row = cm_w >= 64 ? ~0ull : (1ull << cm_w) - 1;
    if (blend32) {
        uint64_t tm = get_ticks();
        skip_alpha = alpha_needs_blend;
        if (masks_ok) {
            cmask_t T, apass;
            compute_masks(l2, &reg, &T, &need);
            amask = 1;
            for (int j = 0; j < cm_h; j++) {
                uint64_t a = reg.r[j];
                if (defer) {
                    /* deferred brighten: all of it tagged */
                    T.r[j] = need.r[j] = a;
                } else if (!alpha_needs_blend) {
                    /* EVB = 0: the plain pass does the fade (or no effect),
                     * the 32-bit path only the cells where both can occur */
                    reg.r[j] = T.r[j] = T.r[j] & need.r[j];
                } else {
                    reg.r[j] = need.r[j];
                    T.r[j] &= need.r[j];
                }
                /* first-target cells left to the plain pass: an untagged
                 * 32-bit rectangle must not cover them (it would apply the
                 * effect over a non-target) */
                apass.r[j] = a & ~reg.r[j];
            }
            cmask_t mn, mt, av0;
            for (int j = 0; j < cm_h; j++) {
                mt.r[j] = reg.r[j] & T.r[j];
                mn.r[j] = reg.r[j] & ~T.r[j];
                av0.r[j] = mt.r[j] | apass.r[j];
            }
            /* the two sets of rectangles never overlap (a cell drawn by both
             * passes would be blended twice) */
            nrn = mask_rects(&mn, &av0, rl_n, MAX_RECTS);
            nrt = nrn < 0 ? -1 : mask_rects(&mt, &mn, rl_t, MAX_RECTS);
            if (nrt >= 0 && nrt <= 4 && nrn > 8) {
                /* many tagless rectangles (every rectangle costs a draw of
                 * each item it meets): one per band of 4 cell rows where its
                 * box meets no tagged rectangle, the exact ones elsewhere */
                cmask_t av, band;
                for (int j = 0; j < cm_h; j++) av.r[j] = apass.r[j];
                for (int q = 0; q < nrt; q++)
                    for (int j = rl_t[q].j0; j < rl_t[q].j1; j++) av.r[j] |= rect_bits(&rl_t[q]);
                nrn = 0;
                for (int g = 0; g < cm_h && nrn >= 0; g += 4) {
                    int ge = g + 4 < cm_h ? g + 4 : cm_h, ja = ge, jb = g;
                    uint64_t u = 0;
                    for (int j = g; j < ge; j++)
                        if (mn.r[j]) { u |= mn.r[j]; if (j < ja) ja = j; jb = j + 1; }
                    if (!u) continue;
                    int i0 = r_ctz64(u), i1 = 64;
                    while (!((u >> (i1 - 1)) & 1)) i1--;
                    rrect_t b = { (int16_t)i0, (int16_t)ja, (int16_t)i1, (int16_t)jb };
                    uint64_t bm = rect_bits(&b), hit = 0;
                    for (int j = ja; j < jb; j++) hit |= av.r[j] & bm;
                    if (!hit) { rl_n[nrn++] = b; continue; }
                    for (int j = 0; j < cm_h; j++) band.r[j] = j >= g && j < ge ? mn.r[j] : 0;
                    int k = mask_rects(&band, &av, rl_n + nrn, MAX_RECTS - nrn);
                    nrn = k < 0 ? -1 : nrn + k;
                }
            }
            if (nrn < 0 || nrt < 0 || nrn + nrt > 16) {
                /* still too fragmented: a cell row with a tagged cell is
                 * tagged entirely, and each band of 4 rows is one rectangle
                 * per set */
                for (int j = 0; j < cm_h; j++)
                    if (mt.r[j]) { mt.r[j] = reg.r[j]; mn.r[j] = 0; }
                nrn = band_rects(&mn, &mt, rl_n);
                /* an untagged rectangle over a cell left to the plain pass:
                 * its rows are tagged too (the other untagged rectangles of
                 * the band stay as they are: they end at tagged rows) */
                int moved = 0;
                for (int q = 0; q < nrn; q++) {
                    uint64_t b = rect_bits(&rl_n[q]), hit = 0;
                    for (int j = rl_n[q].j0; j < rl_n[q].j1; j++) hit |= apass.r[j] & b;
                    if (!hit) continue;
                    for (int j = rl_n[q].j0; j < rl_n[q].j1; j++)
                        if (mn.r[j]) { mt.r[j] = reg.r[j]; mn.r[j] = 0; }
                    moved = 1;
                }
                if (moved) nrn = band_rects(&mn, &mt, rl_n);
                nrt = band_rects(&mt, &mn, rl_t);
            }

        } else {
            /* region too large for the cell masks: all of it, as before */
            rrect_t all = { 0, 0, (int16_t)cm_w, (int16_t)cm_h };
            if (tags) { rl_t[0] = all; nrt = 1; } else { rl_n[0] = all; nrn = 1; }
            for (int j = 0; j < CM_ROWS; j++) reg.r[j] = ~0ull;
        }
        r_stats.us_track += us_since(tm);
        if (!nrn && !nrt)
            blend32 = 0;
    }
    tags = nrt > 0;
    /* cells the 32-bit path draws: the plain pass leaves out items entirely inside them */
    cmask_t cover;
    if (amask) {
        for (int j = 0; j < cm_h; j++) cover.r[j] = 0;
        for (int q = 0; q < nrn; q++)
            for (int j = rl_n[q].j0; j < rl_n[q].j1; j++) cover.r[j] |= rect_bits(&rl_n[q]);
        for (int q = 0; q < nrt; q++)
            for (int j = rl_t[q].j0; j < rl_t[q].j1; j++) cover.r[j] |= rect_bits(&rl_t[q]);
    }
    /* The final pass for the colours on screen: the 2x pass when the exact 2x
     * configuration stays in its range for the first-target colours in view;
     * else the TLUT pass, with two passes instead of three when every blend
     * pixel shows the one first target and a channel cannot reach 31. */
    int fin_bc = 2, fin_npass = 3;
    if (blend32 && tlut_final && alpha_needs_blend) {
        uint32_t am[3] = { 0, 0, 0 };
        for (int k = 0; k < nitems; k++) {
            if (items[k].fx != FX_ALPHA) continue;
            uint32_t c = items[k].kind == 0 ? bg_text_colour_max(items[k].id) :
                         items[k].kind == 3 ? obj_colour_max(items[k].id) :
                         items[k].kind == 4 ? obj_cpu_colour_max(items[k].id) : 0x7FFFu;
            for (int ch = 0; ch < 3; ch++, c >>= 5)
                if ((c & 31) > am[ch]) am[ch] = c & 31;
        }
        int amx = (int)(am[0] > am[1] ? (am[0] > am[2] ? am[0] : am[2]) : (am[1] > am[2] ? am[1] : am[2]));
        const blend_cfg_t *c2 = &blend_table[eva][evb - 1];
        if (c2->wrong == 0 && cfg_vmax(c2, amx) <= 191) {
            bcfg = c2;
            tlut_final = 0;
            memcpy(tau_alpha, bcfg->tau, 32);
            tlut_built[FX_ALPHA][PAL_BG] = tlut_built[FX_ALPHA][PAL_OBJ] = 0;
            r_stats.blend_wrong = 0;
        } else if (fr_nalpha == 1 && masks_ok) {
            int all = 1;
            if (!fr_asolid) all = 0;
            for (int q = 0; q < nrn && all; q++) all = mask_covers(fr_asolid, &rl_n[q]);
            for (int q = 0; q < nrt && all; q++) all = mask_covers(fr_asolid, &rl_t[q]);
            if (all) {
                fin_prepare(bcfg, (int)eva, (int)evb);
                for (int ch = 2; ch >= 0; ch--)
                    if (fin_amax[am[ch]] <= 30) { fin_bc = ch; fin_npass = 2; break; }
            }
        }
    }
    r_stats.blend_path = (uint32_t)blend32;
    r_stats.tags = (uint32_t)tags;
    r_stats.blend_px = rects_area(rl_n, nrn) + rects_area(rl_t, nrt);
    r_stats.tag_px = rects_area(rl_t, nrt);
    r_stats.blend_rects = (uint32_t)(nrn + nrt);

    /* ---- plain pass: the display, rows not entirely drawn by the 32-bit path ---- */
    uint64_t t_bg = 0, t_obj = 0;
    z_state = ZT_NONE; z_tag = 0;
    for (int j = 0; j < cm_h;) {
        int in32 = blend32 && masks_ok && reg.r[j] == full_row;
        if (blend32 && !masks_ok) in32 = 1;
        if (in32) { j++; continue; }
        int j1 = j + 1;
        while (j1 < cm_h && !(blend32 && masks_ok && reg.r[j1] == full_row)) j1++;
        set_window(R.gx0, R.gy0 + 8 * j, R.gx1, R.gy0 + 8 * j1);
        raw_flush();
        rdpq_set_mode_fill(color_from_packed16(r_conv(bd_bright)));
        pipe_mode = PIPE_NONE;
        fill_g(R.wx0, R.wy0, R.wx1, R.wy1);
        for (int k = 0; k < nitems; k++) {
            const item_t *it = &items[k];
            if (blend32 && k >= n32) continue;  /* drawn once over everything at the end */
            int fx = it->fx, split = 0;
            if (fx == FX_ALPHA && amask) {
                /* a first target outside the 32-bit path: without its effect
                 * where no 2nd target can lie under it, else (EVB = 0) faded */
                uint64_t pm = 0, fm = 0;
                for (int q = j; q < j1; q++) {
                    uint64_t o = item_row(k, q) & ~cover.r[q];
                    pm |= o & ~need.r[q];
                    fm |= o & need.r[q];
                }
                if (!pm && !fm) continue;       /* drawn by the 32-bit path */
                if (pm) {
                    split = fm != 0;
                    fx = item_obj_semi(it->kind, it->id) && fr_obj_bright ? FX_BRIGHT : FX_PLAIN;
                }
            } else {
                if (skip_alpha && fx == FX_ALPHA) continue;
                if (blend32 && masks_ok) {
                    uint64_t out = 0;
                    for (int q = j; q < j1; q++) out |= item_row(k, q) & ~cover.r[q];
                    if (!out) continue;         /* drawn by the 32-bit path (or after it) */
                }
            }
            uint64_t ta = get_ticks();
            if (split)
                draw_split(k, &cover, &need, j, j1, fx, it->fx);
            else
                draw_item(it->kind, it->id, fx, mode, dispcnt);
            if (item_is_obj(it->kind)) t_obj += get_ticks() - ta; else t_bg += get_ticks() - ta;
        }
        j = j1;
    }
    raw_flush();

    /* ---- 32-bit path over the blend rectangles, then back to the display ---- */
    if (blend32) {
        int bw = R.gx1 - R.gx0, bh = R.gy1 - R.gy0;
        if (!blend_surf.buffer || blend_surf.width < bw || blend_surf.height < bh) {
            if (blend_surf.buffer) surface_free(&blend_surf);
            blend_surf = surface_alloc(FMT_RGBA32, W, H);
        }
        surface_t sub = surface_make_sub(&blend_surf, 0, 0, bw, bh);
        surface_t zsub;
        if (tags) {
            if (!zbuf_surf.buffer || zbuf_surf.width < bw || zbuf_surf.height < bh) {
                if (zbuf_surf.buffer) surface_free(&zbuf_surf);
                zbuf_surf = surface_alloc(FMT_RGBA16, W, H);
            }
            zsub = surface_make_sub(&zbuf_surf, 0, 0, bw, bh);
        }
        rdpq_attach(&sub, tags ? &zsub : NULL);
        R.is32 = 1;
        R.tx = -R.gx0; R.ty = -R.gy0;
        clip_reset();
        pipe_mode = PIPE_NONE;
        pipe_z = -1;
        if (nrn) pass32(rl_n, nrn, 0, &t_bg, &t_obj);
        if (nrt) pass32(rl_t, nrt, 1, &t_bg, &t_obj);
        z_state = ZT_NONE;
        raw_flush();
        rdpq_detach();
        rdpq_sync_full(NULL, NULL);             /* intermediate fully written */
        R.is32 = 0;
        R.tx = view->ox; R.ty = view->oy;
        set_window(R.gx0, R.gy0, R.gx1, R.gy1);
        clip_reset();
        int na = 0;
        for (int k = 0; k < nrn; k++) rl_all[na++] = rl_n[k];
        for (int k = 0; k < nrt; k++) rl_all[na++] = rl_t[k];
        if (tlut_final) {
            fin_prepare(bcfg, (int)eva, (int)evb);
            final_pass_tlut(rl_all, na, fin_bc, fin_npass);
            r_stats.blend_path = fin_npass == 2 ? 3 : 2;
        } else {
            final_pass(rl_all, na);
        }
        /* the items above every first target, over the whole region */
        set_window(R.gx0, R.gy0, R.gx1, R.gy1);
        for (int k = n32; k < nitems; k++) {
            uint64_t ta = get_ticks();
            draw_item(items[k].kind, items[k].id, items[k].fx, mode, dispcnt);
            if (item_is_obj(items[k].kind)) t_obj += get_ticks() - ta; else t_bg += get_ticks() - ta;
        }
    }
    r_stats.us_bg = (uint32_t)TICKS_TO_US(t_bg) - r_stats.us_raster;
    r_stats.us_obj = (uint32_t)TICKS_TO_US(t_obj);

done:
    obj_cpu_end();
    raw_flush();
    rdpq_set_scissor(0, 0, W, H);
    rdpq_fence();
    last_sp = rspq_syncpoint_new();
    have_sp = 1;
    rspq_flush();
    r_stats.us_total = us_since(t_start);
#ifdef GBA_RENDER_TRACE
    /* per-frame trace line for frames [GBA_RENDER_TRACE_FROM, GBA_RENDER_TRACE_TO) */
    if (r_frame >= GBA_RENDER_TRACE_FROM && r_frame < GBA_RENDER_TRACE_TO) {
        const gba_render_stats_t *s = &r_stats;
        debugf("RT %lu tot %lu wait %lu trk %lu bg %lu upd %lu obj %lu op %lu ras %lu | d %06lx ch %lu/%lu/%lu/%lu "
               "cells %lu/%lu obj %lu tiles %lu conv %lu aff %lu rects %lu loads %lu | %lux%lu disp %04x cmp %lu\n",
               (unsigned long)r_frame, (unsigned long)s->us_total, (unsigned long)s->us_wait,
               (unsigned long)s->us_track, (unsigned long)s->us_bg, (unsigned long)s->us_bg_update,
               (unsigned long)s->us_obj, (unsigned long)s->us_obj_parse, (unsigned long)s->us_raster,
               (unsigned long)s->dirty_mask, (unsigned long)bg_trace_grp[0], (unsigned long)bg_trace_grp[1],
               (unsigned long)bg_trace_grp[2], (unsigned long)bg_trace_grp[3],
               (unsigned long)s->cells_expanded, (unsigned long)s->cells_checked,
               (unsigned long)s->obj_drawn, (unsigned long)s->obj_tiles, (unsigned long)s->obj_units_conv,
               (unsigned long)s->obj_affine, (unsigned long)s->rects, (unsigned long)s->loads,
               (unsigned long)(view->x1 - view->x0), (unsigned long)(view->y1 - view->y0),
               (unsigned)io16(REG_DISPCNT), (unsigned long)bg_trace_cmp);
    }
#endif
}

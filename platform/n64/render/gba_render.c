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
 *         of a rectangle are composited on the CPU with ppu_soft's rules and
 *         drawn per priority (render_obj.c, obj_cpu_begin): a later OBJ with
 *         a better priority over an earlier one (its transparent texels
 *         still pass their priority down), OBJ mosaic, and with alpha
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
 *       - alpha with EVB>0 (and semi-transparent OBJs): the frame is drawn in
 *         1-cycle mode into a 32-bit intermediate where a colour c is stored
 *         as 4c (the sum of two layers cannot wrap the 8-bit channel), and the
 *         first-target layers are blended over it with a per-(EVA,EVB)
 *         blender/combiner configuration plus a TLUT remap (blend_table.h).
 *         A final pass converts to the display: a 2-cycle "2x" pass
 *         (v -> min(31, v>>2), exact for v <= 191) or, when EVA+EVB is large
 *         enough to exceed that, a TLUT pass that reads the intermediate as
 *         bytes and maps every value through a table (saturation included).
 *       - BLDCNT's rule that a pixel blends only when the layer under it is a
 *         2nd target: when a layer that is not a 2nd target (or the backdrop)
 *         can lie under a first target, every draw also writes a Z "tag"
 *         (2nd target or not) and first-target layers are drawn twice with a
 *         DECAL depth test (blended over tagged 2nd targets, plain elsewhere).
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
    if (gx0 < R.gx0) gx0 = R.gx0;
    if (gy0 < R.gy0) gy0 = R.gy0;
    if (gx1 > R.gx1) gx1 = R.gx1;
    if (gy1 > R.gy1) gy1 = R.gy1;
    r_clip.tx = R.tx; r_clip.ty = R.ty;
    r_clip.x0 = gx0 + R.tx; r_clip.y0 = gy0 + R.ty;
    r_clip.x1 = gx1 + R.tx; r_clip.y1 = gy1 + R.ty;
}

void r_scissor_region(void) {
    r_scissor(R.gx0, R.gy0, R.gx1, R.gy1);
}

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
        if (gy + r1 <= R.gy0 || gy + r0 >= R.gy1)
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
 * priority | semi << 2) */
static inline int item_is_obj(int kind) {
    return kind >= 3;
}
static inline int item_obj_semi(int kind, int id) {
    return kind == 3 ? obj_info(id)->semi : kind == 4 ? (id >> 2) & 1 : 0;
}

static void final_pass(surface_t *disp) {
    /* 32-bit intermediate (v = 4c, up to 255) -> display: min(31, v>>2) */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, 0, PRIM, TEX0), (0, 0, 0, 1),
                                      (COMBINED, 0, ENV, COMBINED), (0, 0, 0, 1)));
    rdpq_set_prim_color(RGBA32(FIN_C0, FIN_C0, FIN_C0, 255));
    rdpq_set_env_color(RGBA32(FIN_C1, FIN_C1, FIN_C1, 255));
    rdpq_mode_dithering(DITHER_NONE_NONE);
    pipe_mode = PIPE_NONE;
    pipe_tlut = -1;
    int w = R.gx1 - R.gx0, h = R.gy1 - R.gy0;
    const int CWF = 64, CHF = 16;               /* 1024 RGBA32 texels = full TMEM (hi/lo halves) */
    raw_reserve(7);
    raw_put(RAW_SYNC_PIPE);
    raw_put(raw_tex_image(RF_RGBA, RS_32, blend_surf.stride / 4, PhysicalAddr(blend_surf.buffer)));
    raw_put(RAW_SYNC_TILE);
    raw_put(raw_set_tile(RF_RGBA, RS_32, CWF * 2, 0, 1, 0));   /* RGBA32: TMEM line = width*2 */
    raw_put(raw_set_tile(RF_RGBA, RS_32, CWF * 2, 0, 0, 0));
    raw_put(raw_set_tile_size(0, 0, 0, CWF, CHF));
    for (int y = 0; y < h; y += CHF) {
        int y1 = y + CHF > h ? h : y + CHF;
        for (int x = 0; x < w; x += CWF) {
            int x1 = x + CWF > w ? w : x + CWF;
            int fx = x + R.gx0 + R.tx, fy = y + R.gy0 + R.ty;
            raw_reserve(5);
            raw_put(RAW_SYNC_TILE);
            raw_put(RAW_SYNC_LOAD);
            raw_put(raw_load_tile(1, x, y, x1, y1));
            raw_put((0x24ull << 56) | ((uint64_t)((fx + x1 - x) * 4) << 44) |
                    ((uint64_t)((fy + y1 - y) * 4) << 32) | ((uint64_t)(fx * 4) << 12) | (uint64_t)(fy * 4));
            raw_put((0x400ull << 16) | 0x400);          /* s = t = 0, dsdx = dtdy = 1 */
            r_stats.rects++;
            r_stats.loads++; r_stats.load_bytes += (uint32_t)((x1 - x) * 4 * (y1 - y));
        }
    }
    raw_flush();
    r_stats.px_2cyc += (uint32_t)(w * h);
    (void)disp;
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
 * the intermediate is read as CI8 bytes through a TLUT holding F.
 *   A (2-cycle): TEX0 = R byte, TEX1 = G byte -> writes (F(R), F(G), 0)
 *   B (1-cycle): B byte, blender adds F(B) (at most 30) into blue
 *   C (1-cycle): blender adds 1 more to blue where F(B) = 31
 * (the blender can add at most 247/8 to an empty channel: P*31/32 with P<=255) */
static void final_pass_tlut(void) {
    /* blue increments: smallest texel t whose T8(t)*31/32 lands in [8k, 8k+7] */
    static uint8_t tk[32];
    if (!tk[1]) {
        for (int k = 0; k < 31; k++)
            for (int t = 0; t < 32; t++) {
                int q = (((t << 3) | (t >> 2)) * 31) >> 5;
                if (q >= 8 * k && q <= 8 * k + 7) { tk[k] = (uint8_t)t; break; }
            }
    }
    uint64_t *ta = (uint64_t *)fin_tlut, *tb = (uint64_t *)(fin_tlut + 256), *tc = (uint64_t *)(fin_tlut + 512);
    for (int v = 0; v < 256; v += 4) {
        uint64_t wa = 0, wb = 0, wc = 0;
        for (int k = 0; k < 4; k++) {
            uint32_t f = fin_F[v + k];
            uint32_t fb = f > 30 ? 30 : f;
            wa = (wa << 16) | (uint16_t)((f << 11) | (f << 6) | (f << 1) | 1);
            wb = (wb << 16) | (uint16_t)(f ? ((uint32_t)tk[fb] << 1) | 1 : 0);
            wc = (wc << 16) | (uint16_t)(f == 31 ? ((uint32_t)tk[1] << 1) | 1 : 0);
        }
        ta[v >> 2] = wa; tb[v >> 2] = wb; tc[v >> 2] = wc;
    }
    int w = R.gx1 - R.gx0, h = R.gy1 - R.gy0;
    uint32_t img = PhysicalAddr(blend_surf.buffer);
    /* loaded as RGBA16 texels (an image is at most 1024 texels wide), drawn as CI8 bytes */
    int img_w = blend_surf.stride / 2;
    const int CW = 64, CH = 8;                      /* 64 pixels = 256 bytes x 8 rows = 2KB */
    for (int pass = 0; pass < 3; pass++) {
        raw_flush();
        rdpq_set_mode_standard();
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_dithering(DITHER_NONE_NONE);
        if (pass == 0) {
            rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, 0, PRIM, 0), (0, 0, 0, 1),
                                              (TEX1, 0, ENV, COMBINED), (0, 0, 0, 1)));
            rdpq_set_prim_color(RGBA32(255, 0, 0, 255));
            rdpq_set_env_color(RGBA32(0, 255, 0, 255));
        } else {
            rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, TEX0), (0, 0, 0, TEX0)));
            rdpq_mode_alphacompare(1);
            rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, FOG_ALPHA, MEMORY_RGB, ONE)));
            rdpq_set_fog_color(RGBA32(0, 0, 0, 255));
        }
        rdpq_tex_upload_tlut(fin_tlut + pass * 256, 0, 256);
        raw_reserve(8);
        raw_put(RAW_SYNC_PIPE);
        raw_put(raw_tex_image(RF_RGBA, RS_16, img_w, img));
        raw_put(RAW_SYNC_TILE);
        raw_put(raw_set_tile(RF_RGBA, RS_16, CW * 4, 0, 2, 0));       /* load tile */
        raw_put(raw_set_tile(RF_CI, RS_8, CW * 4, 0, 0, 0));
        raw_put(raw_set_tile(RF_CI, RS_8, CW * 4, 0, 1, 0));
        raw_put(raw_set_tile_size(0, pass == 0 ? 1 : 0, 0, CW * 4, CH)); /* TEX0: s - 1 / s */
        raw_put(raw_set_tile_size(1, 0, 0, CW * 4, CH));                 /* TEX1: s */
        int s0 = pass == 0 ? 1 : 2;
        for (int y = 0; y < h; y += CH) {
            int y1 = y + CH > h ? h : y + CH;
            for (int x = 0; x < w; x += CW) {
                int x1 = x + CW > w ? w : x + CW;
                int fx = x + R.gx0 + R.tx, fy = y + R.gy0 + R.ty;
                raw_reserve(5);
                raw_put(RAW_SYNC_TILE);
                raw_put(RAW_SYNC_LOAD);
                raw_put(raw_load_tile(2, x * 2, y, x1 * 2, y1));
                r_stats.loads++; r_stats.load_bytes += (uint32_t)((x1 - x) * 4 * (y1 - y));
                raw_put((0x24ull << 56) | ((uint64_t)((fx + x1 - x) * 4) << 44) |
                        ((uint64_t)((fy + y1 - y) * 4) << 32) | ((uint64_t)(fx * 4) << 12) | (uint64_t)(fy * 4));
                raw_put(((uint64_t)(s0 * 32) << 48) | (0ull << 32) | (0x1000ull << 16) | 0x400);
                r_stats.rects++;
            }
        }
        if (pass == 0) r_stats.px_2cyc += (uint32_t)(w * h); else r_stats.px_1cyc += (uint32_t)(w * h);
    }
    raw_flush();
    pipe_mode = PIPE_NONE;
    pipe_tlut = -1;
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
    if (force_all) {
        dirty = 0xFFFFFFu;
        bg_invalidate();
        obj_invalidate();
        force_all = 0;
    }
    r_stats.dirty_pages = (uint32_t)__builtin_popcount(dirty);
    r_stats.dirty_mask = dirty;
    bg_track(dirty & 0xFFFFu);
    obj_track(dirty >> 16);
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
    uint8_t ocodes[8];
    int ncodes = 0;
    if (dispcnt & 0x1000) {
        uint64_t tp = get_ticks();
        obj_parse(dispcnt, olist, ocount);
        /* OBJs drawn as alpha items blend with (or, by the 2nd-target
         * rule, check) what is under them: under the GBA's OBJ layer that is
         * a BG, never another OBJ */
        ncodes = obj_cpu_begin(olist, ocount,
                               lfx[4] == FX_ALPHA ? OBJ_CPU_BLEND_ALL :
                               semi_fx == FX_ALPHA ? OBJ_CPU_BLEND_SEMI : OBJ_CPU_BLEND_NONE, ocodes);
        uint64_t tc = get_ticks();
        r_stats.us_obj_parse = (uint32_t)TICKS_TO_US(tc - tp);
        obj_prepare(olist, ocount);             /* OBJ shadow: part of us_track */
        r_stats.us_track += (uint32_t)TICKS_TO_US(get_ticks() - tc);
    }

    /* ---- draw list in painter's order: priority 3..0, BGs (3..0), then the
     * CPU-composited OBJ pixels of that priority, then OBJs (127..0) ---- */
    typedef struct { uint8_t kind, id, fx, l2; } item_t;    /* kind: 0..2 BG kind-1, 3 OBJ, 4 CPU OBJs */
    static item_t items[4 + 8 + 128];
    int nitems = 0;
    int bgprio[4];
    for (int n = 0; n < 4; n++)
        bgprio[n] = (int)(io16(REG_BG0CNT + 2 * n) & 3);
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
    int blend32 = 0, tags = 0, last_alpha = -1;
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
    int defer = 0, bd_defer = 0;
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
    r_stats.blend_path = (uint32_t)blend32;
    r_stats.tags = (uint32_t)tags;

    /* ---- target + backdrop ---- */
    uint32_t bd = r_pal16(0), bd_bright = bd;
    if (lfx[5] == FX_BRIGHT) {
        const uint8_t *t = tau_bright;
        bd_bright = t[bd & 31] | (t[(bd >> 5) & 31] << 5) | (t[(bd >> 10) & 31] << 10);
        if (!bd_defer) bd = bd_bright;
    }
    z_state = ZT_NONE; z_tag = 0;
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
        if (tags) {
            /* backdrop colour and its tag in one 1-cycle fill */
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
            rdpq_set_prim_color(RGBA32((bd & 31) * 4, ((bd >> 5) & 31) * 4, ((bd >> 10) & 31) * 4, 255));
            rdpq_mode_zoverride(true, 0, 0);
            rdpq_set_prim_depth_raw(bd_defer ? TAG_DEFERRED : ((l2 >> 5) & 1) ? TAG_TARGET : TAG_NONTARGET, 0);
            rdpq_mode_zbuf(false, true);
            rdpq_fill_rectangle(0, 0, bw, bh);
            r_stats.px_1cyc += (uint32_t)(bw * bh);
        } else {
            rdpq_set_mode_fill(RGBA32((bd & 31) * 4, ((bd >> 5) & 31) * 4, ((bd >> 10) & 31) * 4, 255));
        }
    } else {
        rdpq_set_mode_fill(color_from_packed16(r_conv(bd)));
    }
    r_scissor_region();
    if (!tags)
        fill_g(R.gx0, R.gy0, R.gx1, R.gy1);
    pipe_mode = PIPE_NONE;

    /* ---- layers ---- */
    uint64_t t_bg = 0, t_obj = 0;
    for (int k = 0; k < nitems; k++) {
        const item_t *it = &items[k];
        uint64_t ta = get_ticks();
        int tag = it->l2 ? TAG_TARGET : TAG_NONTARGET;
        if (!tags) {
            draw_item(it->kind, it->id, it->fx, mode, dispcnt);
        } else if (it->fx != FX_ALPHA) {
            int dfr = defer && it->fx == FX_BRIGHT && it->l2;   /* brightened at the end */
            z_state = ZT_WRITE; z_tag = dfr ? TAG_DEFERRED : tag;
            draw_item(it->kind, it->id, dfr ? FX_PLAIN : it->fx, mode, dispcnt);
        } else {
            /* first target: blend where the tag under it says 2nd target ... */
            z_state = ZT_DECAL; z_tag = TAG_TARGET;
            draw_item(it->kind, it->id, FX_ALPHA, mode, dispcnt);
            if (defer) {                        /* (raw deferred 2nd targets too) */
                z_state = ZT_DECAL; z_tag = TAG_DEFERRED;
                draw_item(it->kind, it->id, FX_ALPHA, mode, dispcnt);
            }
            /* ... draw it unblended where it does not (a semi-transparent OBJ
             * keeps the OBJ layer's brighten/darken there) ... */
            int nfx = item_obj_semi(it->kind, it->id) && lfx[4] == FX_BRIGHT ? FX_BRIGHT : FX_PLAIN;
            z_state = ZT_DECAL; z_tag = TAG_NONTARGET;
            draw_item(it->kind, it->id, nfx, mode, dispcnt);
            /* ... and leave its own tag for first targets drawn later (and
             * for the deferred brighten, which must not touch it) */
            if (k < last_alpha || defer) {
                z_state = ZT_KEEP; z_tag = tag;
                draw_item(it->kind, it->id, FX_PLAIN, mode, dispcnt);
            }
        }
        if (item_is_obj(it->kind)) t_obj += get_ticks() - ta; else t_bg += get_ticks() - ta;
    }
    if (defer) {
        /* deferred brighten/darken where a deferred layer is still on top: the
         * backdrop, then the deferred layers in painter's order (at a pixel
         * the last opaque one is the top layer) */
        uint64_t ta = get_ticks();
        if (bd_defer) {
            raw_flush();
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
            rdpq_set_prim_color(RGBA32((bd_bright & 31) * 4, ((bd_bright >> 5) & 31) * 4,
                                       ((bd_bright >> 10) & 31) * 4, 255));
            rdpq_mode_zoverride(true, 0, 0);
            rdpq_set_prim_depth_raw(TAG_DEFERRED, 0);
            rdpq_mode_zbuf(true, false);
            rdpq_mode_zmode(ZMODE_DECAL);
            rdpq_fill_rectangle(0, 0, R.gx1 - R.gx0, R.gy1 - R.gy0);
            r_stats.px_1cyc += (uint32_t)((R.gx1 - R.gx0) * (R.gy1 - R.gy0));
            pipe_mode = PIPE_NONE;
            pipe_z = -1;
        }
        z_state = ZT_DECAL; z_tag = TAG_DEFERRED;
        for (int k = 0; k < nitems; k++)
            if (items[k].fx == FX_BRIGHT && items[k].l2)
                draw_item(items[k].kind, items[k].id, FX_BRIGHT, mode, dispcnt);
        t_bg += get_ticks() - ta;
    }
    z_state = ZT_NONE;
    r_stats.us_bg = (uint32_t)TICKS_TO_US(t_bg) - r_stats.us_raster;
    r_stats.us_obj = (uint32_t)TICKS_TO_US(t_obj);

    raw_flush();
    if (blend32) {
        rdpq_detach();
        rdpq_sync_full(NULL, NULL);             /* intermediate fully written */
        R.is32 = 0;
        R.tx = view->ox; R.ty = view->oy;
        clip_reset();
        r_scissor_region();
        if (tlut_final) {
            build_fin_map(bcfg, (int)eva, (int)evb);
            final_pass_tlut();
            r_stats.blend_path = 2;
        } else {
            final_pass(disp);
        }
    }

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

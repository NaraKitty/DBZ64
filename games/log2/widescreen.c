/*
 * widescreen.c - 320x240 field view of LoG2.  See runtime/widescreen.h for
 * the interface and docs/WIDESCREEN.md for the design.
 *
 * Game facts used here (DBZ Legacy of Goku II USA, verified by disassembly;
 * see docs/WIDESCREEN.md for the derivations):
 *
 *  camera / view object 0x03001090:
 *    +0x0C L, +0x10 T, +0x14 R=L+240, +0x18 B=T+160 (camera rect, px)
 *    +0x2C, +0x30  L, T used by the last layer update (0x080052B0) - i.e. the
 *                  camera of the frame the hardware is showing
 *    +0x34         incremented by every field present (0x080052BC)
 *    +0x54, +0x58  map width / height (px)
 *  layer set 0x0300107C: u32[4] layer objects, one per BG
 *    field layer (vtable 0x08023868): +4 u8 BG, +8 resource,
 *      +0x1C..0x1F owner chunk index per slot (0xFF none),
 *      +0x28 + slot*0x800 decoded 32x32 chunk
 *    wrap layer  (vtable 0x08023C44): +4 u8 BG, +8 resource (hardware ring
 *      holds the authored repeat)
 *    resource (factory 0x080056A1): +4 fx, +8 fy (1024 = 1.0), +0xC ox,
 *      +0x10 oy (22.10), +0x14 u8 cols, +0x15 u8 rows (256 px chunks),
 *      +0x18 u32 descriptors[cols*rows]
 *    BGxHOFS = (fx*L + ox + shake) >> 10, written with all 16 bits
 *  sprite manager build 0x08006AD8: clears the shadow OAM 0x030022BC (call at
 *    0x08006BCE), then for each visible object calls emit (vt+0x10) at
 *    0x08006C00 with r1 = sp = cursor {oam_ptr, aff_ptr, aff_idx}; the shadow
 *    is copied to OAM by 0x0801F566 (calls at 0x08006AAE, 0x080070B0)
 */
#include <string.h>
#include <stdio.h>
#include "widescreen.h"
#include "gba.h"
#include "io.h"

int ws_enabled = 1;
int ws_widen_despawn = 1;
int ws_hud_inset = 8;
int ws_debug_flags;

/* ------------------------------------------------------------ addresses */

#define CAM            0x03001090u
#define CAM_L          (CAM + 0x0C)
#define CAM_T          (CAM + 0x10)
#define CAM_SHOWN_L    (CAM + 0x2C)
#define CAM_SHOWN_T    (CAM + 0x30)
#define CAM_PRESENTS   (CAM + 0x34)
#define CAM_MAP        (CAM + 0x50)
#define CAM_W          (CAM + 0x54)
#define CAM_H          (CAM + 0x58)
#define LAYER_SET      0x0300107Cu
#define VT_FIELD_LAYER 0x08023868u
#define VT_WRAP_LAYER  0x08023C44u
#define RES_FACTORY    0x080056A1u
#define SHADOW_OAM     0x030022BCu
#define SHADOW_END     (SHADOW_OAM + 0x400u)
#define FN_RETURN_0    0x080264ACu     /* Thumb: movs r0,#0; bx lr */

/* ------------------------------------------------------------ guest access */

static inline uint32_t ew8(uint32_t a) {
    return ((const uint8_t *)gba_mem.ewram)[(a & 0x3FFFFu) ^ GBA_BX];
}
static inline uint32_t ew16(uint32_t a) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.ewram + ((a & 0x3FFFEu) ^ GBA_HX));
}
static inline uint32_t ew32(uint32_t a) {
    return gba_mem.ewram[(a & 0x3FFFCu) >> 2];
}
static inline uint32_t iw32(uint32_t a) {
    return gba_mem.iwram[(a & 0x7FFCu) >> 2];
}
static inline uint32_t iw16(uint32_t a) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.iwram + ((a & 0x7FFEu) ^ GBA_HX));
}
static inline uint32_t oam16(uint32_t o) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.oam + ((o & 0x3FEu) ^ GBA_HX));
}
static inline int is_ewram(uint32_t a, uint32_t len) {
    return a >= 0x02000000u && a + len <= 0x02040000u;
}
static inline int is_rom(uint32_t a, uint32_t len) {
    return a >= 0x08000000u && a + len <= 0x08800000u;
}
/* any readable guest word (ROM through the runtime's accessor) */
static uint32_t g32(uint32_t a) {
    uint32_t r = a >> 24;
    if (r == 2) return ew32(a);
    if (r == 3) return iw32(a);
    if (r == 8) return rd32(a & ~3u);
    return 0;
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------ state */

static ws_stats_t st;
static int field_on;                 /* field view this frame */
static int OX, OY;                   /* view origin relative to the native screen */
static uint32_t last_presents;       /* CAM_PRESENTS at the previous VBlank */
static int frames_since_present = 99;
static uint32_t vblank_count;
static uint32_t view_requested;      /* vblank_count when ws_get_view() last gave the field view */
static uint32_t last_map_key;        /* map pointer/size of the field shown (0: none) */
static uint32_t map_generation;      /* ws_map_generation() */

/* the widening is only applied when the extended view is really presented */
static inline int widen_active(void) {
    return ws_enabled && field_on && vblank_count - view_requested <= 2;
}

/* View rectangle for camera (L,T) on a W x H map: the 320x240 window around
 * the 240x160 camera, clamped to the map, centred on small maps, and always
 * containing the native view. */
static void view_rect(int L, int T, int W, int H, int *X0, int *Y0) {
    int x = L - 40, y = T - 40;
    x = W >= WS_W ? clampi(x, 0, W - WS_W) : (W - WS_W) / 2;
    y = H >= WS_H ? clampi(y, 0, H - WS_H) : (H - WS_H) / 2;
    *X0 = clampi(x, L + 240 - WS_W, L);
    *Y0 = clampi(y, T + 160 - WS_H, T);
}

static inline int map_w(void) { return (int)iw32(CAM_W); }
static inline int map_h(void) { return (int)iw32(CAM_H); }

/* ------------------------------------------------------------ culling overrides */

/* camera bound v -> widened bound, from the camera as the game sees it now */
static inline uint32_t widen_x(uint32_t v, int right) {
    int L = (int)v - (right ? 240 : 0), X0, Y0;
    view_rect(L, (int)iw32(CAM_T), map_w(), map_h(), &X0, &Y0);
    return (uint32_t)(right ? X0 + WS_W : X0);
}
static inline uint32_t widen_y(uint32_t v, int bottom) {
    int T = (int)v - (bottom ? 160 : 0), X0, Y0;
    view_rect((int)iw32(CAM_L), T, map_w(), map_h(), &X0, &Y0);
    return (uint32_t)(bottom ? Y0 + WS_H : Y0);
}

uint32_t ws_cull_left(uint32_t v)   { return widen_active() ? widen_x(v, 0) : v; }
uint32_t ws_cull_right(uint32_t v)  { return widen_active() ? widen_x(v, 1) : v; }
uint32_t ws_cull_top(uint32_t v)    { return widen_active() ? widen_y(v, 0) : v; }
uint32_t ws_cull_bottom(uint32_t v) { return widen_active() ? widen_y(v, 1) : v; }
uint32_t ws_despawn_left(uint32_t v)   { return ws_widen_despawn ? ws_cull_left(v) : v; }
uint32_t ws_despawn_right(uint32_t v)  { return ws_widen_despawn ? ws_cull_right(v) : v; }
uint32_t ws_despawn_top(uint32_t v)    { return ws_widen_despawn ? ws_cull_top(v) : v; }
uint32_t ws_despawn_bottom(uint32_t v) { return ws_widen_despawn ? ws_cull_bottom(v) : v; }

/* Screen-space culls against constant native-screen rectangles in ROM (the
 * particle cull 0x0800ECF8 uses {-7,-7,240,160} at 0x086ADDD8; the beam
 * emitter 0x08014894 tests each segment against {0,0,240,160} at 0x086E3604
 * through the rect test 0x0802805C).  The bound v is in native screen
 * coordinates: shift it by the view offset of the live camera. */
static inline void view_offset(int *ox, int *oy) {
    int L = (int)iw32(CAM_L), T = (int)iw32(CAM_T), X0, Y0;
    view_rect(L, T, map_w(), map_h(), &X0, &Y0);
    *ox = X0 - L;
    *oy = Y0 - T;
}
uint32_t ws_scull_left(uint32_t v)   { int ox, oy; if (!widen_active()) return v; view_offset(&ox, &oy); return v + (uint32_t)ox; }
uint32_t ws_scull_right(uint32_t v)  { int ox, oy; if (!widen_active()) return v; view_offset(&ox, &oy); return v + (uint32_t)(ox + WS_W - 240); }
uint32_t ws_scull_top(uint32_t v)    { int ox, oy; if (!widen_active()) return v; view_offset(&ox, &oy); return v + (uint32_t)oy; }
uint32_t ws_scull_bottom(uint32_t v) { int ox, oy; if (!widen_active()) return v; view_offset(&ox, &oy); return v + (uint32_t)(oy + WS_H - 160); }

/* 0x0802805C is a generic rect-overlap test (r0, r1 = rects); only the
 * constant screen rectangle 0x086E3604 is widened */
#define SCREEN_RECT_ROM 0x086E3604u
static uint32_t recttest_r1;
void ws_hook_recttest(const uint32_t *regs) { recttest_r1 = regs[1]; }
uint32_t ws_rect_left(uint32_t v)   { return recttest_r1 == SCREEN_RECT_ROM ? ws_scull_left(v) : v; }
uint32_t ws_rect_right(uint32_t v)  { return recttest_r1 == SCREEN_RECT_ROM ? ws_scull_right(v) : v; }
uint32_t ws_rect_top(uint32_t v)    { return recttest_r1 == SCREEN_RECT_ROM ? ws_scull_top(v) : v; }
uint32_t ws_rect_bottom(uint32_t v) { return recttest_r1 == SCREEN_RECT_ROM ? ws_scull_bottom(v) : v; }

/* ------------------------------------------------------------ chunk decoder */

/* Webfoot's bit-stream LZ for field map chunks (ported from the reference
 * recomp's legacy2_extended_view.cpp decode_field_chunk).  Descriptor:
 * u32 kind (0 raw, 1/2 LZ with that many initial literal bits), u32 size,
 * then the data; bits are read MSB first from little-endian words. */
typedef struct {
    uint32_t addr, word;
    unsigned rem;
    int ok;
} bitreader_t;

static uint32_t br_bits(bitreader_t *b, unsigned n) {
    uint32_t v = 0;
    while (n) {
        if (!b->rem) {
            if (!is_rom(b->addr, 4)) { b->ok = 0; return 0; }
            b->word = rd32(b->addr);
            b->addr += 4;
            b->rem = 32;
        }
        unsigned k = n < b->rem ? n : b->rem;          /* 1..24 */
        v = (v << k) | (b->word >> (32 - k));
        b->word <<= k;
        b->rem -= k;
        n -= k;
    }
    return v;
}

static uint32_t br_gamma(bitreader_t *b) {
    uint32_t v = 1;
    for (int i = 0; i < 30; i++) {
        v = (v << 1) | br_bits(b, 1);
        if (!br_bits(b, 1) || !b->ok)
            return v;
    }
    b->ok = 0;
    return 0;
}

#define CHUNK_BYTES 0x800u

static int decode_chunk(uint32_t desc, uint8_t *out) {
    if (!is_rom(desc, 8))
        return 0;
    uint32_t kind = rd32(desc), size = rd32(desc + 4);
    if (size != CHUNK_BYTES)
        return 0;
    if (kind == 0) {
        if (!is_rom(desc + 8, CHUNK_BYTES)) return 0;
        for (uint32_t i = 0; i < CHUNK_BYTES; i++)
            out[i] = (uint8_t)rd8(desc + 8 + i);
        return 1;
    }
    if (kind != 1 && kind != 2)
        return 0;
    bitreader_t b = { desc + 8, 0, 0, 1 };
    unsigned n = 0, dbits = 8, lbits = kind;
    uint32_t lbase = 0, last = 1;
#define PUT(v) do { if (n >= CHUNK_BYTES) return 0; out[n++] = (uint8_t)(v); } while (0)
#define COPY(d, l) do { uint32_t d_ = (d), l_ = (l); \
        if (!d_ || d_ > n || l_ > CHUNK_BYTES - n) return 0; \
        while (l_--) { out[n] = out[n - d_]; n++; } } while (0)
    for (unsigned cmd = 0; cmd < 65536u; cmd++) {
        if (br_bits(&b, 1)) {                   /* literal */
            if (lbits > 24) return 0;
            PUT(br_bits(&b, lbits) + lbase);
            continue;
        }
        if (br_bits(&b, 1)) {                   /* long match */
            uint32_t code = br_gamma(&b), dist, len;
            if (code == 2) {
                dist = last;
                len = br_gamma(&b);
            } else {
                if (dbits > 24 || code < 3) return 0;
                dist = br_bits(&b, dbits) + ((code - 3) << dbits);
                last = dist;
                len = br_gamma(&b);
                if (dist >= 0x10000u) len += 3;
                else if (dist >= 0x37FFu) len += 2;
                else if (dist >= 0x27Fu) len += 1;
                else if (dist <= 0x7Fu) len += 4;
            }
            if (!b.ok) return 0;
            COPY(dist, len);
            continue;
        }
        if (!br_bits(&b, 1)) {                  /* short match / control */
            uint32_t dist = br_bits(&b, 7);
            if (dist) {
                last = dist;
                COPY(dist, br_bits(&b, 2) + 2);
            } else {
                unsigned c = br_bits(&b, 2);
                if (!c)
                    return b.ok && n == CHUNK_BYTES;   /* end of stream */
                dbits = br_bits(&b, c + 3);
            }
            if (!b.ok) return 0;
            continue;
        }
        int sc = (int)br_bits(&b, 4) - 1;
        if (sc == 0) {
            PUT(0);
        } else if (sc > 0) {
            COPY((uint32_t)sc, 1);
        } else if (br_bits(&b, 1)) {            /* raw 256-byte blocks */
            do {
                for (int i = 0; i < 256; i++)
                    PUT(br_bits(&b, 8));
            } while (br_bits(&b, 1) && b.ok);
        } else {                                /* literal width change */
            lbase = 0;
            lbits = 7 + br_bits(&b, 1);
            if (lbits == 7)
                lbase = br_bits(&b, 8);
        }
        if (!b.ok) return 0;
    }
#undef PUT
#undef COPY
    return 0;
}

/* LRU cache of decoded chunks (tilemap entries, native order) */
#define DCACHE_N 32
static struct {
    uint32_t desc;          /* 0 = empty */
    uint32_t used;
    int ok;
    uint16_t e[CHUNK_BYTES / 2];
} dcache[DCACHE_N];
static uint32_t dcache_clock;

static const uint16_t *decoded_chunk(uint32_t desc) {
    int victim = 0;
    for (int i = 0; i < DCACHE_N; i++) {
        if (dcache[i].desc == desc) {
            dcache[i].used = ++dcache_clock;
            return dcache[i].ok ? dcache[i].e : NULL;
        }
        if (dcache[i].used < dcache[victim].used)
            victim = i;
    }
    static uint8_t buf[CHUNK_BYTES];
    int ok = decode_chunk(desc, buf);
    st.chunk_decodes++;
    dcache[victim].desc = desc;
    dcache[victim].used = ++dcache_clock;
    dcache[victim].ok = ok;
    if (ok)
        for (unsigned i = 0; i < CHUNK_BYTES / 2; i++)
            dcache[victim].e[i] = (uint16_t)(buf[2 * i] | (buf[2 * i + 1] << 8));
    return ok ? dcache[victim].e : NULL;
}

/* ------------------------------------------------------------ field layers */

enum { LAYER_NONE, LAYER_FIELD, LAYER_WRAP };

#define RUNS 6
typedef struct {
    int type;
    uint32_t obj, res;
    int cols, rows;
    int cx0, cy0, cx1, cy1;      /* clip rect, layer pixels */
    int h, v;                    /* signed scroll of the shown frame */
    int has_blank;
    uint16_t blank;              /* entry of an all-zero tile */
    /* chunk lookups of this frame: idx -> loaded slot (EWRAM) or decoded data */
    int run_idx[RUNS];
    uint32_t run_ew[RUNS];
    const uint16_t *run_dec[RUNS];
    int run_next;
} layer_t;
static layer_t lay[4];
static int blank_tile_cache[4] = { 0, 0, 0, 0 };   /* per char block */

/* an all-zero 64-byte block in char block cb (so a blank cell is
 * transparent): tile t for 8bpp, tiles 2t/2t+1 for 4bpp */
static int find_blank_tile(int cb, int bpp8, uint16_t *entry) {
    const uint32_t *base = gba_mem.vram + (cb * 0x4000u) / 4;
    int cand = blank_tile_cache[cb];
    for (int pass = 0; pass < 2; pass++) {
        int from = pass ? 0 : cand, to = pass ? 1024 : cand + 1;
        for (int t = from; t < to; t++) {
            if (cb * 0x4000 + t * 64 + 64 > 0x10000) break;
            const uint32_t *p = base + t * 16;
            uint32_t acc = 0;
            for (int k = 0; k < 16; k++) acc |= p[k];
            if (!acc) {
                blank_tile_cache[cb] = t;
                *entry = (uint16_t)(bpp8 ? t : 2 * t);
                return 1;
            }
        }
    }
    return 0;
}

static int setup_layer(int bg, uint32_t obj, uint32_t vt) {
    layer_t *l = &lay[bg];
    memset(l, 0, sizeof *l);
    for (int k = 0; k < RUNS; k++) l->run_idx[k] = -1;
    l->h = (int16_t)io16(REG_BG0HOFS + 4 * bg);
    l->v = (int16_t)io16(REG_BG0HOFS + 4 * bg + 2);
    if (vt == VT_WRAP_LAYER) {
        l->type = LAYER_WRAP;
        return 1;
    }
    uint32_t res = ew32(obj + 8);
    if (!is_rom(res, 0x18) || rd32(res) != RES_FACTORY)
        return 0;
    int cols = (int)rd8(res + 0x14), rows = (int)rd8(res + 0x15);
    if (!cols || !rows || cols * rows > 0xFF || !is_rom(res + 0x18, (uint32_t)(4 * cols * rows)))
        return 0;
    int32_t fx = (int32_t)rd32(res + 4), fy = (int32_t)rd32(res + 8);
    int32_t ox = (int32_t)rd32(res + 0xC), oy = (int32_t)rd32(res + 0x10);
    l->type = LAYER_FIELD;
    l->obj = obj;
    l->res = res;
    l->cols = cols;
    l->rows = rows;
    /* factor-1.0 axes: the map rectangle in layer space (anything outside was
     * never visible on the GBA); other factors: the whole chunk grid */
    l->cx0 = 0; l->cx1 = cols * 256;
    l->cy0 = 0; l->cy1 = rows * 256;
    if (fx == 1024) {
        l->cx0 = imax(l->cx0, ox >> 10);
        l->cx1 = imin(l->cx1, (ox >> 10) + map_w());
    }
    if (fy == 1024) {
        l->cy0 = imax(l->cy0, oy >> 10);
        l->cy1 = imin(l->cy1, (oy >> 10) + map_h());
    }
    uint32_t cnt = io16(REG_BG0CNT + 2 * bg);
    l->has_blank = find_blank_tile((int)((cnt >> 2) & 3), (cnt & 0x80) != 0, &l->blank);
    return 1;
}

/* is the shown frame a field map (mode 0, field layer objects driving BG0-3)? */
static int detect_field(void) {
    uint32_t d = io16(REG_DISPCNT);
    if ((d & 0x87) != 0 || !(d & 0x1000))
        return 0;
    if (frames_since_present > 3)        /* the field present must be running */
        return 0;
    int W = map_w(), H = map_h();
    if (W < 240 || H < 160 || W > 8192 || H > 8192)
        return 0;
    for (int bg = 0; bg < 4; bg++) {
        uint32_t cnt = io16(REG_BG0CNT + 2 * bg);
        if (((cnt >> 8) & 31) != 28u + (uint32_t)bg || (cnt & 0xC000))
            return 0;
        uint32_t o = iw32(LAYER_SET + 4 * bg);
        if (!is_ewram(o, 0x2028))
            return 0;
        uint32_t vt = ew32(o);
        if (vt != VT_FIELD_LAYER && vt != VT_WRAP_LAYER)
            return 0;
        if (ew8(o + 4) != (uint32_t)bg)
            return 0;
        if (!setup_layer(bg, o, vt))
            return 0;
    }
    return 1;
}

static int ws_bg_entry(int bg, int cx, int cy, uint16_t *e) {
    layer_t *l = &lay[bg & 3];
    if (l->type != LAYER_FIELD)
        return 0;
    /* world pixel of the cell, from the 16-bit scroll space */
    int wx = (int16_t)(uint16_t)((uint32_t)cx << 3);
    int wy = (int16_t)(uint16_t)((uint32_t)cy << 3);
    /* the hardware ring covers the native screen exactly as the game drew it */
    if (wx + 8 > l->h && wx < l->h + 240 && wy + 8 > l->v && wy < l->v + 160)
        return 0;
    if (wx + 8 <= l->cx0 || wx >= l->cx1 || wy + 8 <= l->cy0 || wy >= l->cy1 || wx < 0 || wy < 0)
        goto blank;
    int idx = (wy >> 8) * l->cols + (wx >> 8);
    int k;
    for (k = 0; k < RUNS; k++)
        if (l->run_idx[k] == idx)
            break;
    if (k == RUNS) {
        k = l->run_next;
        l->run_next = (k + 1) % RUNS;
        l->run_idx[k] = idx;
        l->run_ew[k] = 0;
        l->run_dec[k] = NULL;
        for (int s = 0; s < 4 && !(ws_debug_flags & WS_DBG_FORCE_DECODE); s++)
            if (ew8(l->obj + 0x1C + s) == (uint32_t)idx) {
                l->run_ew[k] = l->obj + 0x28 + (uint32_t)s * CHUNK_BYTES;
                break;
            }
        if (!l->run_ew[k])
            l->run_dec[k] = decoded_chunk(rd32(l->res + 0x18 + 4 * (uint32_t)idx));
    }
    uint32_t off = (uint32_t)(((wy >> 3) & 31) * 32 + ((wx >> 3) & 31));
    if (l->run_ew[k]) {
        *e = (uint16_t)ew16(l->run_ew[k] + off * 2);
        return 1;
    }
    if (l->run_dec[k]) {
        *e = l->run_dec[k][off];
        return 1;
    }
blank:
    if (!l->has_blank)
        return 0;
    *e = l->blank;
    return 1;
}

/* ------------------------------------------------------------ sprite tracking */

/* World objects are positioned relative to the camera; screen-space objects
 * (HUD, dialogue, banners, menus) are re-anchored.  Unknown classes count as
 * world objects: they then keep their native place in the view. */
static const uint32_t world_culls[] = {
    0x080071FA, 0x08007A84, 0x0800FD3E, 0x08011896, 0x080136A6, 0x08013C8E, 0x0801647A,
};
static const uint32_t screen_culls[] = {
    0x0800775A,     /* position set by its owner (HUD pieces) */
    0x08019050,
    0x08007CDE,     /* position at obj+0x150/0x154 */
    0x08012A28,     /* fixed native centre */
    0x080264A4,     /* ScreenSprite (vt 0x08024EBC) */
};
static const uint32_t screen_vts[] = {
    0x0802403C,     /* 4x 64x64 affine cluster (dialogue window), emit 0x0800BBEE */
    0x0802408C,     /* strip of sprites at fixed x 8,40,56.. (emit 0x08008BEE) */
    0x0802527C,     /* icon at fixed (216,8) (emit 0x0801C4FA) */
    0x08024D40,     /* location banner */
    0x08024EBC,     /* ScreenSprite */
    /* always-visible classes whose emitters write constant screen positions */
    0x08025748,     /* boss HP bar, 2x 32x8 at (160,y),(192,y) (emit 0x0801055C) */
    0x080250D4,     /* 8x8 at (119,139) (emit 0x08013112) */
    0x08025DC8,     /* 16x32 at (4,25) (emit 0x08019E4C) */
    0x08025E84,     /* 32x8 at (32,4) (emit 0x08019D34) */
    0x08025B2C,     /* cursor from a fixed position table (emit 0x08019F18) */
};

/* Persistent HUD pieces anchor on their own: they never join another
 * object's cluster by touching it.  A dialogue window at the top overlaps
 * the HUD strip; joining them dragged the HUD 32 px to the centre while the
 * window opened and closed.  The cost: the window, centred, now covers only
 * part of the HUD that it hides completely on the GBA. */
static const uint32_t hud_vts[] = {
    0x0802408C, 0x0802527C, 0x08025748, 0x08025DC8, 0x08025E84,
};

static int is_hud(uint32_t vt) {
    for (unsigned i = 0; i < sizeof hud_vts / sizeof hud_vts[0]; i++)
        if (vt == hud_vts[i]) return 1;
    return 0;
}

enum { CLS_NONE = 0, CLS_WORLD = 1, CLS_SCREEN = 2 };

static int classify(uint32_t vt, uint32_t cull) {
    cull &= ~1u;
    for (unsigned i = 0; i < sizeof world_culls / sizeof world_culls[0]; i++)
        if (cull == world_culls[i]) return CLS_WORLD;
    for (unsigned i = 0; i < sizeof screen_culls / sizeof screen_culls[0]; i++)
        if (cull == screen_culls[i]) return CLS_SCREEN;
    for (unsigned i = 0; i < sizeof screen_vts / sizeof screen_vts[0]; i++)
        if (vt == screen_vts[i]) return CLS_SCREEN;
    return CLS_WORLD;
}

typedef struct {
    uint32_t obj, vt, cull, emit;
    int32_t x, y;           /* exact screen position (when flagged) */
    uint16_t a0, a1;        /* attributes as emitted (validation) */
    uint8_t has_x, has_y, cls;
} side_t;
static side_t side_build[128], side_live[128];
/* slots ever written: everything above is zero in both arrays, so clears
 * and copies can stop there (they ran every frame over all 128) */
static int side_hwm;
static int live_valid;

#define MAXCAND 128
static int32_t cand_x[MAXCAND], cand_y[MAXCAND];
static int ncx, ncy;
static int in_emit;
static uint32_t emit_obj, emit_begin;
static int build_used;

static inline void add_x(int32_t v) { if (in_emit && ncx < MAXCAND) cand_x[ncx++] = v; }
static inline void add_y(int32_t v) { if (in_emit && ncy < MAXCAND) cand_y[ncy++] = v; }

void ws_px_r0(const uint32_t *r) { add_x((int32_t)r[0]); }
void ws_px_r1(const uint32_t *r) { add_x((int32_t)r[1]); }
void ws_px_r2(const uint32_t *r) { add_x((int32_t)r[2]); }
void ws_px_r3(const uint32_t *r) { add_x((int32_t)r[3]); }
void ws_px_r4(const uint32_t *r) { add_x((int32_t)r[4]); }
void ws_px_r5(const uint32_t *r) { add_x((int32_t)r[5]); }
void ws_px_r6(const uint32_t *r) { add_x((int32_t)r[6]); }
void ws_py_r0(const uint32_t *r) { add_y((int32_t)r[0]); }
void ws_py_r1(const uint32_t *r) { add_y((int32_t)r[1]); }
void ws_py_r2(const uint32_t *r) { add_y((int32_t)r[2]); }
void ws_py_r3(const uint32_t *r) { add_y((int32_t)r[3]); }
void ws_py_r4(const uint32_t *r) { add_y((int32_t)r[4]); }
void ws_py_r5(const uint32_t *r) { add_y((int32_t)r[5]); }
void ws_py_r6(const uint32_t *r) { add_y((int32_t)r[6]); }
/* sites that pack (y - 8) / (y - 16) / (y - 32) as y + 0xF8 / 0xF0 / 0xE0 */
void ws_py_r2_m256(const uint32_t *r) { add_y((int32_t)r[2] - 256); }
void ws_py_r3_m256(const uint32_t *r) { add_y((int32_t)r[3] - 256); }

/* 0x08006BCE: the build clears the shadow OAM */
void ws_hook_oam_clear(const uint32_t *regs) {
    (void)regs;
    memset(side_build, 0, (size_t)side_hwm * sizeof(side_t));
    build_used = 0;
}

/* OAM overflow guard.  0x08006BFA loads the emit method's offset from the
 * vtable (r1); with the wider view more objects pass the cull, so when fewer
 * than 16 shadow entries are left the emit is redirected to a guest function
 * returning 0 (the object is skipped this frame).  The shadow OAM ends at
 * 0x030026BC, right before another global. */
static uint32_t guard_vt, guard_sp;

void ws_hook_emit_guard(const uint32_t *regs) {
    guard_vt = regs[1];
    guard_sp = regs[13];
}

uint32_t ws_emit_guard(uint32_t v) {
    if (widen_active() && rd32(guard_sp) > SHADOW_END - 16 * 8) {
        st.emit_skipped++;
        return (FN_RETURN_0 | 1u) - guard_vt;
    }
    return v;
}

/* 0x08006BFE: about to call obj->emit(cursor); r0 = obj, sp = cursor */
void ws_hook_emit_begin(const uint32_t *regs) {
    emit_obj = regs[0];
    emit_begin = rd32(regs[13]);
    ncx = ncy = 0;
    in_emit = 1;
}

/* 0x08006C04: emit returned; assign the coordinates packed meanwhile to the
 * entries written, in order, by matching their low bits */
void ws_hook_emit_end(const uint32_t *regs) {
    uint32_t end = rd32(regs[13]);
    in_emit = 0;
    if (emit_begin < SHADOW_OAM || end > SHADOW_END || end <= emit_begin)
        return;
    uint32_t vt = g32(emit_obj);
    uint32_t cull = is_rom(vt, 0x14) ? vt + rd32(vt + 0xC) : 0;
    uint32_t emit = is_rom(vt, 0x14) ? vt + rd32(vt + 0x10) : 0;
    int cls = classify(vt, cull);
    int kx = 0, ky = 0;
    for (uint32_t a = emit_begin; a + 8 <= end; a += 8) {
        int slot = (int)((a - SHADOW_OAM) >> 3);
        side_t *s = &side_build[slot];
        uint32_t a0 = iw16(a), a1 = iw16(a + 2);
        s->obj = emit_obj;
        s->vt = vt;
        s->cull = cull;
        s->emit = emit;
        s->cls = (uint8_t)cls;
        s->a0 = (uint16_t)a0;
        s->a1 = (uint16_t)a1;
        s->has_x = s->has_y = 0;
        for (int k = kx; k < ncx; k++)
            if (((uint32_t)cand_x[k] & 0x1FF) == (a1 & 0x1FF)) {
                s->x = cand_x[k];
                s->has_x = 1;
                kx = k + 1;
                break;
            }
        for (int k = ky; k < ncy; k++)
            if (((uint32_t)cand_y[k] & 0xFF) == (a0 & 0xFF)) {
                s->y = cand_y[k];
                s->has_y = 1;
                ky = k + 1;
                break;
            }
        /* multi-entry objects often pack one coordinate once and reuse it
         * (a row of sprites shares y): reuse the last value matched */
        if (!s->has_x && kx > 0 && ((uint32_t)cand_x[kx - 1] & 0x1FF) == (a1 & 0x1FF)) {
            s->x = cand_x[kx - 1];
            s->has_x = 1;
        }
        if (!s->has_y && ky > 0 && ((uint32_t)cand_y[ky - 1] & 0xFF) == (a0 & 0xFF)) {
            s->y = cand_y[ky - 1];
            s->has_y = 1;
        }
        if (slot + 1 > build_used)
            build_used = slot + 1;
        if (slot + 1 > side_hwm)
            side_hwm = slot + 1;
    }
}

/* 0x08006AAE / 0x080070B0: the shadow OAM is copied to OAM */
void ws_hook_oam_upload(const uint32_t *regs) {
    (void)regs;
    memcpy(side_live, side_build, (size_t)side_hwm * sizeof(side_t));
    live_valid = 1;
    st.slots_used = build_used;
    if (build_used > st.slots_max)
        st.slots_max = build_used;
}

/* ------------------------------------------------------------ per-frame sprite positions */

static const uint8_t obj_dim[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

#define HIDDEN (-4096)
static int16_t pos_x[128], pos_y[128];
static uint8_t pos_ok[128];

static void compute_obj_positions(void) {
    int cls[128], bw[128], bh[128], gx[128], gy[128];
    uint32_t owner[128];
    uint8_t hud[128];
    int nscreen = 0, ex = 0, ey = 0;
    for (int i = 0; i < 128; i++) {
        pos_ok[i] = 0;
        cls[i] = CLS_NONE;
        uint32_t a0 = oam16(i * 8u), a1 = oam16(i * 8u + 2);
        int affine = (a0 & 0x100) != 0;
        if ((!affine && (a0 & 0x200)) || (a0 >> 14) == 3)
            continue;
        int w = obj_dim[a0 >> 14][a1 >> 14][0], h = obj_dim[a0 >> 14][a1 >> 14][1];
        if (affine && (a0 & 0x200)) { w *= 2; h *= 2; }
        bw[i] = w; bh[i] = h;
        int rx = (int)(a1 & 0x1FF), ry = (int)(a0 & 0xFF);
        const side_t *s = &side_live[i];
        int known = live_valid && s->obj && s->a0 == a0 && s->a1 == a1;
        int c = known ? s->cls : CLS_WORLD;
        int x, y;
        if (c == CLS_WORLD) {
            /* window decode around the view, unless the exact value is known */
            x = rx;
            while (x < OX - 64) x += 512;
            while (x >= OX + 448) x -= 512;
            y = ry;
            while (y < OY - 16) y += 256;
            while (y >= OY + 240) y -= 256;
        } else {
            /* screen-space: exactly the native placement */
            x = rx >= 256 ? rx - 512 : rx;
            y = ry + h > 256 ? ry - 256 : ry;
        }
        if (known && s->has_x && s->x > -2048 && s->x < 2048) { x = s->x; ex++; }
        if (known && s->has_y && s->y > -2048 && s->y < 2048) { y = s->y; ey++; }
        cls[i] = c;
        owner[i] = known ? s->obj : 0;
        hud[i] = known && is_hud(s->vt);
        gx[i] = x;
        gy[i] = y;
        pos_ok[i] = 1;
        if (c == CLS_WORLD) {
            pos_x[i] = (int16_t)x;
            pos_y[i] = (int16_t)y;
        } else {
            nscreen++;
        }
    }
    st.slots_exact_x = ex;
    st.slots_exact_y = ey;
    st.slots_screen = nscreen;
    if (!nscreen)
        return;

    /* Screen-space objects: hide what the GBA would not show, then group
     * touching boxes (and the entries of one object) into clusters and snap
     * each cluster to the nearest edge / centre of the 320x240 screen. */
    int idx[128], n = 0;
    for (int i = 0; i < 128; i++) {
        if (cls[i] != CLS_SCREEN) continue;
        if (gx[i] + bw[i] <= 0 || gx[i] >= 240 || gy[i] + bh[i] <= 0 || gy[i] >= 160) {
            pos_x[i] = HIDDEN;            /* parked off-screen */
            pos_y[i] = HIDDEN;
            continue;
        }
        idx[n++] = i;
    }
    int parent[128];                      /* union-find over idx[] positions */
    for (int k = 0; k < n; k++) parent[k] = k;
    for (int a = 0; a < n; a++) {
        int i = idx[a];
        for (int b = a + 1; b < n; b++) {
            int j = idx[b];
            int join = owner[i] && owner[i] == owner[j];
            if (!join && !hud[i] && !hud[j])
                join = gx[i] <= gx[j] + bw[j] + 2 && gx[j] <= gx[i] + bw[i] + 2 &&
                       gy[i] <= gy[j] + bh[j] + 2 && gy[j] <= gy[i] + bh[i] + 2;
            if (join) {
                int ra = a, rb = b;
                while (parent[ra] != ra) ra = parent[ra];
                while (parent[rb] != rb) rb = parent[rb];
                if (ra != rb) parent[rb] = ra;
            }
        }
    }
    int bx0[128], by0[128], bx1[128], by1[128], root[128];
    for (int k = 0; k < n; k++) {
        bx0[k] = by0[k] = 1 << 20;
        bx1[k] = by1[k] = -(1 << 20);
    }
    for (int k = 0; k < n; k++) {
        int r = k, i = idx[k];
        while (parent[r] != r) r = parent[r];
        root[k] = r;
        bx0[r] = imin(bx0[r], gx[i]); bx1[r] = imax(bx1[r], gx[i] + bw[i]);
        by0[r] = imin(by0[r], gy[i]); by1[r] = imax(by1[r], gy[i] + bh[i]);
    }
    for (int k = 0; k < n; k++) {
        int r = root[k], i = idx[k];
        /* the cluster box, clipped to the native screen, picks the anchor:
         * left / centre / right third, top / middle / bottom third */
        int x0 = imax(bx0[r], 0), x1 = imin(bx1[r], 240);
        int y0 = imax(by0[r], 0), y1 = imin(by1[r], 160);
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        int fx = cx < 80 ? ws_hud_inset : cx >= 160 ? 80 - ws_hud_inset : 40;
        int fy = cy < 54 ? ws_hud_inset : cy >= 107 ? 80 - ws_hud_inset : 40;
        /* framebuffer = native + f; screen coordinate = framebuffer - (-OX) */
        pos_x[i] = (int16_t)(gx[i] + fx + OX);
        pos_y[i] = (int16_t)(gy[i] + fy + OY);
    }
}

static int ws_obj_pos(int i, int *x, int *y) {
    if ((unsigned)i >= 128 || !pos_ok[i])
        return 0;
    *x = pos_x[i];
    *y = pos_y[i];
    return 1;
}

/* ------------------------------------------------------------ frame */

void ws_frame_begin(void) {
    vblank_count++;
    uint32_t p = iw32(CAM_PRESENTS);
    if (p != last_presents) frames_since_present = 0;
    else if (frames_since_present < 99) frames_since_present++;
    last_presents = p;

    field_on = ws_enabled && detect_field();
    st.field = field_on;
    if (!field_on) {
        for (int bg = 0; bg < 4; bg++) lay[bg].type = LAYER_NONE;
        OX = OY = 0;
        last_map_key = 0;
        return;
    }
    int L = (int)iw32(CAM_SHOWN_L), T = (int)iw32(CAM_SHOWN_T);
    int W = map_w(), H = map_h(), X0, Y0;
    /* a different map (or the field view coming back) invalidates margins
     * a renderer may have cached */
    uint32_t key = iw32(CAM_MAP) ^ ((uint32_t)W << 16) ^ (uint32_t)H;
    if (key != last_map_key || !last_map_key) {
        last_map_key = key ? key : 1;
        map_generation++;
    }
    view_rect(L, T, W, H, &X0, &Y0);
    OX = X0 - L;
    OY = Y0 - T;
    st.OX = OX; st.OY = OY; st.L = L; st.T = T; st.W = W; st.H = H;
    compute_obj_positions();
}

int ws_field_view_active(void) {
    return field_on;
}

uint32_t ws_map_generation(void) {
    return map_generation;
}

void ws_native_view(ws_view_t *v) {
    memset(v, 0, sizeof *v);
    v->ox = 40; v->oy = 40;
    v->x0 = 0; v->y0 = 0; v->x1 = 240; v->y1 = 160;
}

void ws_get_view(ws_view_t *v) {
    if (!field_on) {
        ws_native_view(v);
        return;
    }
    view_requested = vblank_count;
    v->ox = -OX;
    v->oy = -OY;
    v->x0 = OX;
    v->y0 = OY;
    v->x1 = OX + WS_W;
    v->y1 = OY + WS_H;
    v->bg_entry = ws_bg_entry;
    v->obj_pos = ws_obj_pos;
}

const ws_stats_t *ws_get_stats(void) {
    return &st;
}

/* ------------------------------------------------------------ diagnostics */

static uint16_t hw_entry(int bg, int px, int py) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * bg);
    uint32_t sb = ((cnt >> 8) & 31) * 0x800u;
    px &= 255; py &= 255;                   /* field BGs are 256x256 */
    uint32_t o = sb + (uint32_t)(((py >> 3) * 32 + (px >> 3)) * 2);
    return *(const uint16_t *)((const uint8_t *)gba_mem.vram + (o ^ GBA_HX));
}

int ws_debug_decode(uint32_t desc, uint8_t *out) {
    return decode_chunk(desc, out);
}

int ws_debug_check_bg(int *cells_checked) {
    int bad = 0, n = 0;
    if (!field_on) { if (cells_checked) *cells_checked = 0; return 0; }
    for (int bg = 0; bg < 4; bg++) {
        layer_t *l = &lay[bg];
        if (l->type != LAYER_FIELD) continue;
        int h = l->h;
        l->h = -100000;                      /* force the authored path */
        for (int y = 0; y < 160; y += 8)
            for (int x = 0; x < 240; x += 8) {
                int wx = h + x, wy = l->v + y;
                uint16_t e;
                if (!ws_bg_entry(bg, wx >> 3, wy >> 3, &e)) continue;
                if (wx < l->cx0 || wx >= l->cx1 || wy < l->cy0 || wy >= l->cy1) continue;
                n++;
                if (e != hw_entry(bg, wx, wy)) bad++;
            }
        l->h = h;
    }
    if (cells_checked) *cells_checked = n;
    return bad;
}

void ws_debug_dump_objs(void (*print)(const char *line)) {
    char buf[200];
    snprintf(buf, sizeof buf, "map desc %08X layers %08X/%d %08X/%d %08X/%d %08X/%d",
             (unsigned)iw32(CAM + 0x50),
             (unsigned)lay[0].res, lay[0].type, (unsigned)lay[1].res, lay[1].type,
             (unsigned)lay[2].res, lay[2].type, (unsigned)lay[3].res, lay[3].type);
    print(buf);
    for (int i = 0; i < 128; i++) {
        uint32_t a0 = oam16(i * 8u), a1 = oam16(i * 8u + 2);
        if (!(a0 & 0x100) && (a0 & 0x200)) continue;
        const side_t *s = &side_live[i];
        int known = live_valid && s->obj && s->a0 == a0 && s->a1 == a1;
        snprintf(buf, sizeof buf, "slot %3d raw(%3u,%3u) %s obj %08X vt %08X cull %08X emit %08X %s x%s=%d y%s=%d -> pos %d,%d",
                 i, (unsigned)(a1 & 0x1FF), (unsigned)(a0 & 0xFF), known ? "ok " : "-- ",
                 (unsigned)s->obj, (unsigned)s->vt, (unsigned)s->cull, (unsigned)s->emit,
                 s->cls == CLS_SCREEN ? "SCREEN" : "world ",
                 s->has_x ? "!" : "?", (int)s->x, s->has_y ? "!" : "?", (int)s->y,
                 pos_ok[i] ? pos_x[i] : -9999, pos_ok[i] ? pos_y[i] : -9999);
        print(buf);
    }
}

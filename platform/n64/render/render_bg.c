/*
 * render_bg.c - BG layers for the N64 GBA renderer.
 *
 * Text BGs use a CI8 layer cache per BG: a 512x256 ring (64x32 cells) in
 * world tile space.  World pixel X = BGxHOFS (all 16 bits) + screen x; cell
 * (cx,cy) = (X>>3, Y>>3) lives in ring slot (cx&63, cy&31), so a scroll is a
 * toroidal offset of the ring.  Every visible cell is checked each frame
 * against the slot's key (world cell, map entry, expansion frame) and the
 * per-32-byte change frame of its character data; only cells whose entry or
 * tile data changed are re-expanded (8 uncached 64-bit stores).  Without the
 * view's bg_entry hook the entry comes from the hardware map at
 * (cx & map_w-1, cy & map_h-1), which is exactly ppu_soft's (scroll & 0x1FF)
 * wrap; with the hook the world cell is passed on.
 *
 * Character data changes are found from gba_vram_take_dirty(): each dirty
 * 4KB page of BG VRAM is compared against a shadow copy in 32-byte chunks,
 * but only the chunks something depends on: the screen blocks of the text
 * layers and the tiles held by ring slots (reference counts per chunk).  A
 * chunk's shadow is refreshed when it gets its first reference, so it always
 * equals the data the ring was expanded from.
 *
 * Affine BGs (modes 1/2) and bitmap modes 3/4/5 are rasterised on the CPU
 * into a screen-sized buffer with ppu_soft's exact arithmetic (per-line
 * parameters from gba_line_regs[] when valid), then blitted.
 */
#include <string.h>
#include <malloc.h>
#include "render_internal.h"
#include "render_raw.h"

#define RING_W 512
#define RING_H 256
#define RING_CX 64
#define RING_CY 32
#define CHUNK_W 64
#define CHUNK_H 32

/* ring slot key: map entry << 16 | world cell high bits ((cy>>5)&0xFF)<<8 | (cx>>6)&0xFF */
static uint8_t *ring[4];                /* uncached */
static surface_t ring_surf[4];
/* frame: r_frame of the expansion (0 = empty) | SLOT_FULL when the expanded
 * tile has a non-zero (opaque) pixel | SLOT_SOLID when all its pixels are
 * (r_frame stays below 2^30 for 200 days of 60 fps) */
typedef struct { uint32_t key, frame; } slot_t;
#define SLOT_FULL  0x80000000u
#define SLOT_SOLID 0x40000000u
#define SLOT_FRAME 0x3FFFFFFFu
static slot_t slot[4][RING_CX * RING_CY];
static uint8_t touched[4][RING_CX * RING_CY / 8];   /* slots drawn from in this frame */
static uint8_t touched_any[4];          /* layer has bits set in touched (cleared lazily) */
static uint16_t tile_use[4][1024];      /* number of ring slots holding each tile */
/* Transparent chunks are not drawn: full slots (SLOT_FULL) are counted per
 * 64x32 ring chunk.  The flags of the slots in the view are always current
 * (they are checked before every draw), so a chunk with no full slot has
 * nothing to show. */
static uint8_t chunk_full[4][(RING_CX / 8) * (RING_CY / 4)];
static uint64_t chunk_opaque[4];        /* bit k: chunk_full[n][k] != 0 */
/* the SLOT_FULL / SLOT_SOLID flags again, one word per ring row (bit = slot
 * column), for the blend path's cell masks */
static uint64_t row_full[4][RING_CY], row_solid[4][RING_CY];
/* lowest << 8 | highest non-zero palette index of each slot's tile (BG
 * palette), 0 = not known yet (found when needed, see bg_text_colour_max) */
static uint16_t slot_rng[4][RING_CX * RING_CY];
/* Results of bg_text_cells / bg_text_colour_max kept for the next frames:
 * valid while the view (single scroll band) and the layer's slot flags
 * (mask_gen) or slot contents (rng_gen) stay the same */
typedef struct { int ok, h, v, x0, y0, x1, y1; uint32_t gen; } vkey_t;
static uint32_t mask_gen[4], rng_gen[4];
static vkey_t mkey[4], rkey[4];
static cmask_t mc_touch[4], mc_solid[4];
static uint32_t rc_rng[4];

/* per-layer change tracking */
typedef struct {
    uint32_t key;               /* char base, colour depth, map size/base, hook */
    int valid;                  /* window below describes the ring contents */
    int cx0, cy0, cx1, cy1;     /* last frame's visible cell window (inclusive) */
    uint32_t frame;             /* r_frame of the last update */
    /* arguments of the last update: the same update again in the same frame
     * (a layer drawn in several passes) has nothing to do */
    int ux0, ux1, uy0, uy1, uinc, ukey_hook;
    uint32_t ucnt;
} lstate_t;
static lstate_t ls[4];

static uint32_t *bg_shadow;             /* cached copy of BG VRAM (64KB) */
static uint32_t chunk_frame[0x10000 / 32];
static uint16_t chunk_refs[0x10000 / 32];   /* ring slots + layer maps depending on each chunk */
static uint16_t changed[0x10000 / 32];  /* chunks that changed this frame */
static int nchanged;
static uint16_t lut4[256];              /* 4bpp byte -> two pixels, left pixel in the high byte */
static uint16_t lut4m[256];             /* mask of non-zero pixels */
#ifdef GBA_RENDER_TRACE
uint32_t bg_trace_grp[4];
uint32_t bg_trace_cmp;
#endif

void bg_init(void) {
    for (int n = 0; n < 4; n++) {
        ring[n] = malloc_uncached_aligned(64, RING_W * RING_H);
        memset(ring[n], 0, RING_W * RING_H);
        ring_surf[n] = surface_make(ring[n], FMT_CI8, RING_W, RING_H, RING_W);
    }
    /* bg_track reads VRAM and the shadow at the same offsets: keep the
     * shadow 4KB away from VRAM in the 8KB direct-mapped D-cache, so a chunk
     * and its shadow never evict each other */
    uint8_t *sh = memalign(16, 0x10000 + 0x2000);
    uint32_t want = ((uint32_t)(uintptr_t)gba_mem.vram + 0x1000u) & 0x1FF0u;
    bg_shadow = (uint32_t *)(sh + ((want - ((uint32_t)(uintptr_t)sh & 0x1FF0u)) & 0x1FF0u));
    for (int b = 0; b < 256; b++) {
        int lo = b & 15, hi = b >> 4;           /* GBA: low nibble = left pixel */
        lut4[b] = (uint16_t)((lo << 8) | hi);
        lut4m[b] = (uint16_t)((lo ? 0xFF00 : 0) | (hi ? 0x00FF : 0));
    }
    bg_invalidate();
}

static inline void chunk_ref(uint32_t c) {
    if (chunk_refs[c]++ == 0) {
        /* tracking starts: the shadow takes the current contents (the data
         * any expansion from now on reads) */
        const uint64_t *v = (const uint64_t *)gba_mem.vram + c * 4;
        uint64_t *sh = (uint64_t *)bg_shadow + c * 4;
        sh[0] = v[0]; sh[1] = v[1]; sh[2] = v[2]; sh[3] = v[3];
    }
}

/* chunks of tile t for char base cb: 1 (4bpp) or 2 (8bpp), none past BG VRAM */
static inline void tile_ref(uint32_t cb, int bpp8, uint32_t t) {
    uint32_t a = cb + (t << (bpp8 ? 6 : 5));
    if (a >= 0x10000u) return;
    chunk_ref(a >> 5);
    if (bpp8) chunk_ref((a >> 5) + 1);
}

static inline void tile_unref(uint32_t cb, int bpp8, uint32_t t, uint32_t count) {
    uint32_t a = cb + (t << (bpp8 ? 6 : 5));
    if (a >= 0x10000u) return;
    chunk_refs[a >> 5] -= (uint16_t)count;
    if (bpp8) chunk_refs[(a >> 5) + 1] -= (uint16_t)count;
}

/* the screen blocks of a layer key (as far as they lie in BG VRAM) */
static void map_range(uint32_t key, uint32_t *c0, uint32_t *c1) {
    uint32_t sb = ((key >> 8) & 31) * 0x800u, size = (key >> 14) & 3;
    uint32_t end = sb + (size == 0 ? 1u : size == 3 ? 4u : 2u) * 0x800u;
    if (end > 0x10000u) end = 0x10000u;
    *c0 = sb >> 5; *c1 = end >> 5;
}

/* drop every chunk reference of layer n (its slots and its map) */
static void layer_release(int n) {
    uint32_t key = ls[n].key;
    if (key == 0xFFFFFFFFu) return;
    uint32_t cb = ((key >> 2) & 3) * 0x4000u;
    int bpp8 = (key & 0x80) != 0;
    for (uint32_t t = 0; t < 1024; t++)
        if (tile_use[n][t]) tile_unref(cb, bpp8, t, tile_use[n][t]);
    uint32_t c0, c1;
    map_range(key, &c0, &c1);
    for (uint32_t c = c0; c < c1; c++) chunk_refs[c]--;
}

static void layer_reset(int n) {
    memset(slot[n], 0, sizeof slot[n]);
    memset(tile_use[n], 0, sizeof tile_use[n]);
    memset(chunk_full[n], 0, sizeof chunk_full[n]);
    chunk_opaque[n] = 0;
    memset(row_full[n], 0, sizeof row_full[n]);
    memset(row_solid[n], 0, sizeof row_solid[n]);
    mask_gen[n]++;
    rng_gen[n]++;
    ls[n].valid = 0;
}

void bg_invalidate_layer(int n) {
    ls[n].valid = 0;        /* next frame re-checks every cell in view */
}

void bg_invalidate(void) {
    for (int n = 0; n < 4; n++) {
        layer_reset(n);
        ls[n].key = 0xFFFFFFFFu;
    }
    memset(chunk_refs, 0, sizeof chunk_refs);
    memcpy(bg_shadow, gba_mem.vram, 0x10000);
    memset(chunk_frame, 0, sizeof chunk_frame);
    nchanged = 0;
}

void bg_track(uint32_t pages) {
    nchanged = 0;
#ifdef GBA_RENDER_TRACE
    bg_trace_cmp = 0;
#endif
    for (int n = 0; n < 4; n++)
        if (touched_any[n]) {
            memset(touched[n], 0, sizeof touched[n]);
            touched_any[n] = 0;
        }
    while (pages) {
        int p = r_ctz64(pages);
        pages &= pages - 1;
        const uint64_t *v = (const uint64_t *)(gba_mem.vram + p * 1024);
        uint64_t *s = (uint64_t *)(bg_shadow + p * 1024);
        const uint16_t *refs = &chunk_refs[p * 128];
        for (int c = 0; c < 128; c++, v += 4, s += 4) {
            if (!refs[c]) continue;             /* nothing depends on it */
#ifdef GBA_RENDER_TRACE
            bg_trace_cmp++;
#endif
            uint64_t v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];
            if (((v0 ^ s[0]) | (v1 ^ s[1]) | (v2 ^ s[2]) | (v3 ^ s[3])) == 0)
                continue;
            s[0] = v0; s[1] = v1; s[2] = v2; s[3] = v3;
            chunk_frame[p * 128 + c] = r_frame;
            changed[nchanged++] = (uint16_t)(p * 128 + c);
        }
    }
    r_stats.chunks_changed = (uint32_t)nchanged;
#ifdef GBA_RENDER_TRACE
    memset(bg_trace_grp, 0, sizeof bg_trace_grp);
    for (int i = 0; i < nchanged; i++)
        bg_trace_grp[changed[i] >> 9]++;
#endif
}

/* ------------------------------------------------------------ text BG */

/* lowest << 8 | highest non-zero palette index among 'n' values of 'bits' bits
 * packed in the words w[] (+ 'base'); 0xFF00 if none */
static uint32_t index_range(const void *w, int nbytes, int bits, uint32_t base) {
    const uint8_t *b = (const uint8_t *)w;
    uint32_t lo = 0xFF, hi = 0;
    for (int i = 0; i < nbytes; i++) {
        uint32_t v = b[i];
        if (bits == 8) {
            if (v) { if (v < lo) lo = v; if (v > hi) hi = v; }
        } else {
            for (int k = 0; k < 2; k++, v >>= 4)
                if (v & 15) {
                    uint32_t x = base + (v & 15);
                    if (x < lo) lo = x;
                    if (x > hi) hi = x;
                }
        }
    }
    return (lo << 8) | hi;
}

/* expand a cell into its ring slot; returns SLOT_FULL if it has a non-zero
 * pixel, | SLOT_SOLID if all of its pixels are */
static uint32_t expand_cell(int n, int si, uint32_t entry, uint32_t cb, int bpp8) {
    uint64_t *dst = (uint64_t *)(ring[n] + (si >> 6) * 8 * RING_W + (si & 63) * 8);
    uint64_t any = 0;
    const int stride = RING_W / 8;
    uint32_t tile = entry & 0x3FF;
    int hf = (entry & 0x400) != 0, vf = (entry & 0x800) != 0;
    if (bpp8) {
        uint32_t addr = cb + tile * 64;
        if (addr >= 0x10000u) {
            for (int r = 0; r < 8; r++) dst[r * stride] = 0;
            return 0;
        }
        /* a row is one native doubleword [w0][w1] holding pixels 3..0, 7..4 */
        const uint64_t *src = (const uint64_t *)(gba_mem.vram + (addr >> 2));
        const uint64_t ones = 0x0101010101010101ull, highs = 0x8080808080808080ull;
        uint64_t zero = 0;          /* high bit set in a byte lane: some row has a zero pixel there */
        if (vf) src += 7;
        int step = vf ? -1 : 1;
        if (hf) {   /* right-to-left = the two words swapped */
            for (int r = 0; r < 8; r++, src += step) {
                uint64_t x = *src;
                any |= x;
                zero |= (x - ones) & ~x & highs;
                dst[r * stride] = (x << 32) | (x >> 32);
            }
        } else {    /* byte-reverse each word */
            const uint64_t m8 = 0x00FF00FF00FF00FFull, m16 = 0x0000FFFF0000FFFFull;
            for (int r = 0; r < 8; r++, src += step) {
                uint64_t x = *src;
                any |= x;
                zero |= (x - ones) & ~x & highs;
                x = ((x & m8) << 8) | ((x >> 8) & m8);
                dst[r * stride] = ((x & m16) << 16) | ((x >> 16) & m16);
            }
        }
        return (any ? SLOT_FULL : 0) | (zero ? 0 : SLOT_SOLID);
    } else {
        uint32_t addr = cb + tile * 32;
        if (addr >= 0x10000u) {
            for (int r = 0; r < 8; r++) dst[r * stride] = 0;
            return 0;
        }
        const uint32_t *src = gba_mem.vram + (addr >> 2);
        uint64_t bank = 0x0101010101010101ull * (uint64_t)((entry >> 12) << 4);
        uint64_t allm = ~0ull;      /* pixels non-zero in every row */
        for (int r = 0; r < 8; r++) {
            int sr = vf ? 7 - r : r;
            uint32_t w = src[sr];       /* guest LE word: byte k = pixels 2k (low nib), 2k+1 */
            any |= w;
            uint32_t b0 = w & 255, b1 = (w >> 8) & 255, b2 = (w >> 16) & 255, b3 = w >> 24;
            uint64_t v, m;
            if (!hf) {
                v = ((uint64_t)lut4[b0] << 48) | ((uint64_t)lut4[b1] << 32) | ((uint64_t)lut4[b2] << 16) | lut4[b3];
                m = ((uint64_t)lut4m[b0] << 48) | ((uint64_t)lut4m[b1] << 32) | ((uint64_t)lut4m[b2] << 16) | lut4m[b3];
            } else {
                /* reversed: pixel 7 first = high nibble of byte 3 */
                #define SW(x) ((uint64_t)(((x) >> 8) | (((x) & 0xFF) << 8)) & 0xFFFF)
                v = (SW(lut4[b3]) << 48) | (SW(lut4[b2]) << 32) | (SW(lut4[b1]) << 16) | SW(lut4[b0]);
                m = (SW(lut4m[b3]) << 48) | (SW(lut4m[b2]) << 32) | (SW(lut4m[b1]) << 16) | SW(lut4m[b0]);
                #undef SW
            }
            dst[r * stride] = v | (bank & m);
            allm &= m;
        }
        return (any ? SLOT_FULL : 0) | (~allm ? 0 : SLOT_SOLID);
    }
}

/* layer parameters for the current frame */
static struct {
    int n, bpp8, wide, map_ok, tshift;
    uint32_t cb, sb, size, nblk, wmask, hmask;
    int (*hook)(int, int, int, uint16_t *);
} L;

static inline int cell_stale(int si, uint32_t entry) {
    uint32_t a = L.cb + ((entry & 0x3FF) << L.tshift);
    if (a >= 0x10000u) return 0;
    uint32_t c = a >> 5, f = slot[L.n][si].frame & SLOT_FRAME;
    return chunk_frame[c] > f || (L.bpp8 && chunk_frame[c + 1] > f);
}

static void refill(int si, uint32_t key) {
    uint8_t *t = &touched[L.n][si >> 3];
    uint8_t bit = (uint8_t)(1u << (si & 7));
    /* the RDP may still have to draw this slot for an earlier band: submit the
     * raw commands that read it and wait for them */
    if (*t & bit) {
        raw_flush();
        rspq_wait();
    }
    *t |= bit;
    touched_any[L.n] = 1;
    slot_t *sl = &slot[L.n][si];
    uint32_t t_old = (sl->key >> 16) & 0x3FF, t_new = (key >> 16) & 0x3FF;
    if (!sl->frame || t_old != t_new) {
        if (sl->frame) {
            tile_use[L.n][t_old]--;
            tile_unref(L.cb, L.bpp8, t_old, 1);
        }
        tile_use[L.n][t_new]++;
        tile_ref(L.cb, L.bpp8, t_new);
    }
    uint32_t flags = expand_cell(L.n, si, key >> 16, L.cb, L.bpp8);
    uint32_t full = flags & SLOT_FULL;
    if (full != (sl->frame & SLOT_FULL)) {
        /* opaque slots per chunk (and the chunk's bit) follow the change */
        int ci = ((si >> 8) << 3) | ((si & 63) >> 3);
        uint8_t *cf = &chunk_full[L.n][ci];
        *cf = (uint8_t)(*cf + (full ? 1 : -1));
        if (*cf) chunk_opaque[L.n] |= 1ull << ci;
        else chunk_opaque[L.n] &= ~(1ull << ci);
    }
    if ((flags ^ sl->frame) & (SLOT_FULL | SLOT_SOLID))
        mask_gen[L.n]++;
    rng_gen[L.n]++;
    sl->key = key;
    sl->frame = r_frame | flags;
    slot_rng[L.n][si] = 0;
    uint64_t cbit = 1ull << (si & (RING_CX - 1));
    int row = si >> 6;
    row_full[L.n][row] = (flags & SLOT_FULL) ? row_full[L.n][row] | cbit : row_full[L.n][row] & ~cbit;
    row_solid[L.n][row] = (flags & SLOT_SOLID) ? row_solid[L.n][row] | cbit : row_solid[L.n][row] & ~cbit;
    r_stats.cells_expanded++;
}

/* bring world cell (cx,cy) up to date in the ring */
static void check_cell(int cx, int cy) {
    uint32_t entry;
    uint16_t e16;
    if (L.hook && L.hook(L.n, cx, cy, &e16)) {
        entry = e16;
    } else {
        uint32_t xc = (uint32_t)cx & L.wmask, yc = (uint32_t)cy & L.hmask;
        uint32_t a = L.sb + ((((yc >> 5) << L.wide) + (xc >> 5)) << 11) + ((yc & 31) << 6) + ((xc & 31) << 1);
        if (!L.map_ok) a = r_vram_wrap(a);
        entry = r_vram16(a);
    }
    int si = ((cy & (RING_CY - 1)) << 6) | (cx & (RING_CX - 1));
    uint32_t key = (entry << 16) | (((uint32_t)(cy >> 5) & 0xFF) << 8) | ((uint32_t)(cx >> 6) & 0xFF);
    r_stats.cells_checked++;
    const slot_t *sl = &slot[L.n][si];
    if (sl->key == key && sl->frame && !cell_stale(si, entry)) {
        touched[L.n][si >> 3] |= (uint8_t)(1u << (si & 7));
        touched_any[L.n] = 1;
        return;
    }
    refill(si, key);
}

static void check_rect(int cx0, int cy0, int cx1, int cy1) {
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++)
            check_cell(cx, cy);
}

/* What changed this frame for this layer: bit 0 = map entries in its screen
 * blocks, bit 1 = character data of a tile the ring holds (tbits marks them). */
static int layer_changes(uint8_t tbits[128]) {
    uint32_t m0 = L.sb >> 5, m1 = (L.sb + L.nblk * 0x800u) >> 5;
    int ch = 0;
    for (int i = 0; i < nchanged; i++) {
        uint32_t c = changed[i];
        if (c >= m0 && c < m1) ch |= 1;
        uint32_t a = c * 32u;
        if (a < L.cb) continue;
        uint32_t t = (a - L.cb) >> L.tshift;
        if (t < 1024 && tile_use[L.n][t]) {
            if (!(ch & 2)) memset(tbits, 0, 128);
            tbits[t >> 3] |= (uint8_t)(1u << (t & 7));
            ch |= 2;
        }
    }
    return ch;
}

/* map entries that changed this frame -> world cells inside the window */
static void check_map_changes(int cx0, int cy0, int cx1, int cy1) {
    uint32_t mw = L.wmask + 1, mh = L.hmask + 1;
    for (int i = 0; i < nchanged; i++) {
        uint32_t a = changed[i] * 32u;
        if (a < L.sb || a >= L.sb + L.nblk * 0x800u) continue;
        uint32_t off = a - L.sb, blk = off >> 11, idx = (off & 0x7FF) >> 1;
        uint32_t bx = L.wide ? (blk & 1) : 0;
        uint32_t by = L.size == 3 ? (blk >> 1) : L.size == 2 ? blk : 0;
        uint32_t hy = by * 32 + (idx >> 5);
        for (uint32_t k = 0; k < 16; k++) {
            uint32_t hx = bx * 32 + ((idx + k) & 31);
            for (int cy = cy0 + (int)((hy - (uint32_t)cy0) & (mh - 1)); cy <= cy1; cy += (int)mh)
                for (int cx = cx0 + (int)((hx - (uint32_t)cx0) & (mw - 1)); cx <= cx1; cx += (int)mw)
                    check_cell(cx, cy);
        }
    }
}

/* re-expand window cells whose tile data changed (their map entries are current) */
static void rescan_tiles(int cx0, int cy0, int cx1, int cy1, const uint8_t tbits[128]) {
    const slot_t *sl = slot[L.n];
    for (int cy = cy0; cy <= cy1; cy++) {
        int row = (cy & (RING_CY - 1)) << 6;
        for (int cx = cx0; cx <= cx1; cx++) {
            int si = row | (cx & (RING_CX - 1));
            uint32_t t = (sl[si].key >> 16) & 0x3FF;
            if ((tbits[t >> 3] >> (t & 7)) & 1 && (sl[si].frame & SLOT_FRAME) != r_frame)
                refill(si, sl[si].key);
        }
    }
}

/* make sure every cell covering world rect [X0,X1) x [Y0,Y1) is in the ring */
static void update_layer(int n, uint32_t cnt, int X0, int X1, int Y0, int Y1, int incremental) {
    L.n = n;
    L.cb = ((cnt >> 2) & 3) * 0x4000u;
    L.sb = ((cnt >> 8) & 31) * 0x800u;
    L.size = (cnt >> 14) & 3;
    L.nblk = L.size == 0 ? 1 : L.size == 3 ? 4 : 2;
    L.wide = L.size & 1;
    L.wmask = L.wide ? 63 : 31;
    L.hmask = (L.size & 2) ? 63 : 31;
    L.bpp8 = (cnt & 0x80) != 0;
    L.tshift = L.bpp8 ? 6 : 5;
    L.map_ok = L.sb + L.nblk * 0x800u <= 0x10000u;
    L.hook = r_view->bg_entry;
    uint32_t key = (cnt & 0xDF8C) | (L.hook ? 0x10000u : 0);
    lstate_t *st = &ls[n];
    if (st->frame == r_frame && key == st->key && st->ucnt == cnt && st->ux0 == X0 && st->ux1 == X1 &&
        st->uy0 == Y0 && st->uy1 == Y1 && st->uinc == incremental)
        return;
    st->ucnt = cnt; st->ux0 = X0; st->ux1 = X1; st->uy0 = Y0; st->uy1 = Y1; st->uinc = incremental;
    st->ukey_hook = L.hook != NULL;
    if (key != st->key) {
        layer_release(n);
        layer_reset(n);
        st->key = key;
        uint32_t c0, c1;
        map_range(key, &c0, &c1);
        for (uint32_t c = c0; c < c1; c++) chunk_ref(c);
    }
    /* The incremental update only sees this frame's VRAM changes: it needs the
     * layer to have been updated in the previous frame too (not disabled,
     * forced blank, or absent from the BG mode in between). */
    if (st->frame != r_frame && st->frame + 1 != r_frame)
        st->valid = 0;
    st->frame = r_frame;
    int cx0 = X0 >> 3, cx1 = (X1 - 1) >> 3, cy0 = Y0 >> 3, cy1 = (Y1 - 1) >> 3;
    /* Maps reaching into OBJ VRAM are not change-tracked.  With a bg_entry
     * hook, world cells are re-queried when they enter the window or when
     * their hardware map cell changes; a hook whose data changes otherwise
     * must call gba_render_invalidate_bg(). */
    int ox0 = st->cx0 > cx0 ? st->cx0 : cx0, ox1 = st->cx1 < cx1 ? st->cx1 : cx1;
    int oy0 = st->cy0 > cy0 ? st->cy0 : cy0, oy1 = st->cy1 < cy1 ? st->cy1 : cy1;
    if (!incremental || !st->valid || !L.map_ok || ox0 > ox1 || oy0 > oy1) {
        check_rect(cx0, cy0, cx1, cy1);
    } else {
        /* newly exposed cells: the window minus last frame's window */
        if (cy0 < oy0) check_rect(cx0, cy0, cx1, oy0 - 1);
        if (cy1 > oy1) check_rect(cx0, oy1 + 1, cx1, cy1);
        if (cx0 < ox0) check_rect(cx0, oy0, ox0 - 1, oy1);
        if (cx1 > ox1) check_rect(ox1 + 1, oy0, cx1, oy1);
        uint8_t tbits[128];
        int ch = layer_changes(tbits);
        if (ch & 1)
            check_map_changes(ox0, oy0, ox1, oy1);
        if (ch & 2)
            rescan_tiles(cx0, cy0, cx1, cy1, tbits);
    }
    st->cx0 = cx0; st->cy0 = cy0; st->cx1 = cx1; st->cy1 = cy1;
    st->valid = incremental;
}

static void draw_ring(int n, int h, int v, int gy0, int gy1) {
    r_scissor(R.wx0, gy0, R.wx1, gy1);
    /* tile 1 loads a chunk, tile 0 draws it with chunk-relative texel coordinates */
    raw_reserve(7);
    raw_put(RAW_SYNC_PIPE);
    raw_put(raw_tex_image(RF_CI, RS_8, RING_W, PhysicalAddr(ring[n])));
    raw_put(RAW_SYNC_TILE);
    raw_put(raw_set_tile(RF_CI, RS_8, CHUNK_W, 0, 1, 0));
    raw_put(raw_set_tile(RF_CI, RS_8, CHUNK_W, 0, 0, 0));
    raw_put(raw_set_tile_size(0, 0, 0, CHUNK_W, CHUNK_H));
    raw_owner = RAW_OWNER_NONE;
    /* r_rect specialised for the chunks: the band [gy0, gy1) lies inside the
     * draw window and only its rows are loaded, so the rectangles are clipped
     * at the left and right edges only; s starts at the clipped columns, t = 0 */
    const int copy = r_clip.copy, ty = r_clip.ty;
    const int cx0 = r_clip.x0, cx1 = r_clip.x1;
    const uint64_t w1 = ((uint64_t)(copy ? 0x1000 : 0x400) << 16) | 0x400;
    int rx = (h + R.wx0) & (RING_W - 1), ry = (v + gy0) & (RING_H - 1);
    int xs = R.wx0 + r_clip.tx - (rx & (CHUNK_W - 1)), lx0 = rx & ~(CHUNK_W - 1);
    int gy = gy0 - (ry & (CHUNK_H - 1)), ly = ry & ~(CHUNK_H - 1);
    const uint64_t om = chunk_opaque[n];
    /* commands go straight into the raw buffer through a local pointer; the
     * inner loop touches no other memory (its state stays in registers) */
    uint64_t *p = raw_ptr, *pend = raw_end - (copy ? 5 + 4 : 3 + 2 * 16 + 4);
    uint32_t px = 0, rects = 0, lbytes = 0, loads = 0;
    if (!copy) {
        /* 1-cycle (the 32-bit blend path, where every pixel counts): a
         * rectangle per run of slots with an opaque pixel in each row of
         * slots (rows with the same runs stacked), one load per chunk */
        for (; gy < gy1; gy += CHUNK_H, ly = (ly + CHUNK_H) & (RING_H - 1)) {
            int t0 = gy < gy0 ? gy0 - gy : 0;
            int t1 = gy + CHUNK_H > gy1 ? gy1 - gy : CHUNK_H;
            int y = gy + t0 + ty;
            uint64_t ldw = raw_load_tile(1, 0, ly + t0, CHUNK_W, ly + t1);
            uint32_t opaque = (uint32_t)(om >> ((ly >> 5) << 3)) & 0xFF;
            int lx = lx0;
            for (int x = xs; x < cx1; x += CHUNK_W, lx = (lx + CHUNK_W) & (RING_W - 1)) {
                int xa = x > cx0 ? x : cx0, xb = x + CHUNK_W < cx1 ? x + CHUNK_W : cx1;
                if (xa >= xb || !((opaque >> (lx >> 6)) & 1))
                    continue;
                if (__builtin_expect(p > pend, 0)) {
                    raw_ptr = p;
                    raw_overflow();
                    p = raw_ptr;
                }
                RAW_HWCHECK("LOAD_TILE", lx, ly + t0);
                p[0] = RAW_SYNC_TILE;
                p[1] = RAW_SYNC_LOAD;
                p[2] = ldw + ((uint64_t)(lx * 4) << 44) + ((uint64_t)(lx * 4) << 12);
                p += 3;
                loads++;
                lbytes += (uint32_t)(CHUNK_W * (t1 - t0));
                for (int r = t0, r1; r < t1; r = r1) {
                    uint32_t bits = (uint32_t)(row_full[n][((ly + r) >> 3) & (RING_CY - 1)] >> (lx >> 3)) & 0xFF;
                    r1 = ((r >> 3) + 1) << 3;
                    if (r1 > t1) r1 = t1;
                    while (r1 < t1 && ((uint32_t)(row_full[n][((ly + r1) >> 3) & (RING_CY - 1)] >> (lx >> 3)) & 0xFF) == bits)
                        r1 = r1 + 8 < t1 ? r1 + 8 : t1;
                    uint64_t yw = ((uint64_t)((y + r1 - t0) * 4) << 32) | (uint64_t)((y + r - t0) * 4);
                    while (bits) {
                        int c0 = r_ctz64(bits);
                        int len = r_ctz64(~(uint64_t)(bits >> c0));
                        bits &= ~(((1u << len) - 1) << c0);
                        int rx0 = x + c0 * 8, rx1 = rx0 + len * 8;
                        if (rx0 < xa) rx0 = xa;
                        if (rx1 > xb) rx1 = xb;
                        if (rx0 >= rx1) continue;
                        p[0] = (0x24ull << 56) | ((uint64_t)(rx1 * 4) << 44) | yw | ((uint64_t)(rx0 * 4) << 12);
                        p[1] = ((uint64_t)((rx0 - x) * 32) << 48) | ((uint64_t)((r - t0) * 32) << 32) | w1;
                        p += 2;
                        rects++;
                        px += (uint32_t)((rx1 - rx0) * (r1 - r));
                    }
                }
            }
        }
        raw_ptr = p;
        r_stats.loads += loads; r_stats.load_bytes += lbytes;
        r_rect_stats(&r_clip, rects, px);
        return;
    }
    for (; gy < gy1; gy += CHUNK_H, ly = (ly + CHUNK_H) & (RING_H - 1)) {
        /* load only the rows of the chunk that are inside the band */
        int t0 = gy < gy0 ? gy0 - gy : 0;
        int t1 = gy + CHUNK_H > gy1 ? gy1 - gy : CHUNK_H;
        int y = gy + t0 + ty, hh = t1 - t0;
        uint64_t yw = ((uint64_t)((y + hh - copy) * 4) << 32) | (uint64_t)(y * 4);
        uint64_t ldw = raw_load_tile(1, 0, ly + t0, CHUNK_W, ly + t1);   /* + the chunk's columns */
        /* the chunks of this ring row that hold an opaque slot, one bit per column */
        uint32_t opaque = (uint32_t)(om >> ((ly >> 5) << 3)) & 0xFF;
        uint32_t wsum = 0, nch = 0;
        int lx = lx0;
        for (int x = xs; x < cx1; x += CHUNK_W, lx = (lx + CHUNK_W) & (RING_W - 1)) {
            int xa = x, w = CHUNK_W, sc = 0;
            if (xa < cx0) { sc = cx0 - xa; w -= sc; xa = cx0; }
            if (xa + w > cx1) w = cx1 - xa;
            if (w <= 0 || !((opaque >> (lx >> 6)) & 1))
                continue;                       /* (transparent: nothing to draw) */
            if (__builtin_expect(p > pend, 0)) {
                raw_ptr = p;
                raw_overflow();
                p = raw_ptr;
            }
            RAW_HWCHECK("LOAD_TILE", lx, ly + t0);
            p[0] = RAW_SYNC_TILE;
            p[1] = RAW_SYNC_LOAD;
            p[2] = ldw + ((uint64_t)(lx * 4) << 44) + ((uint64_t)(lx * 4) << 12);
            p[3] = (0x24ull << 56) | ((uint64_t)((xa + w - copy) * 4) << 44) | yw | ((uint64_t)(xa * 4) << 12);
            p[4] = ((uint64_t)(sc * 32) << 48) | w1;
            p += 5;
            wsum += (uint32_t)w;
            nch++;
        }
        px += wsum * (uint32_t)hh;
        rects += nch;
        lbytes += nch * (uint32_t)(CHUNK_W * hh);
    }
    raw_ptr = p;
    r_stats.loads += rects; r_stats.load_bytes += lbytes;
    r_rect_stats(&r_clip, rects, px);
}

static void scroll_of(int n, int line, int *h, int *v) {
    if (gba_line_regs_valid && line >= 0 && line < 160) {
        *h = gba_line_regs[line].hofs[n];
        *v = gba_line_regs[line].vofs[n];
    } else {
        *h = io16(REG_BG0HOFS + 4 * n);
        *v = io16(REG_BG0HOFS + 4 * n + 2);
    }
}

/* Bands of lines with the same scroll (HBlank snapshots may differ per line)
 * over the whole region: the layer cache is updated for each band as in the
 * first pass of the frame (an update with the same arguments again is free),
 * whatever part of it is drawn. */
static int scan_band(int n, int y, int *h, int *v) {
    scroll_of(n, y, h, v);
    int y1 = R.gy1;
    if (gba_line_regs_valid) {
        y1 = y + 1;
        while (y1 < R.gy1) {
            int h2, v2;
            scroll_of(n, y1, &h2, &v2);
            if (h2 != *h || v2 != *v) break;
            y1++;
        }
    }
    return y1;
}

/* the bands of each layer, found once per frame (a layer is drawn in several
 * passes and rectangles); more than BANDS_MAX: scanned every time */
#define BANDS_MAX 16
static struct { uint32_t frame; int gy0, gy1, n; int16_t y1[BANDS_MAX]; uint16_t h[BANDS_MAX], v[BANDS_MAX]; } bnd[4];

static int next_band(int n, int y, int *h, int *v) {
    if (bnd[n].frame != r_frame || bnd[n].gy0 != R.gy0 || bnd[n].gy1 != R.gy1) {
        bnd[n].frame = r_frame; bnd[n].gy0 = R.gy0; bnd[n].gy1 = R.gy1; bnd[n].n = 0;
        for (int yy = R.gy0, yy1; yy < R.gy1; yy = yy1) {
            int hh, vv;
            yy1 = scan_band(n, yy, &hh, &vv);
            if (bnd[n].n == BANDS_MAX) { bnd[n].n = -1; break; }
            bnd[n].y1[bnd[n].n] = (int16_t)yy1;
            bnd[n].h[bnd[n].n] = (uint16_t)hh;
            bnd[n].v[bnd[n].n] = (uint16_t)vv;
            bnd[n].n++;
        }
    }
    if (bnd[n].n < 0)
        return scan_band(n, y, h, v);
    int k = 0;
    while (bnd[n].y1[k] <= y) k++;
    *h = bnd[n].h[k]; *v = bnd[n].v[k];
    return bnd[n].y1[k];
}

static void update_band(int n, uint32_t cnt, int h, int v, int y, int y1) {
    /* only a single full-height band keeps the frame-to-frame window bookkeeping */
    int single = y == R.gy0 && y1 == R.gy1;
    const lstate_t *st = &ls[n];
    if (st->frame == r_frame && st->ucnt == cnt && st->ux0 == h + R.gx0 && st->ux1 == h + R.gx1 &&
        st->uy0 == v + y && st->uy1 == v + y1 && st->uinc == single && st->ukey_hook == (r_view->bg_entry != NULL))
        return;                         /* done already in this frame */
    uint64_t tu = get_ticks();
    update_layer(n, cnt, h + R.gx0, h + R.gx1, v + y, v + y1, single);
    r_stats.us_bg_update += (uint32_t)TICKS_TO_US(get_ticks() - tu);
}

void bg_draw_text(int n, int fx) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * n);
    if (R.wx1 <= R.wx0 || R.wy1 <= R.wy0) return;
    r_bind(PAL_BG, fx);
    for (int y = R.gy0, y1; y < R.gy1; y = y1) {
        int h, v;
        y1 = next_band(n, y, &h, &v);
        update_band(n, cnt, h, v, y, y1);
        int d0 = y > R.wy0 ? y : R.wy0, d1 = y1 < R.wy1 ? y1 : R.wy1;
        if (d0 < d1)
            draw_ring(n, h, v, d0, d1);
    }
}

/* the slot flags of world row cy, columns cx..cx+63, as bits (bit i = column cx+i) */
static inline void row_flags(int n, int cx, int cy, uint64_t *full, uint64_t *solid) {
    unsigned s = (unsigned)cx & (RING_CX - 1);
    uint64_t f = row_full[n][cy & (RING_CY - 1)], so = row_solid[n][cy & (RING_CY - 1)];
    if (s) {
        f = (f >> s) | (f << (64 - s));
        so = (so >> s) | (so << (64 - s));
    }
    *full = f; *solid = so;
}

/* lowest << 8 | highest palette index over the opaque slots of the band */
static uint32_t band_range(int n, uint32_t cnt, int h, int v, int y, int y1, uint32_t rng) {
    uint32_t cb = ((cnt >> 2) & 3) * 0x4000u;
    int bpp8 = (cnt & 0x80) != 0;
    uint32_t lo = rng >> 8, hi = rng & 255;
    int cx0 = (h + R.gx0) >> 3, cx1 = (h + R.gx1 - 1) >> 3;
    for (int cy = (v + y) >> 3; cy <= (v + y1 - 1) >> 3; cy++) {
        uint64_t f = row_full[n][cy & (RING_CY - 1)];
        int row = (cy & (RING_CY - 1)) << 6;
        for (int cx = cx0; cx <= cx1; cx++) {
            int c = cx & (RING_CX - 1);
            if (!((f >> c) & 1)) continue;
            uint32_t r = slot_rng[n][row | c];
            if (!r) {
                /* the slot is current: its tile data in VRAM is what it shows */
                uint32_t e = slot[n][row | c].key >> 16;
                uint32_t a = cb + (e & 0x3FF) * (bpp8 ? 64u : 32u);
                r = index_range(gba_mem.vram + (a >> 2), bpp8 ? 64 : 32, bpp8 ? 8 : 4, (e >> 12) << 4);
                slot_rng[n][row | c] = (uint16_t)r;
            }
            if (r >> 8 < lo) lo = r >> 8;
            if ((r & 255) > hi) hi = r & 255;
        }
    }
    return (lo << 8) | hi;
}

uint32_t bg_text_colour_max(int n) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * n);
    uint32_t rng = 0xFF00;
    int h, v;
    int y1 = next_band(n, R.gy0, &h, &v);
    if (y1 == R.gy1) {
        update_band(n, cnt, h, v, R.gy0, y1);
        vkey_t k = { 1, h, v, R.gx0, R.gy0, R.gx1, R.gy1, rng_gen[n] };
        if (!memcmp(&k, &rkey[n], sizeof k)) {
            rng = rc_rng[n];
        } else {
            rng = band_range(n, cnt, h, v, R.gy0, y1, rng);
            rkey[n] = k;
            rc_rng[n] = rng;
        }
    } else {
        for (int y = R.gy0; y < R.gy1; y = y1) {
            y1 = next_band(n, y, &h, &v);
            update_band(n, cnt, h, v, y, y1);
            rng = band_range(n, cnt, h, v, y, y1, rng);
        }
    }
    uint32_t m[3] = { 0, 0, 0 };
    for (uint32_t i = rng >> 8; i <= (rng & 255); i++) {
        uint32_t c = r_pal16(i);
        for (int k = 0; k < 3; k++, c >>= 5)
            if ((c & 31) > m[k]) m[k] = c & 31;
    }
    return m[0] | (m[1] << 5) | (m[2] << 10);
}

/* bg_text_cells for one band of lines with scroll (h, v) */
static void band_cells(int n, int h, int v, int y, int y1, cmask_t *touch, cmask_t *solid) {
    uint64_t cols = (1ull << cm_w) - 1;
    /* screen cell i covers world columns cxs+i (and cxs+i+1 if the scroll is
     * not a multiple of 8): one bit per world column, then per cell */
    int X0 = h + R.gx0, cxs = X0 >> 3, split = (X0 & 7) != 0;
    int j0 = (y - R.gy0) >> 3, j1 = (y1 - 1 - R.gy0) >> 3;
    for (int j = j0; j <= j1; j++) {
        int ya = R.gy0 + 8 * j, yb = ya + 8;
        if (ya < y) ya = y;
        if (yb > y1) yb = y1;
        uint64_t t = 0, s = ~0ull;
        for (int cy = (v + ya) >> 3; cy <= (v + yb - 1) >> 3; cy++) {
            uint64_t f, so;
            row_flags(n, cxs, cy, &f, &so);
            if (split) { f |= f >> 1; so &= so >> 1; }     /* (cm_w <= 63: bit cm_w is valid) */
            t |= f; s &= so;
        }
        touch->r[j] |= t & cols;
        solid->r[j] &= s;
    }
}

void bg_text_cells(int n, cmask_t *touch, cmask_t *solid) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * n);
    uint64_t cols = (1ull << cm_w) - 1;
    for (int j = 0; j < cm_h; j++) solid->r[j] = cols;
    int h, v;
    int y1 = next_band(n, R.gy0, &h, &v);
    if (y1 == R.gy1) {
        update_band(n, cnt, h, v, R.gy0, y1);
        vkey_t k = { 1, h, v, R.gx0, R.gy0, R.gx1, R.gy1, mask_gen[n] };
        if (memcmp(&k, &mkey[n], sizeof k)) {
            for (int j = 0; j < cm_h; j++) { mc_touch[n].r[j] = 0; mc_solid[n].r[j] = cols; }
            band_cells(n, h, v, R.gy0, y1, &mc_touch[n], &mc_solid[n]);
            mkey[n] = k;
        }
        for (int j = 0; j < cm_h; j++) {
            touch->r[j] |= mc_touch[n].r[j];
            solid->r[j] = mc_solid[n].r[j];
        }
        return;
    }
    for (int y = R.gy0; y < R.gy1; y = y1) {
        y1 = next_band(n, y, &h, &v);
        update_band(n, cnt, h, v, y, y1);
        band_cells(n, h, v, y, y1, touch, solid);
    }
}

/* ------------------------------------------------------------ CPU raster */

typedef struct { int32_t x, y, pa, pc; } aff_t;

static void affine_line(int n, int line, aff_t *a) {
    if (n == 2 && gba_line_regs_valid && line >= 0 && line < 160) {
        const gba_line_regs_t *s = &gba_line_regs[line];
        a->pa = (int16_t)s->bg2pa;
        a->pc = (int16_t)s->bg2pc;
        a->x = r_sext28(s->bg2x);
        a->y = r_sext28(s->bg2y);
        return;
    }
    uint32_t base = n == 2 ? REG_BG2PA : REG_BG3PA;
    int32_t pb = (int16_t)io16(base + 2), pd = (int16_t)io16(base + 6);
    a->pa = (int16_t)io16(base);
    a->pc = (int16_t)io16(base + 4);
    a->x = (int32_t)((uint32_t)r_sext28(io32(base + 8)) + (uint32_t)(pb * line));
    a->y = (int32_t)((uint32_t)r_sext28(io32(base + 12)) + (uint32_t)(pd * line));
}

void bg_draw_affine(int n, int fx) {
    uint64_t t0 = get_ticks();
    uint32_t cnt = io16(REG_BG0CNT + 2 * n);
    uint32_t cb = ((cnt >> 2) & 3) * 0x4000u;
    uint32_t sb = ((cnt >> 8) & 31) * 0x800u;
    uint32_t size = 128u << ((cnt >> 14) & 3);
    uint32_t smask = size - 1, tshift = 4 + ((cnt >> 14) & 3);   /* log2(size/8) */
    int wrap = (cnt & 0x2000) != 0;
    int w = R.gx1 - R.gx0, h = R.gy1 - R.gy0;
    int pitch = (w + 7) & ~7;
    uint8_t *buf = r_raster_buf(n - 2, (uint32_t)(pitch * h));
    static uint32_t done_frame[2];
    if (done_frame[n - 2] == r_frame)       /* drawn again in a later pass of this frame */
        goto blit;
    done_frame[n - 2] = r_frame;
    const uint8_t *vr = (const uint8_t *)gba_mem.vram;
    int fast = wrap && sb + (size >> 3) * (size >> 3) <= 0x10000u && cb + 0x4000u <= 0x10000u;
    for (int yy = 0; yy < h; yy++) {
        aff_t a;
        affine_line(n, R.gy0 + yy, &a);
        uint32_t tx = (uint32_t)a.x + (uint32_t)(a.pa * R.gx0);
        uint32_t ty = (uint32_t)a.y + (uint32_t)(a.pc * R.gx0);
        uint64_t *dst = (uint64_t *)(buf + yy * pitch);
        if (fast) {
            /* wrapping map inside BG VRAM: no per-pixel range checks.  sb and cb
             * are multiples of 0x800, so the byte-lane XOR commutes with them. */
            const uint8_t *map = vr + sb, *chr = vr + cb;
            int32_t pa = a.pa, pc = a.pc;
            for (int x = 0; x < pitch; x += 8) {
                uint64_t acc = 0;
                for (int k = 0; k < 8; k++) {
                    uint32_t px = ((uint32_t)((int32_t)tx >> 8)) & smask;
                    uint32_t py = ((uint32_t)((int32_t)ty >> 8)) & smask;
                    tx += (uint32_t)pa; ty += (uint32_t)pc;
                    uint32_t tile = map[(((py >> 3) << tshift) + (px >> 3)) ^ GBA_BX];
                    acc = (acc << 8) | chr[((tile << 6) + ((py & 7) << 3) + (px & 7)) ^ GBA_BX];
                }
                dst[x >> 3] = acc;
            }
            continue;
        }
        for (int x = 0; x < pitch; x += 8) {
            uint64_t acc = 0;
            for (int k = 0; k < 8; k++, tx += (uint32_t)a.pa, ty += (uint32_t)a.pc) {
                uint32_t px = (uint32_t)((int32_t)tx >> 8);
                uint32_t py = (uint32_t)((int32_t)ty >> 8);
                uint32_t idx = 0;
                if (wrap) {
                    px &= smask;
                    py &= smask;
                } else if (px >= size || py >= size) {
                    acc <<= 8;
                    continue;
                }
                uint32_t tile = vr[r_vram_wrap(sb + ((py >> 3) << tshift) + (px >> 3)) ^ GBA_BX];
                uint32_t ta = cb + tile * 64 + (py & 7) * 8 + (px & 7);
                if (ta < 0x10000u)
                    idx = vr[ta ^ GBA_BX];
                acc = (acc << 8) | idx;
            }
            dst[x >> 3] = acc;
        }
    }
    r_stats.us_raster += (uint32_t)TICKS_TO_US(get_ticks() - t0);
blit:
    r_bind(PAL_BG, fx);
    r_scissor_region();
    r_blit_image(buf, pitch, FMT_CI8, R.gx0, R.gy0, w, h);
}

void bg_draw_bitmap(int mode, uint32_t dispcnt, int fx) {
    uint64_t t0 = get_ticks();
    uint32_t page = (mode != 3 && (dispcnt & 0x10)) ? 0xA000u : 0;
    uint32_t bw = mode == 5 ? 160 : 240, bh = mode == 5 ? 128 : 160;
    int w = R.gx1 - R.gx0, h = R.gy1 - R.gy0;
    int ci8 = mode == 4;
    int pitch_px = (w + 7) & ~7;
    int pitch = ci8 ? pitch_px : pitch_px * 2;
    /* direct colour (modes 3/5) bakes the colour effect in, so the passes of a
     * tagged blend frame (BG2's own effect, then FX_PLAIN) need different
     * pixels: one buffer per effect, never rewritten while an earlier blit of
     * this frame may still read it (raster buffer 1 is free in bitmap modes) */
    int which = (!ci8 && fx != FX_PLAIN) ? 1 : 0;
    uint8_t *buf = r_raster_buf(which, (uint32_t)(pitch * h));
    const uint8_t *tau = r_tau(fx);
    static uint32_t done_frame[2], done_fx[2];
    if (done_frame[which] == r_frame && (ci8 || done_fx[which] == (uint32_t)fx))
        goto blit;
    if (done_frame[which] == r_frame) {     /* another effect in the same buffer */
        raw_flush();
        rspq_wait();
    }
    done_frame[which] = r_frame; done_fx[which] = (uint32_t)fx;
    for (int yy = 0; yy < h; yy++) {
        aff_t a;
        affine_line(2, R.gy0 + yy, &a);
        uint32_t tx = (uint32_t)a.x + (uint32_t)(a.pa * R.gx0);
        uint32_t ty = (uint32_t)a.y + (uint32_t)(a.pc * R.gx0);
        uint8_t *row = buf + yy * pitch;
        if (ci8 && a.pa == 256 && a.pc == 0) {
            /* unscaled row (the assert screen): straight copy of the bitmap row */
            int32_t px = (int32_t)tx >> 8;
            uint32_t py = (uint32_t)((int32_t)ty >> 8);
            const uint8_t *src = (const uint8_t *)gba_mem.vram + page + py * 240;
            for (int x = 0; x < pitch_px; x += 8) {
                uint64_t acc = 0;
                for (int k = 0; k < 8; k++, px++)
                    acc = (acc << 8) | ((uint32_t)px < bw && py < bh ? src[(uint32_t)px ^ GBA_BX] : 0);
                *(uint64_t *)(row + x) = acc;
            }
            continue;
        }
        for (int x = 0; x < pitch_px; x += 4) {
            uint64_t acc = 0;
            for (int k = 0; k < 4 * (ci8 ? 2 : 1); k++, tx += (uint32_t)a.pa, ty += (uint32_t)a.pc) {
                uint32_t px = (uint32_t)((int32_t)tx >> 8);
                uint32_t py = (uint32_t)((int32_t)ty >> 8);
                uint32_t out = 0;
                if (px < bw && py < bh) {
                    if (ci8) {
                        out = r_vram8(page + py * 240 + px);
                    } else {
                        uint32_t c = r_vram16(page + (py * bw + px) * 2) & 0x7FFF;
                        if (fx != FX_PLAIN)
                            c = tau[c & 31] | (tau[(c >> 5) & 31] << 5) | (tau[(c >> 10) & 31] << 10);
                        out = r_conv(c);
                    }
                }
                acc = ci8 ? (acc << 8) | out : (acc << 16) | out;
            }
            if (ci8) {
                *(uint64_t *)(row + x) = acc;
                x += 4;
            } else {
                *(uint64_t *)(row + x * 2) = acc;
            }
        }
    }
    r_stats.us_raster += (uint32_t)TICKS_TO_US(get_ticks() - t0);
blit:
    r_bind(ci8 ? PAL_BG : PAL_DIRECT, fx);
    r_scissor_region();
    r_blit_image(buf, pitch, ci8 ? FMT_CI8 : FMT_RGBA16, R.gx0, R.gy0, w, h);
}

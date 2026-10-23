/*
 * ppu_soft.c - reference software renderer for the GBA PPU.
 *
 * One scanline at a time, straight from guest VRAM / palette / OAM and the
 * display registers.  Clarity first: this is the correctness reference that
 * faster N64 paths get compared against.
 *
 * Per line:
 *   1. each enabled BG -> a 240-entry line buffer (TRANSP = no pixel)
 *   2. OBJ layer -> colour / priority / semi-transparent / OBJ-window buffers
 *   3. window mask per pixel (bits 0..3 BG, 4 OBJ, 5 colour effect)
 *   4. compositor: top two visible layers per pixel, then the BLDCNT effect
 *
 * Hardware details that are deliberately not modelled: the OBJ rendering
 * cycle budget per line, the BG enable delay, green swap, and mid-frame
 * register writes other than the HBlank snapshots in gba_line_regs[].
 */
#include <string.h>
#include "ppu_soft.h"
#include "gba.h"
#include "io.h"

#define LINE_W  PPU_SOFT_WIDTH
#define TRANSP  0x8000u     /* line buffer marker; real colours have bit 15 clear */

/* layer ids, also the bit numbers in BLDCNT (1st target) and the window masks */
enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BD, L_NONE };

/* ------------------------------------------------------------ memory */

/* Guest memory is native 32-bit words: XOR the low address bits to find a
 * byte / halfword lane (no-op on little-endian hosts). */
static inline uint32_t vram8(uint32_t o) {
    return ((const uint8_t *)gba_mem.vram)[o ^ GBA_BX];
}
static inline uint32_t vram16(uint32_t o) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.vram + (o ^ GBA_HX));
}
static inline uint32_t oam16(uint32_t o) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.oam + (o ^ GBA_HX));
}
/* palette entry i (0..511, OBJ palette at 256); bit 15 is unused by hardware */
static inline uint32_t pal16(uint32_t i) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.pal + ((i * 2) ^ GBA_HX)) & 0x7FFFu;
}

/* VRAM is 96KB mirrored in 128KB blocks, the last 32KB repeating 0x10000.. */
static inline uint32_t vram_wrap(uint32_t o) {
    o &= 0x1FFFFu;
    return o >= 0x18000u ? o - 0x8000u : o;
}

/* BGxX / BGxY are 28-bit signed 20.8 fixed point */
static inline int32_t sext28(uint32_t v) {
    return (int32_t)(v << 4) >> 4;
}

static inline int imin(int a, int b) { return a < b ? a : b; }

/* ------------------------------------------------------------ line state */

static uint16_t bg_buf[4][LINE_W];
static uint16_t obj_col[LINE_W];
static uint8_t obj_prio[LINE_W];
static uint8_t obj_semi[LINE_W];
static uint8_t obj_win[LINE_W];
static uint8_t win_mask[LINE_W];

static inline int line_snapshot(int line) {
    return gba_line_regs_valid && line >= 0 && line < PPU_SOFT_HEIGHT;
}

static void bg_scroll(int n, int line, uint32_t *h, uint32_t *v) {
    if (line_snapshot(line)) {
        *h = gba_line_regs[line].hofs[n];
        *v = gba_line_regs[line].vofs[n];
    } else {
        *h = io16(REG_BG0HOFS + 4 * n);
        *v = io16(REG_BG0HOFS + 4 * n + 2);
    }
    *h &= 0x1FF;
    *v &= 0x1FF;
}

typedef struct {
    int32_t x, y;       /* reference point of the line, 20.8 */
    int32_t pa, pc;     /* per-pixel step, 8.8 */
} affine_line_t;

/* Affine parameters for BG2/BG3 on 'line'.  The HBlank snapshot holds the
 * exact reference point the game installed for that line; otherwise the
 * internal reference point is BGxX/BGxY advanced by PB/PD once per line. */
static void bg_affine(int n, int line, affine_line_t *a) {
    if (n == 2 && line_snapshot(line)) {
        const gba_line_regs_t *s = &gba_line_regs[line];
        a->pa = (int16_t)s->bg2pa;
        a->pc = (int16_t)s->bg2pc;
        a->x = sext28(s->bg2x);
        a->y = sext28(s->bg2y);
        return;
    }
    uint32_t base = n == 2 ? REG_BG2PA : REG_BG3PA;
    int32_t pb = (int16_t)io16(base + 2);
    int32_t pd = (int16_t)io16(base + 6);
    a->pa = (int16_t)io16(base);
    a->pc = (int16_t)io16(base + 4);
    /* unsigned math: the 28-bit hardware counter wraps, C ints must not */
    a->x = (int32_t)((uint32_t)sext28(io32(base + 8)) + (uint32_t)(pb * line));
    a->y = (int32_t)((uint32_t)sext28(io32(base + 12)) + (uint32_t)(pd * line));
}

/* ------------------------------------------------------------ BG layers */

/* Text BG: y is the (mosaic-adjusted) line, scroll comes from 'line'. */
static void draw_text_bg(int n, int line, int y, uint16_t *dst) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * n);
    uint32_t char_base = ((cnt >> 2) & 3) * 0x4000u;
    uint32_t scr_base = ((cnt >> 8) & 31) * 0x800u;
    uint32_t wmask = (cnt & 0x4000) ? 511 : 255;
    uint32_t hmask = (cnt & 0x8000) ? 511 : 255;
    int bpp8 = (cnt & 0x80) != 0;
    uint32_t hofs, vofs;
    bg_scroll(n, line, &hofs, &vofs);

    uint32_t by = ((uint32_t)y + vofs) & hmask;
    /* 32x32-entry screen blocks; a 512-wide map has two per block row */
    uint32_t row_base = scr_base + (by >> 8) * ((wmask >> 8) + 1) * 0x800u
                      + ((by >> 3) & 31) * 64;
    uint32_t ty = by & 7;
    uint32_t bx = hofs;
    int x = 0;
    while (x < LINE_W) {
        uint32_t px = bx & wmask;
        uint32_t e = vram16(vram_wrap(row_base + (px >> 8) * 0x800u + ((px >> 3) & 31) * 2));
        uint32_t tile = e & 0x3FF;
        uint32_t r = (e & 0x800) ? 7 - ty : ty;
        uint32_t hflip = (e & 0x400) ? 7 : 0;
        int c0 = (int)(px & 7);
        int span = imin(8 - c0, LINE_W - x);
        /* BG tile fetches cannot reach OBJ VRAM (0x10000+) in tile modes */
        if (bpp8) {
            uint32_t ta = char_base + tile * 64 + r * 8;
            for (int i = 0; i < span; i++) {
                uint32_t idx = ta < 0x10000u ? vram8(ta + ((uint32_t)(c0 + i) ^ hflip)) : 0;
                dst[x + i] = idx ? (uint16_t)pal16(idx) : TRANSP;
            }
        } else {
            uint32_t ta = char_base + tile * 32 + r * 4;
            uint32_t pbank = (e >> 12) * 16;
            for (int i = 0; i < span; i++) {
                uint32_t c = (uint32_t)(c0 + i) ^ hflip;
                uint32_t idx = ta < 0x10000u ? vram8(ta + (c >> 1)) : 0;
                idx = (c & 1) ? idx >> 4 : idx & 15;
                dst[x + i] = idx ? (uint16_t)pal16(pbank + idx) : TRANSP;
            }
        }
        x += span;
        bx += (uint32_t)span;
    }
}

/* Affine (rotation/scaling) BG: 8bpp tiles, byte map, square 128..1024. */
static void draw_affine_bg(int n, int y, uint16_t *dst) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * n);
    uint32_t char_base = ((cnt >> 2) & 3) * 0x4000u;
    uint32_t scr_base = ((cnt >> 8) & 31) * 0x800u;
    uint32_t size = 128u << ((cnt >> 14) & 3);
    int wrap = (cnt & 0x2000) != 0;
    affine_line_t a;
    bg_affine(n, y, &a);

    uint32_t tx = (uint32_t)a.x, ty = (uint32_t)a.y;
    for (int x = 0; x < LINE_W; x++, tx += (uint32_t)a.pa, ty += (uint32_t)a.pc) {
        uint32_t px = (uint32_t)((int32_t)tx >> 8);
        uint32_t py = (uint32_t)((int32_t)ty >> 8);
        if (wrap) {
            px &= size - 1;
            py &= size - 1;
        } else if (px >= size || py >= size) {
            dst[x] = TRANSP;
            continue;
        }
        uint32_t tile = vram8(vram_wrap(scr_base + (py >> 3) * (size >> 3) + (px >> 3)));
        uint32_t ta = char_base + tile * 64 + (py & 7) * 8 + (px & 7);
        uint32_t idx = ta < 0x10000u ? vram8(ta) : 0;
        dst[x] = idx ? (uint16_t)pal16(idx) : TRANSP;
    }
}

/* Bitmap modes 3/4/5 (BG2, transformed by the BG2 affine parameters). */
static void draw_bitmap_bg(int mode, uint32_t dispcnt, int y, uint16_t *dst) {
    uint32_t page = (mode != 3 && (dispcnt & 0x10)) ? 0xA000u : 0;
    uint32_t bw = mode == 5 ? 160 : 240;
    uint32_t bh = mode == 5 ? 128 : 160;
    affine_line_t a;
    bg_affine(2, y, &a);

    uint32_t tx = (uint32_t)a.x, ty = (uint32_t)a.y;
    for (int x = 0; x < LINE_W; x++, tx += (uint32_t)a.pa, ty += (uint32_t)a.pc) {
        uint32_t px = (uint32_t)((int32_t)tx >> 8);
        uint32_t py = (uint32_t)((int32_t)ty >> 8);
        if (px >= bw || py >= bh) {     /* no wraparound in bitmap modes */
            dst[x] = TRANSP;
            continue;
        }
        if (mode == 4) {
            uint32_t idx = vram8(page + py * 240 + px);
            dst[x] = idx ? (uint16_t)pal16(idx) : TRANSP;
        } else {
            dst[x] = (uint16_t)(vram16(page + (py * bw + px) * 2) & 0x7FFF);
        }
    }
}

/* Horizontal mosaic: each pixel repeats the first one of its screen-aligned block. */
static void mosaic_h(uint16_t *buf, int mh) {
    for (int x = 0; x < LINE_W; x++)
        buf[x] = buf[x - x % mh];
}

/* ------------------------------------------------------------ OBJ layer */

/* [shape][size] -> width, height */
static const uint8_t obj_dim[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

/* Per pixel the OBJ layer keeps the opaque pixel with the lowest priority
 * value; on ties the lower OAM index wins (it is drawn first).  OBJ-window
 * sprites only mark obj_win[]. */
static void draw_objs(int line, uint32_t dispcnt) {
    uint32_t mosaic = io16(REG_MOSAIC);
    int mh = (int)((mosaic >> 8) & 15) + 1;
    int mv = (int)((mosaic >> 12) & 15) + 1;
    int one_d = (dispcnt & 0x40) != 0;
    int objwin_on = (dispcnt & 0x8000) != 0;
    /* in bitmap modes the BG owns 0x10000-0x13FFF: OBJ tiles 0..511 vanish */
    uint32_t min_addr = (dispcnt & 7) >= 3 ? 0x14000u : 0x10000u;

    for (int x = 0; x < LINE_W; x++) {
        obj_col[x] = TRANSP;
        obj_prio[x] = 4;
        obj_semi[x] = 0;
        obj_win[x] = 0;
    }

    for (uint32_t i = 0; i < 128; i++) {
        uint32_t a0 = oam16(i * 8), a1 = oam16(i * 8 + 2), a2 = oam16(i * 8 + 4);
        int affine = (a0 & 0x100) != 0;
        if (!affine && (a0 & 0x200))            /* disabled */
            continue;
        uint32_t mode = (a0 >> 10) & 3;
        if (mode == 3 || (mode == 2 && !objwin_on))
            continue;
        uint32_t shape = a0 >> 14;
        if (shape == 3)
            continue;
        int w = obj_dim[shape][a1 >> 14][0];
        int h = obj_dim[shape][a1 >> 14][1];
        int bw = w, bh = h;                     /* bounding box */
        if (affine && (a0 & 0x200)) {
            bw *= 2;
            bh *= 2;
        }
        /* y is 8 bits and wraps: objects near the bottom reappear at the top */
        int oy = (int)(a0 & 0xFF);
        int row = (line - oy) & 0xFF;
        if (row >= bh)
            continue;
        int ox = (int)(a1 & 0x1FF);
        if (ox & 0x100)
            ox -= 512;
        if (ox >= LINE_W || ox + bw <= 0)
            continue;
        int mos = (a0 & 0x1000) != 0;
        if (mos && mv > 1) {
            /* vertical mosaic follows the screen grid, not the object */
            row = ((line - line % mv) - oy) & 0xFF;
            if (row >= bh)
                row = 0;
        }

        int bpp8 = (a0 & 0x2000) != 0;
        uint32_t tile = a2 & 0x3FF;
        uint32_t pbank = 256 + (a2 >> 12) * 16;
        uint8_t prio = (uint8_t)((a2 >> 10) & 3);
        /* tile units (32 bytes) between rows of 8 texels */
        uint32_t row_stride = one_d ? (uint32_t)(w / 8) * (bpp8 ? 2 : 1) : 32;
        if (bpp8 && !one_d)
            tile &= ~1u;

        int32_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            uint32_t g = ((a1 >> 9) & 31) * 32;
            pa = (int16_t)oam16(g + 6);
            pb = (int16_t)oam16(g + 14);
            pc = (int16_t)oam16(g + 22);
            pd = (int16_t)oam16(g + 30);
        }

        int x0 = ox < 0 ? 0 : ox;
        int x1 = imin(ox + bw, LINE_W);
        for (int x = x0; x < x1; x++) {
            int lx = x - ox;
            if (mos && mh > 1) {
                int mx = x - x % mh;
                lx = mx < ox ? 0 : mx - ox;
            }
            int tx, ty;
            if (affine) {
                /* rotate around the box centre; texture origin is the object centre */
                int ix = lx - bw / 2, iy = row - bh / 2;
                tx = ((pa * ix + pb * iy) >> 8) + w / 2;
                ty = ((pc * ix + pd * iy) >> 8) + h / 2;
                if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h)
                    continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - row : row;
            }
            uint32_t t = tile + (uint32_t)(ty >> 3) * row_stride
                       + (uint32_t)(tx >> 3) * (bpp8 ? 2 : 1);
            uint32_t inner = bpp8 ? (uint32_t)((ty & 7) * 8 + (tx & 7))
                                  : (uint32_t)((ty & 7) * 4 + ((tx & 7) >> 1));
            uint32_t addr = 0x10000u + ((t * 32 + inner) & 0x7FFFu);
            if (addr < min_addr)
                continue;
            uint32_t idx = vram8(addr);
            if (!bpp8)
                idx = (tx & 1) ? idx >> 4 : idx & 15;
            if (!idx)
                continue;
            if (mode == 2) {
                obj_win[x] = 1;
                continue;
            }
            if (obj_col[x] == TRANSP || prio < obj_prio[x]) {
                obj_col[x] = (uint16_t)pal16(bpp8 ? 256 + idx : pbank + idx);
                obj_prio[x] = prio;
                obj_semi[x] = mode == 1;
            }
        }
    }
}

/* ------------------------------------------------------------ windows */

/* Window edges: inside from a (inclusive) to b (exclusive); a > b wraps. */
static inline int win_inside(int p, int a, int b) {
    return a <= b ? (p >= a && p < b) : (p >= a || p < b);
}

static void build_windows(int line, uint32_t dispcnt) {
    if (!(dispcnt & 0xE000)) {
        memset(win_mask, 0x3F, LINE_W);
        return;
    }
    uint32_t winin = io16(REG_WININ), winout = io16(REG_WINOUT);
    memset(win_mask, (int)(winout & 0x3F), LINE_W);
    /* paint in reverse priority: OBJ window, then WIN1, then WIN0 on top */
    if ((dispcnt & 0x9000) == 0x9000) {
        uint8_t m = (uint8_t)((winout >> 8) & 0x3F);
        for (int x = 0; x < LINE_W; x++)
            if (obj_win[x])
                win_mask[x] = m;
    }
    for (int w = 1; w >= 0; w--) {
        if (!(dispcnt & (0x2000u << w)))
            continue;
        uint32_t v = io16(REG_WIN0H + 4 + 2 * w);
        if (!win_inside(line, (int)(v >> 8), (int)(v & 0xFF)))
            continue;
        uint32_t hreg = io16(REG_WIN0H + 2 * w);
        int x1 = (int)(hreg >> 8), x2 = (int)(hreg & 0xFF);
        uint8_t m = (uint8_t)((winin >> (8 * w)) & 0x3F);
        for (int x = 0; x < LINE_W; x++)
            if (win_inside(x, x1, x2))
                win_mask[x] = m;
    }
}

/* ------------------------------------------------------------ effects */

static inline uint32_t blend_alpha(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb) {
    uint32_t r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    uint32_t g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    uint32_t bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    /* EVA + EVB may exceed 16: saturate each channel */
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return r | (g << 5) | (bl << 10);
}

static inline uint32_t brighten(uint32_t c, uint32_t evy) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return r | (g << 5) | (b << 10);
}

static inline uint32_t darken(uint32_t c, uint32_t evy) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return r | (g << 5) | (b << 10);
}

/* ------------------------------------------------------------ line */

/* which BGs exist in each mode: 1 text, 2 affine, 3 bitmap */
static const uint8_t bg_kind[8][4] = {
    { 1, 1, 1, 1 }, { 1, 1, 2, 0 }, { 0, 0, 2, 2 }, { 0, 0, 3, 0 },
    { 0, 0, 3, 0 }, { 0, 0, 3, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
};

void ppu_soft_render_line(int line, uint16_t *out) {
    uint32_t dispcnt = io16(REG_DISPCNT);
    if (dispcnt & 0x80) {                       /* forced blank: white */
        for (int x = 0; x < LINE_W; x++)
            out[x] = 0x7FFF;
        return;
    }
    int mode = (int)(dispcnt & 7);
    uint32_t mosaic = io16(REG_MOSAIC);
    int bg_mh = (int)(mosaic & 15) + 1;
    int bg_mv = (int)((mosaic >> 4) & 15) + 1;

    /* BG layers, collected in drawing order: priority, then BG number */
    int order[4], nbg = 0;
    uint8_t bg_prio[4] = { 0, 0, 0, 0 };
    for (int n = 0; n < 4; n++) {
        int kind = bg_kind[mode][n];
        if (!kind || !(dispcnt & (0x100u << n)))
            continue;
        uint32_t cnt = io16(REG_BG0CNT + 2 * n);
        int mos = (cnt & 0x40) != 0;
        int y = mos ? line - line % bg_mv : line;
        if (kind == 1)
            draw_text_bg(n, line, y, bg_buf[n]);
        else if (kind == 2)
            draw_affine_bg(n, y, bg_buf[n]);
        else
            draw_bitmap_bg(mode, dispcnt, y, bg_buf[n]);
        if (mos && bg_mh > 1)
            mosaic_h(bg_buf[n], bg_mh);
        bg_prio[n] = (uint8_t)(cnt & 3);
        int k = nbg++;
        while (k > 0 && bg_prio[order[k - 1]] > bg_prio[n]) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = n;
    }

    if (dispcnt & 0x1000) {
        draw_objs(line, dispcnt);
    } else {
        for (int x = 0; x < LINE_W; x++) {
            obj_col[x] = TRANSP;
            obj_win[x] = 0;
        }
    }
    build_windows(line, dispcnt);

    uint32_t bldcnt = io16(REG_BLDCNT) & 0x3FFF;
    uint32_t bldalpha = io16(REG_BLDALPHA);
    /* coefficients 17..31 act as 16 */
    uint32_t eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = io16(REG_BLDY) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    uint32_t effect = (bldcnt >> 6) & 3;
    uint32_t backdrop = pal16(0);

    for (int x = 0; x < LINE_W; x++) {
        uint32_t m = win_mask[x];
        uint32_t col[2] = { backdrop, backdrop };
        int lay[2] = { L_BD, L_NONE };
        int n = 0;
        /* OBJ with priority p sits above BGs of priority >= p */
        int op = (obj_col[x] != TRANSP && (m & 0x10)) ? obj_prio[x] : 4;
        for (int i = 0; i < nbg && n < 2; i++) {
            int b = order[i];
            if (op <= bg_prio[b]) {
                col[n] = obj_col[x];
                lay[n++] = L_OBJ;
                op = 5;
                if (n == 2)
                    break;
            }
            if (((m >> b) & 1) && bg_buf[b][x] != TRANSP) {
                col[n] = bg_buf[b][x];
                lay[n++] = b;
            }
        }
        if (n < 2 && op < 4) {
            col[n] = obj_col[x];
            lay[n++] = L_OBJ;
        }
        if (n < 2) {                            /* backdrop under everything */
            col[n] = backdrop;
            lay[n++] = L_BD;
            if (n == 1)
                lay[1] = L_NONE;
        }

        uint32_t c = col[0];
        uint32_t second = lay[1] == L_NONE ? 0 : (bldcnt >> (8 + lay[1])) & 1;
        if (lay[0] == L_OBJ && obj_semi[x] && second) {
            /* semi-transparent OBJ: alpha with any 2nd target, whatever
             * BLDCNT's mode / 1st-target bits and the window effect bit say */
            c = blend_alpha(c, col[1], eva, evb);
        } else if ((m & 0x20) && effect && ((bldcnt >> lay[0]) & 1)) {
            if (effect == 1) {
                if (second)
                    c = blend_alpha(c, col[1], eva, evb);
            } else if (effect == 2) {
                c = brighten(c, evy);
            } else {
                c = darken(c, evy);
            }
        }
        out[x] = (uint16_t)c;
    }
}

void ppu_soft_render(uint16_t *fb, int pitch_pixels) {
    for (int y = 0; y < PPU_SOFT_HEIGHT; y++)
        ppu_soft_render_line(y, fb + (ptrdiff_t)y * pitch_pixels);
}

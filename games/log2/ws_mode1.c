/*
 * ws_mode1.c - 320x240 view of LoG2's mode-1 world map (flying over the
 * overworld).  See docs/WIDESCREEN.md ("Mode-1 world map") and
 * runtime/widescreen.h; widescreen.c calls in here when the frame is not a
 * field map.
 *
 * The screen (DISPCNT 0x1641/0x1741, mode 1):
 *   BG2  affine ground plane, layer object vt 0x08023E7C at 0x0300107C+8.
 *        Per-line parameters: the HBlank handler 0x08011F88 copies entry
 *        VCOUNT+1 of the 16-byte table [View+0x6C] {PA|PB, PC|PD, X, Y};
 *        0x08012520 fills entries 56..159 each frame (entries 0..55 keep a
 *        filler the sky hides).  For line L, k = L-56:
 *          A = [0x086D82DC + 4k], B = [0x086D855C + 4k]   (ROM, 160 entries,
 *              A = 425984/(3k+104), B = 1024k/104 rounded down)
 *          pa = (m0*A) >> 14, pc = (m1*A) >> 14
 *          x  = ((((U*B) >> 10) + ip) * A >> 14) - 120*pa
 *          y  = ((((V*B) >> 10) + lr) * A >> 14) - 120*pc
 *        (32-bit wrapping multiplies, arithmetic shifts) with m0, m1, U, V,
 *        ip, lr loop invariants computed from the camera.  A hook before the
 *        loop (0x08012612) records them; the lines below the native screen
 *        (160..199) are the same formula with the tables' next entries, with
 *        64-bit products (see line_entry()).
 *   BG1  sky panorama, layer object vt 0x08023A00: a 64x32-cell map at
 *        obj+8 (two 32x32 halves), of which the game DMAs the 32 columns in
 *        view into the hardware map; HOFS = the panorama scroll.  World cell
 *        cx shows panorama column cx & 63; rows 0..6 hold the sky.
 *   BG0  a field layer (vt 0x08023868): the minimap outline (top right) and
 *        the haze band over the horizon (rows 7..11), both 240 px wide.
 *   OBJ  the player (vt 0x08025F28) at the centre, its shadow (0x080250D4),
 *        minimap markers (0x08024064, 0x08025C08), the location banner.
 *
 * The 320x240 view: the native screen at (40,40), region (-40,-40)-(280,200).
 *   - BG2: lines 0..159 from the hardware snapshots; below, the extended
 *     table; above (sky), the filler of line 0.  Horizontally each line is an
 *     affine line, exact for any x.
 *   - BG1: outside the native screen, the panorama (rows above the sky
 *     repeat row 0, the top of the sky).
 *   - BG0: haze rows extend their edge cells; the minimap (its non-band
 *     cells) moves to the top-right corner, its markers with it.
 *   - sprites: the field rules (widescreen.c), except the player and its
 *     shadow, which stay where the ground puts them, and the markers.
 * While the view is shown the game also streams the ground for the wider
 * rect and its landmarks' culls accept the wider screen (render state only,
 * see "ground streaming" and "object culling" below).  With the view hooks
 * inactive the game is unchanged: the table hook only reads registers and
 * the stack, and the overrides return the game's values.
 */
#include <string.h>
#include "widescreen.h"
#include "gba.h"
#include "io.h"

/* widescreen.c */
void ws_m1_sprites(int ox, int oy);
int ws_m1_sprite(int i, int *x, int *y, uint32_t *vt);
extern int ws_hud_inset;

#define VIEW_OBJ        0x03001090u
#define VIEW_TABLE      (VIEW_OBJ + 0x6C)
#define LAYER_SET       0x0300107Cu
#define VT_M1_GROUND    0x08023E7Cu
#define VT_M1_SKY       0x08023A00u
#define VT_FIELD_LAYER  0x08023868u
#define HBLANK_HANDLER  0x08011F89u
#define IRQ_TABLE_HBL   0x03000E40u
#define TAB_A           0x086D82DCu
#define TAB_B           0x086D855Cu
#define TAB_N           160             /* entries of each ROM table: lines 56..215 */
#define FIRST_LINE      56

#define VT_PLAYER       0x08025F28u
#define VT_SHADOW       0x080250D4u
#define VT_MARKER_A     0x08024064u
#define VT_MARKER_B     0x08025C08u

/* view: native screen at (40,40) */
#define OXV (-40)
#define OYV (-40)

static inline uint32_t ew16(uint32_t a) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.ewram + ((a & 0x3FFFEu) ^ GBA_HX));
}
static inline uint32_t ew32(uint32_t a) { return gba_mem.ewram[(a & 0x3FFFCu) >> 2]; }
static inline uint32_t iw32(uint32_t a) { return gba_mem.iwram[(a & 0x7FFCu) >> 2]; }
static inline int is_ewram(uint32_t a, uint32_t len) {
    return a >= 0x02000000u && a + len <= 0x02040000u;
}
static inline uint32_t vram16(uint32_t o) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.vram + ((o & 0xFFFEu) ^ GBA_HX));
}
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* ------------------------------------------------------------ table hook */

static struct {
    uint32_t table;                     /* [View+0x6C] when recorded (0: never) */
    int32_t m0, m1, U, V, ip, lr;
} rec;

/* [[hook_before]] 0x08012612 (0x08012520, before the line loop): ip = r12,
 * lr = r14, U/V/m0/m1 at sp+0x14/0x18/0x1C/0x20 */
void ws_m1_hook_table(const uint32_t *regs) {
    uint32_t sp = regs[13];
    rec.ip = (int32_t)regs[12];
    rec.lr = (int32_t)regs[14];
    rec.U = (int32_t)rd32(sp + 0x14);
    rec.V = (int32_t)rd32(sp + 0x18);
    rec.m0 = (int32_t)rd32(sp + 0x1C);
    rec.m1 = (int32_t)rd32(sp + 0x20);
    rec.table = iw32(VIEW_TABLE);
}

/* The table entry of the game's loop for line L (56 <= L < 216), with the
 * products in 64 bits.  The game's 32-bit products of the reference point
 * wrap for some lines; the (r*A) >> 14 wrap moves the point by 1024 px,
 * the same texel on the 512 px wrapping map, but U*B and V*B start to wrap
 * below the native screen (B grows with the line) and would land anywhere.
 * So x and y equal the game's modulo 1024 px (checked against every line of
 * the native screen by ws_m1_debug_check), pa and pc exactly. */
static void line_entry(int L, uint32_t w[4]) {
    uint32_t k = (uint32_t)(L - FIRST_LINE);
    int64_t A = (int32_t)rd32(TAB_A + 4 * k), B = (int32_t)rd32(TAB_B + 4 * k);
    int32_t pa = (int32_t)((uint32_t)rec.m0 * (uint32_t)A) >> 14;
    int32_t pc = (int32_t)((uint32_t)rec.m1 * (uint32_t)A) >> 14;
    int64_t r4 = ((rec.U * B) >> 10) + rec.ip;
    int64_t r6 = ((rec.V * B) >> 10) + rec.lr;
    r4 = (r4 * A) >> 14;
    r6 = (r6 * A) >> 14;
    w[0] = (uint32_t)pa;
    w[1] = (uint32_t)pc;
    w[2] = (uint32_t)r4 - 120u * (uint32_t)pa;
    w[3] = (uint32_t)r6 - 120u * (uint32_t)pc;
}

/* ------------------------------------------------------------ ground streaming */

/* The ground's 64x64-cell VRAM map is a 512 px ring that 0x0801204E fills
 * each frame from the 3x3 decoded 256 px blocks around the camera, for the
 * camera rect View+0x0C..0x18 only.  0x08005DD4 makes that rect the bounding
 * box of the camera position, three points 256 px ahead (heading and +-20 of
 * 256 units) and two 64 px behind (+-82): the native screen's ground.  The
 * wider view also sees the far line's ends, 40 px further out on each side,
 * so while the view is shown the rect that 0x0801204E reads (its four loads,
 * game.toml) also covers points 256 px ahead and 165 px to the side.  It
 * stays inside the decoded blocks [base-256, base+512) on each axis (base:
 * the camera's 256 px block), so near a block edge, flying diagonally, the
 * outermost far corners can still be missing.  This only changes what is
 * streamed into VRAM (render state). */
#define SIN_TABLE 0x087FD104u           /* 256 x sin, 16.16 */
static int m1_requested;                /* frames since the platform asked for the view */

static int sin16(uint32_t a) { return (int32_t)rd32(SIN_TABLE + 4 * (a & 0xFF)); }

static void stream_rect(int r[4]) {
    uint32_t a = (uint32_t)((int32_t)iw32(VIEW_OBJ) >> 10);
    int px = (int32_t)iw32(VIEW_OBJ + 4) >> 10, py = (int32_t)iw32(VIEW_OBJ + 8) >> 10;
    /* far corners of the wide view: radius, angle offset (256 units) */
    static const int pts[4][2] = { { 304, 23 }, { 304, -23 }, { 280, 26 }, { 280, -26 } };
    for (int k = 0; k < 4; k++) {
        uint32_t b = a + (uint32_t)pts[k][1];
        int x = px + (int)(((int64_t)sin16(b) * pts[k][0]) >> 16);
        int y = py - (int)(((int64_t)sin16(b + 64) * pts[k][0]) >> 16);
        r[0] = imin(r[0], x); r[2] = imax(r[2], x + 1);
        r[1] = imin(r[1], y); r[3] = imax(r[3], y + 1);
    }
    int bx = (int)(((uint32_t)px >> 8) << 8), by = (int)(((uint32_t)py >> 8) << 8);
    r[0] = imax(r[0], bx - 256); r[2] = imin(r[2], bx + 511);
    r[1] = imax(r[1], by - 256); r[3] = imin(r[3], by + 511);
    if (r[2] - r[0] > 512) r[2] = r[0] + 512;
    if (r[3] - r[1] > 512) r[3] = r[1] + 512;
}

static int stream_wide(void);

static uint32_t stream_bound(int side, uint32_t v) {
    /* (the rect read is the View's: anything else is left alone) */
    if (!stream_wide() || iw32(VIEW_OBJ + 0xC + 4 * (uint32_t)side) != v)
        return v;
    int r[4] = { (int)iw32(VIEW_OBJ + 0xC), (int)iw32(VIEW_OBJ + 0x10), (int)iw32(VIEW_OBJ + 0x14),
                 (int)iw32(VIEW_OBJ + 0x18) };
    stream_rect(r);
    return (uint32_t)r[side];
}

/* [[override_load]] at 0x0801205A / 08012068 / 08012072 / 0801207C */
uint32_t ws_m1_rect_l(uint32_t v) { return stream_bound(0, v); }
uint32_t ws_m1_rect_t(uint32_t v) { return stream_bound(1, v); }
uint32_t ws_m1_rect_r(uint32_t v) { return stream_bound(2, v); }
uint32_t ws_m1_rect_b(uint32_t v) { return stream_bound(3, v); }

/* ------------------------------------------------------------ state */

static int m1_on;                       /* this frame uses the mode-1 view */

static int stream_wide(void) {
    return m1_on && m1_requested <= 2;
}

/* ------------------------------------------------------------ object culling */

/* [[override_reg]] r0 before 0x08012D94 (cmp r0, #240): the projected
 * top-left x of a ground object, r1 its width; visible natively for
 * -2w <= x <= 240.  In the wide view's extra 40 px it is moved to the
 * nearest accepted value (r0 is dead after the two compares). */
uint32_t ws_m1_cull_x(const uint32_t *r) {
    int32_t x = (int32_t)r[0], lim = -2 * (int32_t)r[1];
    if (!stream_wide())
        return r[0];
    if (x > 240 && x <= 280)
        return 240;
    if (x < lim && x >= lim - 40)
        return (uint32_t)lim;
    return r[0];
}

/* the same for y before 0x08012DD0 (cmp r0, #160), r6 = height */
uint32_t ws_m1_cull_y(const uint32_t *r) {
    int32_t y = (int32_t)r[0], lim = -2 * (int32_t)r[6];
    if (!stream_wide())
        return r[0];
    if (y > 160 && y <= 200)
        return 160;
    if (y < lim && y >= lim - 40)
        return (uint32_t)lim;
    return r[0];
}

/* 0x08016BD4's screen rect {-7,-7,240,160}: 40 px more on every side */
uint32_t ws_m1_srect_l(uint32_t v) { return stream_wide() ? v - 40 : v; }
uint32_t ws_m1_srect_t(uint32_t v) { return stream_wide() ? v - 40 : v; }
uint32_t ws_m1_srect_r(uint32_t v) { return stream_wide() ? v + 40 : v; }
uint32_t ws_m1_srect_b(uint32_t v) { return stream_wide() ? v + 40 : v; }
static int tab_ok;                      /* the recorded loop invariants describe the table */
static uint32_t sky_obj;                /* BG1 panorama layer object (0: none) */
static int sky_bg;
static int sky_h, sky_v;                /* its scroll */
/* BG0: haze band rows and the moved minimap */
static int hud_bg = -1;
static uint32_t band_rows;              /* bit r: native row r is a full-width band */
static int mm_on, mm_cx0, mm_cy0, mm_cx1, mm_cy1;  /* minimap cells (native, inclusive) */
static int mm_dx, mm_dy;                /* its move, in cells */
static uint16_t blank_entry;
static uint32_t mm_hash, generation;

static inline int32_t sext28(uint32_t v) { return (int32_t)(v << 4) >> 4; }

static int ws_m1_affine(int bg, int line, int32_t p[4]) {
    if (!m1_on || bg != 2)
        return 0;
    uint32_t w[4];
    if (line >= TAB_N + FIRST_LINE || line < -64)
        return 0;
    if (line >= 160) {
        if (!tab_ok)
            return 0;
        line_entry(line, w);
    } else {
        /* above the screen: the sky lines' filler, as on line 0 */
        const gba_line_regs_t *s = &gba_line_regs[0];
        w[0] = s->bg2pa; w[1] = s->bg2pc; w[2] = s->bg2x; w[3] = s->bg2y;
    }
    p[0] = sext28(w[2]);
    p[1] = sext28(w[3]);
    p[2] = (int16_t)w[0];
    p[3] = (int16_t)w[1];
    return 1;
}

/* the hardware map entry of text BG 'bg' at native-screen cell (cx,cy) */
static uint32_t hw_entry(int bg, int cx, int cy) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * bg);
    uint32_t sb = ((cnt >> 8) & 31) * 0x800u;
    uint32_t h = io16(REG_BG0HOFS + 4 * bg), v = io16(REG_BG0HOFS + 4 * bg + 2);
    uint32_t px = (h + (uint32_t)cx * 8) & 255, py = (v + (uint32_t)cy * 8) & 255;
    return vram16(sb + (py >> 3) * 64 + (px >> 3) * 2);
}

static int tile_blank(int bg, uint32_t entry) {
    uint32_t cnt = io16(REG_BG0CNT + 2 * bg);
    uint32_t cb = ((cnt >> 2) & 3) * 0x4000u;
    int bpp8 = (cnt & 0x80) != 0;
    uint32_t a = cb + (entry & 0x3FF) * (bpp8 ? 64u : 32u), n = bpp8 ? 16 : 8;
    if (a + n * 4 > 0x10000u)
        return 1;
    uint32_t acc = 0;
    for (uint32_t k = 0; k < n; k++)
        acc |= gba_mem.vram[(a >> 2) + k];
    return acc == 0;
}

static int ws_m1_bg_entry(int bg, int cx, int cy, uint16_t *e) {
    if (!m1_on)
        return 0;
    if (bg == sky_bg && sky_obj) {
        int wx = (int16_t)(uint16_t)((uint32_t)cx << 3), wy = (int16_t)(uint16_t)((uint32_t)cy << 3);
        if (wx + 8 > sky_h && wx < sky_h + 240 && wy + 8 > sky_v && wy < sky_v + 160)
            return 0;                   /* the hardware ring holds exactly this */
        int P = cx & 63, row = cy < 0 ? 0 : cy > 31 ? 31 : cy;
        *e = (uint16_t)ew16(sky_obj + 8 + (uint32_t)(P >> 5) * 0x800u + (uint32_t)row * 64 + (uint32_t)(P & 31) * 2);
        return 1;
    }
    if (bg == hud_bg) {
        /* BG0 does not scroll on this screen (checked in setup): world cell = screen cell */
        if (mm_on) {
            int sx = cx - mm_dx, sy = cy - mm_dy;
            if (sx >= mm_cx0 && sx <= mm_cx1 && sy >= mm_cy0 && sy <= mm_cy1) {
                *e = (uint16_t)hw_entry(bg, sx, sy);
                return 1;
            }
            if (cx >= mm_cx0 && cx <= mm_cx1 && cy >= mm_cy0 && cy <= mm_cy1) {
                *e = blank_entry;
                return 1;
            }
        }
        if (cx >= 0 && cx < 30 && cy >= 0 && cy < 20)
            return 0;
        if (cy >= 0 && cy < 20 && ((band_rows >> cy) & 1)) {
            *e = (uint16_t)hw_entry(bg, cx < 0 ? 0 : 29, cy);
            return 1;
        }
        *e = blank_entry;
        return 1;
    }
    return 0;
}

/* anchor() of widescreen.c: keep the fraction of the free space */
static int anchor(int p, int size, int native, int view, int inset) {
    int free_n = native - size, free_v = view - 2 * inset - size;
    if (free_n <= 0 || free_v <= 0)
        return (view - size) / 2 - p;
    return inset + (p * free_v + free_n / 2) / free_n - p;
}

/* BG0 of the world map: full-width band rows and the minimap box.  The
 * analysis is redone only when the layer's map entries on screen change
 * (their hash); the tiles' opacity is assumed to stay with the entries. */
static void setup_hud(int bg) {
    static uint32_t last_key = 0xFFFFFFFFu;
    static int last_bg = -1, last_hud = -1;
    uint32_t key = io16(REG_BG0CNT + 2 * bg) * 0x9E3779B1u ^ io16(REG_BG0HOFS + 4 * bg) ^
                   (io16(REG_BG0HOFS + 4 * bg + 2) << 16);
    {
        /* the 32x32 screen block (BG0 does not scroll here: checked below) */
        uint32_t sb = ((io16(REG_BG0CNT + 2 * bg) >> 8) & 31) * 0x800u;
        const uint32_t *m = gba_mem.vram + sb / 4;
        for (int w = 0; w < 20 * 16; w++)       /* rows 0..19, 2 entries per word */
            key = key * 31u + m[(w >> 4) * 16 + (w & 15)];
    }
    if (key == last_key && bg == last_bg) {
        hud_bg = last_hud;
        mm_on = hud_bg >= 0 && mm_cx1 >= 0;
        return;
    }
    last_key = key;
    last_bg = bg;
    last_hud = -1;
    hud_bg = -1;
    mm_on = 0;
    mm_cx1 = -1;
    band_rows = 0;
    if (io16(REG_BG0HOFS + 4 * bg) || io16(REG_BG0HOFS + 4 * bg + 2) || (io16(REG_BG0CNT + 2 * bg) & 0xC000))
        return;
    uint8_t opaque[20][30];
    int found_blank = 0;
    for (int cy = 0; cy < 20; cy++) {
        int all = 1;
        for (int cx = 0; cx < 30; cx++) {
            uint32_t e = hw_entry(bg, cx, cy);
            int b = tile_blank(bg, e);
            opaque[cy][cx] = (uint8_t)!b;
            if (b && !found_blank) {
                blank_entry = (uint16_t)e;
                found_blank = 1;
            }
            all &= !b;
        }
        if (all)
            band_rows |= 1u << cy;
    }
    if (!found_blank)
        return;                         /* nothing to blank the margins with */
    hud_bg = last_hud = bg;
    /* the minimap: the non-blank cells outside the bands, in the top rows */
    int x0 = 99, y0 = 99, x1 = -1, y1 = -1;
    uint32_t hsh = 0;
    for (int cy = 0; cy < 10; cy++) {
        if ((band_rows >> cy) & 1)
            continue;
        for (int cx = 0; cx < 30; cx++)
            if (opaque[cy][cx]) {
                x0 = imin(x0, cx); x1 = imax(x1, cx);
                y0 = imin(y0, cy); y1 = imax(y1, cy);
            }
    }
    if (x1 < 0)
        return;
    /* the box must not reach a band row (it would leave holes in it) */
    for (int cy = y0; cy <= y1; cy++)
        if ((band_rows >> cy) & 1)
            return;
    for (int cy = y0; cy <= y1; cy++)
        for (int cx = x0; cx <= x1; cx++)
            hsh = hsh * 31u + hw_entry(bg, cx, cy);
    int fx = anchor(x0 * 8, (x1 - x0 + 1) * 8, 240, 320, ws_hud_inset);
    int fy = anchor(y0 * 8, (y1 - y0 + 1) * 8, 160, 240, ws_hud_inset);
    /* framebuffer = native + f, screen = framebuffer + OXV; whole cells */
    mm_dx = (fx + OXV) >> 3;
    mm_dy = (fy + OYV) >> 3;
    mm_on = 1;
    mm_cx0 = x0; mm_cy0 = y0; mm_cx1 = x1; mm_cy1 = y1;
    hsh ^= (uint32_t)(x0 | (y0 << 8) | (x1 << 16) | (y1 << 24));
    if (hsh != mm_hash) {
        mm_hash = hsh;
        generation++;                   /* the moved cells' contents changed */
    }
}

static int detect(void) {
    uint32_t d = io16(REG_DISPCNT);
    if ((d & 0x87) != 1 || !(d & 0x400) || !gba_line_regs_valid)
        return 0;
    uint32_t g = iw32(LAYER_SET + 8);
    if (!is_ewram(g, 0x60) || ew32(g) != VT_M1_GROUND || iw32(IRQ_TABLE_HBL) != HBLANK_HANDLER)
        return 0;
    return 1;
}

void ws_m1_frame_begin(void) {
    int was = m1_on;
    if (m1_requested < 99)
        m1_requested++;
    m1_on = ws_enabled && detect();
    if (!m1_on) {
        if (was)
            generation++;
        return;
    }
    if (!was)
        generation++;
    tab_ok = rec.table && rec.table == iw32(VIEW_TABLE);
    sky_obj = 0;
    sky_bg = -1;
    hud_bg = -1;
    mm_on = 0;
    uint32_t d = io16(REG_DISPCNT);
    for (int bg = 0; bg < 2; bg++) {
        if (!(d & (0x100u << bg)))
            continue;
        uint32_t o = iw32(LAYER_SET + 4 * (uint32_t)bg);
        if (!is_ewram(o, 0x1008))
            continue;
        uint32_t vt = ew32(o);
        uint32_t cnt = io16(REG_BG0CNT + 2 * bg);
        if (vt == VT_M1_SKY && ((ew32(o + 4) & 0xFF) == (uint32_t)bg) && !(cnt & 0xC000)) {
            sky_obj = o;
            sky_bg = bg;
            sky_h = (int16_t)io16(REG_BG0HOFS + 4 * bg);
            sky_v = (int16_t)io16(REG_BG0HOFS + 4 * bg + 2);
        } else if (vt == VT_FIELD_LAYER && hud_bg < 0) {
            setup_hud(bg);
        }
    }
    ws_m1_sprites(OXV, OYV);
}

/* sprites: widescreen.c's field rules for a native-centred view, then the
 * world map's own classes */
static int ws_m1_obj_pos(int i, int *x, int *y) {
    uint32_t vt;
    if (!ws_m1_sprite(i, x, y, &vt))
        return 0;
    if (vt == VT_PLAYER || vt == VT_SHADOW || vt == VT_MARKER_A || vt == VT_MARKER_B) {
        uint32_t a0 = *(const uint16_t *)((const uint8_t *)gba_mem.oam + ((i * 8u) ^ GBA_HX));
        uint32_t a1 = *(const uint16_t *)((const uint8_t *)gba_mem.oam + ((i * 8u + 2) ^ GBA_HX));
        int rx = (int)(a1 & 0x1FF), ry = (int)(a0 & 0xFF);
        *x = rx >= 256 ? rx - 512 : rx;
        *y = ry >= 192 ? ry - 256 : ry;
        if (mm_on && (vt == VT_MARKER_A || vt == VT_MARKER_B)) {
            *x += mm_dx * 8;
            *y += mm_dy * 8;
        }
    }
    return 1;
}

int ws_m1_get_view(ws_view_t *v) {
    if (!m1_on)
        return 0;
    m1_requested = 0;
    v->ox = -OXV;
    v->oy = -OYV;
    v->x0 = OXV;
    v->y0 = OYV;
    v->x1 = OXV + WS_W;
    v->y1 = OYV + WS_H;
    v->bg_entry = ws_m1_bg_entry;
    v->obj_pos = ws_m1_obj_pos;
    v->bg_affine = ws_m1_affine;
    return 1;
}

uint32_t ws_m1_generation(void) {
    return generation;
}

/* check the recorded loop invariants against the game's own lines: returns
 * the number of lines 56..159 whose snapshot differs from the formula (-1:
 * nothing to check) */
int ws_m1_debug_check(void) {
    if (!m1_on || !tab_ok)
        return -1;
    int bad = 0;
    for (int L = FIRST_LINE; L < 160; L++) {
        uint32_t w[4];
        line_entry(L, w);
        const gba_line_regs_t *s = &gba_line_regs[L];
        /* reference point modulo 1024 px (20.8): the same texels of a map of
         * up to 1024 px */
        if (s->bg2pa != (uint16_t)w[0] || s->bg2pc != (uint16_t)w[1] ||
            ((s->bg2x ^ w[2]) & 0x3FFFFu) || ((s->bg2y ^ w[3]) & 0x3FFFFu))
            bad++;
    }
    return bad;
}

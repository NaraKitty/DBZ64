/*
 * widescreen.h - 320x240 field view (docs/WIDESCREEN.md).
 *
 * On field maps (towns, overworld, real-time battles) the game is shown as a
 * true 320x240 window onto the authored world.  Every other screen shows the
 * native 240x160 frame centred in 320x240 with a black border.
 *
 * Per frame the platform does:
 *
 *     ws_frame_begin();            // VBlank, before drawing (runtime/io.c does this)
 *     ws_view_t v;
 *     ws_get_view(&v);             // the view for this frame
 *     render(fb320x240, &v);       // ppu_soft_render_view() or the N64 renderer
 *
 * Coordinates.  "GBA screen coordinates" are the native screen's: (0,0) is the
 * top-left pixel of what the GBA shows, (239,159) the bottom-right.  The view
 * draws the GBA-coordinate region [x0,x1) x [y0,y1) with GBA pixel (0,0) at
 * framebuffer pixel (ox,oy):
 *
 *   native view:  ox=40, oy=40, region [0,240) x [0,160), no hooks
 *   field view:   ox=-OX, oy=-OY, region [OX,OX+320) x [OY,OY+240), hooks set,
 *                 where -80 <= OX <= 0 and -80 <= OY <= 0 (the native screen
 *                 always lies inside the view)
 *
 * Framebuffer pixels outside the region are black.  Inside it, everything is
 * drawn with ppu_soft's semantics except:
 *
 *   bg_entry(bg, cx, cy, &entry): text BG 'bg', world tile cell (cx, cy), where
 *     world pixel X = BGxHOFS (all 16 bits, as io16() returns them) + screen x,
 *     cx = X >> 3 (same for Y).  Returns 1 with the tilemap entry to use, or 0:
 *     use the hardware map with its normal wrap ((cx & map_w-1, cy & map_h-1)).
 *     Inside the native screen it always returns 0, so the native area is
 *     drawn from exactly the hardware state.  Outside, field layers come from
 *     the authored map chunks; cells off the map get a blank (all-zero) tile.
 *     Wrap layers (parallax skies, water) return 0: their hardware ring is the
 *     authored repeat.  Cheap enough to call for every cell of every frame.
 *
 *   obj_pos(i, &x, &y): returns 1 with the top-left of OAM entry i's bounding
 *     box (the double-size box for double-size affine sprites) in GBA screen
 *     coordinates, full signed values: draw the sprite there once, with no 9-bit
 *     x / 8-bit y wrap.  Returns 0 for disabled entries.  World objects get
 *     their exact position (recovered where the game packs the coordinates);
 *     HUD / dialogue objects are re-anchored toward the 320x240 edges.  Hidden
 *     objects get a position far outside the view.
 *
 * Both hooks are only valid between ws_frame_begin() and the next game
 * instruction (they read guest memory as it was at VBlank).
 *
 * ws_view_t has exactly the layout of gba_view_t (platform/n64/render/
 * gba_render.h), so the N64 platform can pass it on with a cast:
 *     _Static_assert(sizeof(gba_view_t) == sizeof(ws_view_t), "");
 *     ws_get_view((ws_view_t *)&gba_view);
 *
 * Culling.  The game only emits sprites inside its 240x160 camera rectangle.
 * tools/game.toml routes the loads of the camera bounds at the render-cull and
 * despawn sites through ws_cull_* / ws_despawn_*, which widen them to the
 * 320x240 view.  The widening is active only while ws_enabled is set, the
 * scene is a field map and the platform asked for the view with ws_get_view()
 * during the previous frame: a platform that keeps rendering natively gets
 * exactly the native game.
 */
#ifndef WIDESCREEN_H
#define WIDESCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_W 320
#define WS_H 240

typedef struct ws_view {
    int ox, oy;            /* framebuffer position of GBA screen pixel (0,0) */
    int x0, y0, x1, y1;    /* region to draw, GBA screen coordinates */
    int (*bg_entry)(int bg, int cx, int cy, uint16_t *entry);
    int (*obj_pos)(int i, int *x, int *y);
} ws_view_t;

/* runtime switches */
extern int ws_enabled;          /* 0: always the native view (default 1) */
extern int ws_widen_despawn;    /* widen the off-screen despawn tests too (default 1) */
extern int ws_hud_inset;        /* px between anchored HUD and the 320x240 edge (default 8) */
extern int ws_debug_flags;      /* WS_DBG_* (testing) */
#define WS_DBG_FORCE_DECODE 1   /* ignore the loaded chunk slots: always decode from ROM */

/* once per frame at VBlank, before rendering */
void ws_frame_begin(void);

/* the view for the current frame (native view unless the field view is on) */
void ws_get_view(ws_view_t *v);

/* the native view (240x160 at 40,40, no hooks) */
void ws_native_view(ws_view_t *v);

/* 1 while this frame uses the extended field view */
int ws_field_view_active(void);

/* ---- generated-code hooks (tools/game.toml) ---- */

/* [[override_load]]: camera bound loaded at a render-cull / despawn site */
uint32_t ws_cull_left(uint32_t v);
uint32_t ws_cull_right(uint32_t v);
uint32_t ws_cull_top(uint32_t v);
uint32_t ws_cull_bottom(uint32_t v);
uint32_t ws_despawn_left(uint32_t v);
uint32_t ws_despawn_right(uint32_t v);
uint32_t ws_despawn_top(uint32_t v);
uint32_t ws_despawn_bottom(uint32_t v);
/* constant native-screen rectangles (bounds in screen coordinates) */
uint32_t ws_scull_left(uint32_t v);
uint32_t ws_scull_right(uint32_t v);
uint32_t ws_scull_top(uint32_t v);
uint32_t ws_scull_bottom(uint32_t v);
/* the rect test 0x0802805C: widened only for the ROM screen rect 0x086E3604 */
void ws_hook_recttest(const uint32_t *regs);
uint32_t ws_rect_left(uint32_t v);
uint32_t ws_rect_right(uint32_t v);
uint32_t ws_rect_top(uint32_t v);
uint32_t ws_rect_bottom(uint32_t v);

/* [[hook_before]]: sprite build / emit / upload bracketing and the OAM
 * coordinate pack sites (regs = r0..r14) */
void ws_hook_oam_clear(const uint32_t *regs);
void ws_hook_emit_begin(const uint32_t *regs);
void ws_hook_emit_end(const uint32_t *regs);
void ws_hook_oam_upload(const uint32_t *regs);
void ws_hook_emit_guard(const uint32_t *regs);
uint32_t ws_emit_guard(uint32_t v);
void ws_px_r0(const uint32_t *regs);
void ws_px_r1(const uint32_t *regs);
void ws_px_r2(const uint32_t *regs);
void ws_px_r3(const uint32_t *regs);
void ws_px_r4(const uint32_t *regs);
void ws_px_r5(const uint32_t *regs);
void ws_px_r6(const uint32_t *regs);
void ws_py_r0(const uint32_t *regs);
void ws_py_r1(const uint32_t *regs);
void ws_py_r2(const uint32_t *regs);
void ws_py_r3(const uint32_t *regs);
void ws_py_r4(const uint32_t *regs);
void ws_py_r5(const uint32_t *regs);
void ws_py_r6(const uint32_t *regs);
void ws_py_r2_m256(const uint32_t *regs);
void ws_py_r3_m256(const uint32_t *regs);

/* ---- diagnostics (host) ---- */
typedef struct ws_stats {
    int field;              /* field view this frame */
    int OX, OY;             /* view offset from the native screen */
    int L, T, W, H;         /* camera (as shown) and map size */
    int slots_used;         /* OAM entries emitted by the last build */
    int slots_max;          /* max over the run */
    int slots_exact_x, slots_exact_y;   /* entries with recovered coordinates */
    int slots_screen;       /* entries classed as screen-space (HUD etc.) */
    int chunk_decodes;      /* authored chunks decoded (total) */
    int emit_skipped;       /* emits skipped by the OAM overflow guard (total) */
} ws_stats_t;
const ws_stats_t *ws_get_stats(void);
/* compare the authored map with the hardware map over the native screen;
 * returns the number of differing cells (should be 0) */
int ws_debug_check_bg(int *cells_checked);
/* decode one authored map chunk descriptor into out[0x800]; 1 = ok */
int ws_debug_decode(uint32_t desc, uint8_t *out);
/* print per-class sprite info of the current OAM (vt, cull, emit, slots) */
void ws_debug_dump_objs(void (*print)(const char *line));

#ifdef __cplusplus
}
#endif

#endif

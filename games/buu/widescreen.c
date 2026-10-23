/*
 * widescreen.c - Buu's Fury: native view only (for now).
 *
 * Implements the runtime/widescreen.h interface with the 240x160 frame
 * centred in 320x240 on every screen.  The field view (authored-map margins,
 * widened culling, HUD anchoring) needs the Buu's Fury sites researched for
 * it (camera object 0x030019A4, field-layer vtable 0x08049410, sprite build
 * 0x080115B0); until then game.toml has no widescreen hooks and nothing here
 * changes what the game does.
 */
#include <string.h>
#include "widescreen.h"

int ws_enabled = 1;
int ws_widen_despawn = 1;
int ws_hud_inset = 8;
int ws_debug_flags;

static ws_stats_t st;

void ws_frame_begin(void) {
}

void ws_native_view(ws_view_t *v) {
    memset(v, 0, sizeof *v);
    v->ox = 40;
    v->oy = 40;
    v->x0 = 0;
    v->y0 = 0;
    v->x1 = 240;
    v->y1 = 160;
}

void ws_get_view(ws_view_t *v) {
    ws_native_view(v);
}

int ws_field_view_active(void) {
    return 0;
}

uint32_t ws_map_generation(void) {
    return 0;
}

const ws_stats_t *ws_get_stats(void) {
    return &st;
}

int ws_debug_check_bg(int *cells_checked) {
    if (cells_checked)
        *cells_checked = 0;
    return 0;
}

int ws_debug_decode(uint32_t desc, uint8_t *out) {
    (void)desc;
    memset(out, 0, 0x800);
    return 0;
}

void ws_debug_dump_objs(void (*print)(const char *line)) {
    print("no field view for this game yet");
}

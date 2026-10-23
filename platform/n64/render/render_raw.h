/*
 * render_raw.h - raw RDP command emission for the renderer's hot paths.
 *
 * Texture loads and rectangles are written straight into a per-frame RDRAM
 * command buffer and handed to the RDP with rdpq_exec(); this is several
 * times cheaper on the CPU than one rdpq call per command.  Render modes,
 * TLUT loads and fills still go through rdpq: raw_flush() must be called
 * before any rdpq call so the command order is kept.  Every raw block ends
 * with SYNC_LOAD/SYNC_TILE/SYNC_PIPE because rdpq's autosync does not see
 * raw commands.
 */
#ifndef RENDER_RAW_H
#define RENDER_RAW_H

#include <stdint.h>

extern uint64_t *raw_ptr;
extern uint64_t *raw_end;

void raw_init(void);
void raw_frame_begin(void);
void raw_overflow(void);
/* submit the commands written since the last flush */
void raw_flush(void);

static inline void raw_reserve(int words) {
    if (__builtin_expect(raw_ptr + words + 4 > raw_end, 0))
        raw_overflow();
}
static inline void raw_put(uint64_t w) { *raw_ptr++ = w; }

/* GBA_RENDER_HWCHECK (test builds): report texture load start coordinates
 * outside [0, 1024): real hardware does not handle them like ares (the
 * Analogue 3D wraps LOAD_BLOCK's start row at 1024) */
#ifdef GBA_RENDER_HWCHECK
void raw_hwcheck_fail(const char *what, int a, int b);
#define RAW_HWCHECK(what, a, b) \
    do { if ((unsigned)(a) >= 1024u || (unsigned)(b) >= 1024u) raw_hwcheck_fail(what, a, b); } while (0)
#else
#define RAW_HWCHECK(what, a, b) ((void)0)
#endif

#define RAW_SYNC_LOAD  (0x26ull << 56)
#define RAW_SYNC_PIPE  (0x27ull << 56)
#define RAW_SYNC_TILE  (0x28ull << 56)

/* RDP image formats */
enum { RF_RGBA = 0, RF_CI = 2, RF_IA = 3, RF_I = 4 };
enum { RS_4 = 0, RS_8 = 1, RS_16 = 2, RS_32 = 3 };

static inline uint64_t raw_tex_image(int fmt, int size, int width, uint32_t paddr) {
    return (0x3Dull << 56) | ((uint64_t)fmt << 53) | ((uint64_t)size << 51) |
           ((uint64_t)(width - 1) << 32) | (paddr & 0x3FFFFFF);
}
/* line and tmem in bytes (multiples of 8) */
static inline uint64_t raw_set_tile(int fmt, int size, int line, int tmem, int tile, int pal) {
    return (0x35ull << 56) | ((uint64_t)fmt << 53) | ((uint64_t)size << 51) |
           ((uint64_t)(line >> 3) << 41) | ((uint64_t)(tmem >> 3) << 32) |
           ((uint64_t)tile << 24) | ((uint64_t)pal << 20) | (1ull << 19) | (1ull << 9);
}
/* texel coordinates, s1/t1 exclusive */
static inline uint64_t raw_load_tile(int tile, int s0, int t0, int s1, int t1) {
    RAW_HWCHECK("LOAD_TILE", s0, t0);
    return (0x34ull << 56) | ((uint64_t)(s0 * 4) << 44) | ((uint64_t)(t0 * 4) << 32) |
           ((uint64_t)tile << 24) | ((uint64_t)((s1 - 1) * 4) << 12) | (uint64_t)((t1 - 1) * 4);
}
static inline uint64_t raw_set_tile_size(int tile, int s0, int t0, int s1, int t1) {
    return (0x32ull << 56) | ((uint64_t)(s0 * 4) << 44) | ((uint64_t)(t0 * 4) << 32) |
           ((uint64_t)tile << 24) | ((uint64_t)((s1 - 1) * 4) << 12) | (uint64_t)((t1 - 1) * 4);
}
/* LOAD_BLOCK of 'texels' texels starting at image texel (s0,t0), TMEM line of 'line_bytes' */
static inline uint64_t raw_load_block(int tile, int s0, int t0, int texels, int line_bytes) {
    RAW_HWCHECK("LOAD_BLOCK", s0, t0);
    uint32_t words = (uint32_t)line_bytes / 8;
    uint32_t dxt = (2048 + words - 1) / words;
    return (0x33ull << 56) | ((uint64_t)s0 << 44) | ((uint64_t)t0 << 32) |
           ((uint64_t)tile << 24) | ((uint64_t)(texels - 1) << 12) | dxt;
}

/* Which raw drawing path last configured SET_TEXTURE_IMAGE and tiles 0/1;
 * a path re-emits its setup when it is not the owner. */
enum { RAW_OWNER_NONE = 0, RAW_OWNER_OBJ = 1 };
extern int raw_owner;

#endif

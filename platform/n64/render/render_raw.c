/*
 * render_raw.c - per-frame raw RDP command buffer (see render_raw.h).
 */
#include <libdragon.h>
#include <malloc.h>
#include "render_raw.h"

#define RAW_WORDS (16 * 1024)           /* 128KB per frame */

static uint64_t *raw_mem, *raw_start;
uint64_t *raw_ptr, *raw_end;
int raw_owner;

void raw_init(void) {
    /* uncached: commands are written once and read by the RDP; cached writes
     * would cost a line fill per 16 bytes plus the writeback */
    raw_mem = malloc_uncached_aligned(16, RAW_WORDS * 8);
    raw_end = raw_mem + RAW_WORDS;
    raw_frame_begin();
}

/* the previous frame's RDP work must be finished before this is called */
void raw_frame_begin(void) {
    raw_ptr = raw_start = raw_mem;
    raw_owner = RAW_OWNER_NONE;
}

void raw_flush(void) {
    if (raw_ptr == raw_start)
        return;
    /* rdpq's autosync does not track raw commands: leave the RDP synced */
    raw_put(RAW_SYNC_LOAD);
    raw_put(RAW_SYNC_TILE);
    raw_put(RAW_SYNC_PIPE);
    raw_owner = RAW_OWNER_NONE;         /* rdpq calls may follow and change tiles */
    int bytes = (int)((raw_ptr - raw_start) * 8);
    rdpq_exec(raw_start, bytes);
    raw_start = raw_ptr;
}

#ifdef GBA_RENDER_HWCHECK
void raw_hwcheck_fail(const char *what, int a, int b) {
    static int shown;
    if (shown++ < 20)
        debugf("ERROR HWCHECK %s start coordinate out of [0,1024): %d %d\n", what, a, b);
}
#endif

/* buffer full (very large frames): wait until the RDP consumed it, then reuse */
void raw_overflow(void) {
    raw_flush();
    rspq_wait();
    raw_frame_begin();
}

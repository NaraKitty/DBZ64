/*
 * main.c - N64 platform layer: boot, video, audio, controller, saves.
 *
 * The recompiled game runs on its own thread (deep guest call chains become
 * deep C recursion, more than libdragon's 64KB main stack).  Each guest
 * VBlank calls plat_frame(), which draws the frame, pushes audio, polls the
 * controller and flushes saves; display_get() blocks on vsync, which paces
 * the game at the N64's refresh rate.
 *
 * Test mode: if rom:/test.txt exists the game runs a scripted session and
 * exits through emux (ares).  Format, one item per line:
 *   frames N          stop after N frames
 *   shot F            dump the framebuffer (FBDUMP hex via debugf) at frame F
 *   keys A-B:KEY+KEY  hold GBA keys during frames A..B (a b select start right
 *                     left up down r l), same syntax as the host harness
 */
#include <libdragon.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "gba.h"
#include "io.h"
#include "rom.h"
#include "rom_n64.h"
#include "tlb.h"
#include "prof.h"
#include "ppu_soft.h"
#include "game.h"

void gba_boot(void);

/* ------------------------------------------------------------ config */

#define ROM_CACHE_BYTES  (2 * 1024 * 1024)
#define GAME_STACK       (256 * 1024)
/* the GBA produces 267.52 samples per frame; we show one GBA frame per N64
 * refresh, so the audio rate follows the N64 refresh rate */
#define AUDIO_SAMPLES_PER_FRAME 267.52f

/* ------------------------------------------------------------ logging */

void plat_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    debugf("%s\n", buf);
}

/* ------------------------------------------------------------ test mode */

typedef struct { int from, to; uint16_t keys; } keyspan_t;

static struct {
    int active;
    int nodraw;          /* skip drawing except on shot frames (fast runs) */
    int frameskip;       /* use the real-time frame skip (off in test runs by default) */
    int slow_us;         /* burn this much CPU per frame (imitates slower hardware) */
    int profile_from;    /* start the sampling profiler at this frame */
    int frames;
    int shots[64], nshots;
    keyspan_t spans[2048];
    int nspans, spans_dropped;
} test;

static uint16_t parse_keys(const char *s) {
    static const char *names[10] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l" };
    uint16_t pressed = 0;
    char buf[32];
    while (*s && *s != '\n') {
        int n = 0;
        while (*s && *s != '+' && *s != '\n' && n < 31)
            buf[n++] = *s++;
        buf[n] = 0;
        if (*s == '+')
            s++;
        for (int i = 0; i < 10; i++)
            if (!strcmp(buf, names[i]))
                pressed |= (uint16_t)(1 << i);
    }
    return pressed;
}

static void load_test_script(void) {
    FILE *f = fopen("rom:/test.txt", "r");
    if (!f)
        return;
    test.active = 1;
    test.frames = 600;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int a, b;
        char k[96];
        if (sscanf(line, "frames %d", &a) == 1) {
            test.frames = a;
        } else if (!strncmp(line, "nodraw", 6)) {
            test.nodraw = 1;
        } else if (!strncmp(line, "frameskip", 9)) {
            test.frameskip = 1;
        } else if (sscanf(line, "slow %d", &a) == 1) {
            test.slow_us = a;
        } else if (sscanf(line, "profile %d", &a) == 1) {
            test.profile_from = a;
        } else if (sscanf(line, "shot %d", &a) == 1) {
            if (test.nshots < 64)
                test.shots[test.nshots++] = a;
        } else if (sscanf(line, "keys %d-%d:%95s", &a, &b, k) == 3) {
            if (test.nspans < 2048)
                test.spans[test.nspans++] = (keyspan_t){ a, b, parse_keys(k) };
            else
                test.spans_dropped++;
        } else if (sscanf(line, "keys %d:%95s", &a, k) == 2) {
            if (test.nspans < 2048)
                test.spans[test.nspans++] = (keyspan_t){ a, a, parse_keys(k) };
            else
                test.spans_dropped++;
        }
    }
    fclose(f);
    debugf("test mode: %d frames, %d shots, %d key spans\n", test.frames, test.nshots, test.nspans);
    if (test.spans_dropped)
        debugf("test mode: WARNING %d key spans over the limit were ignored\n", test.spans_dropped);
}

static void fb_dump(int frame, surface_t *s) {
    static char line[16 + 320 * 4 + 2];
    const uint16_t *fb = (const uint16_t *)UncachedAddr(s->buffer);
    debugf("FBDUMP f%05d %d %d\n", frame, s->width, s->height);
    for (int y = 0; y < s->height; y++) {
        int n = sprintf(line, "FB %d ", y);
        for (int x = 0; x < s->width; x++)
            n += sprintf(line + n, "%04x", fb[y * (s->stride / 2) + x]);
        line[n++] = '\n';
        line[n] = 0;
        debugf("%s", line);
    }
    debugf("FBEND f%05d\n", frame);
}

/* ------------------------------------------------------------ input */

static uint16_t keyinput = 0x3FF;
static int frame;

/* C-Up toggles an on-screen performance overlay */
static int overlay;

static uint16_t read_controller(void) {
    joypad_poll();
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    joypad_buttons_t b = in.btn;
    if (joypad_get_buttons_pressed(JOYPAD_PORT_1).c_up)
        overlay ^= 1;
    const int T = 40;       /* analog stick threshold (stick range ~ +-85) */
    uint16_t k = 0;
    if (b.a) k |= 1 << 0;
    if (b.b) k |= 1 << 1;
    if (b.c_down) k |= 1 << 2;                    /* Select */
    if (b.start) k |= 1 << 3;
    if (b.d_right || in.stick_x > T) k |= 1 << 4;
    if (b.d_left || in.stick_x < -T) k |= 1 << 5;
    if (b.d_up || in.stick_y > T) k |= 1 << 6;
    if (b.d_down || in.stick_y < -T) k |= 1 << 7;
    if (b.r) k |= 1 << 8;
    if (b.l || b.z) k |= 1 << 9;                  /* L and Z both act as GBA L */
    return k;
}

uint16_t plat_keyinput(void) {
    return keyinput;
}

/* ------------------------------------------------------------ audio */

/* ring of stereo frames; blocks are 16 frames and never straddle the end */
#define ABUF_FRAMES 4096u
static int16_t abuf[ABUF_FRAMES * 2];
static uint32_t a_rd, a_wr;   /* frame counters; a_wr - a_rd are queued */

static uint32_t stat_audio_blocks, stat_audio_drops;

void plat_audio_block(const int8_t *left, const int8_t *right) {
    stat_audio_blocks++;
    if (a_wr - a_rd + 16 > ABUF_FRAMES) {
        stat_audio_drops++;
        return;   /* the AI is not draining (should not happen) */
    }
    int16_t *o = abuf + (a_wr & (ABUF_FRAMES - 1)) * 2;
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (int16_t)(left[i] * 256);
        o[2 * i + 1] = (int16_t)(right[i] * 256);
    }
    a_wr += 16;
}

static void audio_flush(void) {
    while (a_rd != a_wr) {
        uint32_t pos = a_rd & (ABUF_FRAMES - 1);
        uint32_t n = a_wr - a_rd;
        if (n > ABUF_FRAMES - pos)
            n = ABUF_FRAMES - pos;
        int pushed = audio_push(abuf + pos * 2, (int)n, false);
        if (pushed <= 0)
            break;
        a_rd += (uint32_t)pushed;
        if ((uint32_t)pushed < n)
            break;
    }
    /* keep latency bounded if the AI falls behind */
    if (a_wr - a_rd > ABUF_FRAMES / 2)
        a_rd = a_wr;
}

/* ------------------------------------------------------------ saves */

/* SRAM layout: 16-byte header {GAME_SRAM_MAGIC, version, crc32, 0} + 8KB
 * EEPROM image, mirrored at 0x4000 so a power cut during a write keeps one
 * copy.  The magic is per game, so one game never loads the other's save. */
static int save_dirty;

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static int load_copy(uint32_t off) {
    static uint8_t buf[16 + sizeof gba_eeprom_data] __attribute__((aligned(16)));
    sram_read(buf, off, sizeof buf);
    uint32_t ver, crc;
    memcpy(&ver, buf + 4, 4);
    memcpy(&crc, buf + 8, 4);
    if (memcmp(buf, GAME_SRAM_MAGIC, 4) || ver != 1 || crc != crc32_buf(buf + 16, sizeof gba_eeprom_data))
        return 0;
    memcpy(gba_eeprom_data, buf + 16, sizeof gba_eeprom_data);
    return 1;
}

static void load_save(void) {
    memset(gba_eeprom_data, 0xFF, sizeof gba_eeprom_data);
    if (!load_copy(0) && !load_copy(0x4000))
        debugf("save: no valid SRAM image, starting blank\n");
}

static void flush_save(void) {
    static uint8_t buf[16 + sizeof gba_eeprom_data] __attribute__((aligned(16)));
    if (!save_dirty)
        return;
    save_dirty = 0;
    uint32_t ver = 1, crc = crc32_buf(gba_eeprom_data, sizeof gba_eeprom_data), z = 0;
    memcpy(buf, GAME_SRAM_MAGIC, 4);
    memcpy(buf + 4, &ver, 4);
    memcpy(buf + 8, &crc, 4);
    memcpy(buf + 12, &z, 4);
    memcpy(buf + 16, gba_eeprom_data, sizeof gba_eeprom_data);
    sram_write(buf, 0, sizeof buf);
    sram_write(buf, 0x4000, sizeof buf);
}

void plat_save_block(uint32_t block) {
    (void)block;
    save_dirty = 1;
}

/* ------------------------------------------------------------ overlay */

static char overlay_text[2][64];
static uint64_t ov_t0, ov_game, ov_draw;
static int ov_frames, ov_drawn;
static uint32_t ov_misses;

/* line 1: game speed (guest frames per second) and frames actually drawn;
 * line 2: average CPU time of the game and of a drawn frame, ROM cache
 * misses (each one stalls on a cartridge read) in the last 30 frames and
 * since boot */
static void overlay_account(uint64_t game, uint64_t draw, int drawn) {
    ov_game += game;
    ov_draw += draw;
    ov_drawn += drawn;
    if (++ov_frames == 30) {
        uint64_t now = get_ticks();
        double secs = (double)TICKS_TO_US(now - ov_t0) / 1e6;
        uint32_t m = rom_cache_stats.misses;
        snprintf(overlay_text[0], sizeof overlay_text[0], "%4.1f fps  %4.1f drawn",
                 ov_t0 ? 30.0 / secs : 0.0, ov_t0 ? ov_drawn / secs : 0.0);
        snprintf(overlay_text[1], sizeof overlay_text[1], "game %4.1f draw %4.1f ms  rom %lu/%lu",
                 (double)TICKS_TO_US(ov_game) / 30000.0,
                 ov_drawn ? (double)TICKS_TO_US(ov_draw) / 1000.0 / ov_drawn : 0.0,
                 (unsigned long)(m - ov_misses), (unsigned long)m);
        ov_misses = m;
        ov_t0 = now;
        ov_game = ov_draw = 0;
        ov_frames = ov_drawn = 0;
    }
}

/* CPU text over a finished frame (the RDP must be done with it) */
static void overlay_draw(surface_t *disp) {
    graphics_draw_box(disp, 0, 0, 320, 22, graphics_make_color(0, 0, 0, 255));
    graphics_set_color(graphics_make_color(255, 255, 0, 255), 0);
    graphics_draw_text(disp, 4, 2, overlay_text[0]);
    graphics_draw_text(disp, 4, 12, overlay_text[1]);
}

/* ------------------------------------------------------------ video */

#ifndef USE_RDP
static uint16_t gba_fb[240 * 160];

static void draw_frame_cpu(surface_t *disp) {
    ppu_soft_render(gba_fb, 240);
    /* display surfaces are uncached, so the CPU writes land in RDRAM directly */
    uint16_t *dst = (uint16_t *)disp->buffer;
    int pitch = disp->stride / 2;
    for (int y = 0; y < 240; y++)
        memset(dst + y * pitch, 0, 320 * 2);
    for (int y = 0; y < 160; y++) {
        uint16_t *d = dst + (y + 40) * pitch + 40;
        const uint16_t *s = gba_fb + y * 240;
        for (int x = 0; x < 240; x++) {
            uint16_t c = s[x];
            d[x] = (uint16_t)(((c & 31) << 11) | (((c >> 5) & 31) << 6) | (((c >> 10) & 31) << 1) | 1);
        }
    }
}
#endif

#ifdef USE_RDP
#include <stddef.h>
#include "gba_render.h"
#include "widescreen.h"

_Static_assert(sizeof(gba_view_t) == sizeof(ws_view_t) &&
               offsetof(gba_view_t, bg_entry) == offsetof(ws_view_t, bg_entry) &&
               offsetof(gba_view_t, obj_pos) == offsetof(ws_view_t, obj_pos),
               "ws_view_t must have the layout of gba_view_t");

/* The 320x240 field view (docs/WIDESCREEN.md), asked for every frame even
 * when nothing is drawn: asking is what widens the game's culling. */
static gba_view_t frame_view;

static void get_frame_view(void) {
    static uint32_t map_gen;
    ws_get_view((ws_view_t *)&frame_view);
    if (ws_map_generation() != map_gen) {
        /* margin tiles cached from the previous map are stale */
        map_gen = ws_map_generation();
        for (int bg = 0; bg < 4; bg++)
            gba_render_invalidate_bg(bg);
    }
}

/* RDP renderer: enqueue the frame; the RDP finishes it while the game runs
 * the next frame.  Returns after the frame is queued for display. */
static void draw_frame_rdp(surface_t *disp, int dump) {
    rdpq_attach(disp, NULL);
    gba_render_frame(disp, &frame_view);
    if (dump || overlay) {
        rdpq_detach_wait();
        if (overlay)
            overlay_draw(disp);
        if (dump)
            fb_dump(frame, disp);
        display_show(disp);
    } else {
        rdpq_detach_show();
    }
}
#endif

/* ------------------------------------------------------------ frame */

static uint64_t t_last;
/* per-phase CPU time over the reporting window: guest code (everything
 * between frames: game logic, IRQ handlers, mixing), waiting for a free
 * framebuffer (vsync), and drawing */
static uint64_t t_frame_end, acc_game, acc_wait, acc_draw;

/* Real-time frame skip.  Guest time advances one GBA frame per plat_frame,
 * so a frame that misses its vsync would slow the game down and starve the
 * audio.  When the game is more than half a frame behind real time, the
 * frame is not drawn (the VI keeps showing the previous one) so the game
 * catches up; at most MAX_SKIP frames in a row are skipped. */
#define FRAME_TICKS ((uint64_t)TICKS_PER_SECOND / 60)
#define MAX_SKIP 2
static uint64_t deadline;      /* real time at which this frame is due */
static int skip_run;
static uint32_t stat_skipped;

__attribute__((unused)) static int frame_should_skip(uint64_t now) {
    deadline = deadline ? deadline + FRAME_TICKS : now;
    int64_t late = (int64_t)(now - deadline);
    if (late > (int64_t)(4 * FRAME_TICKS)) {
        /* a long stall (loading): don't try to catch up with it */
        deadline = now;
        late = 0;
    }
    if (late > (int64_t)(FRAME_TICKS / 2) && skip_run < MAX_SKIP) {
        skip_run++;
        return 1;
    }
    skip_run = 0;
    return 0;
}

/* display_get() had to wait for a free buffer: the game is ahead of the
 * display, so this vsync is its schedule (keeps the clock from drifting
 * against the VI rate) */
__attribute__((unused)) static void frame_synced(uint64_t before, uint64_t after) {
    if (after - before > TICKS_FROM_US(2000))
        deadline = after;
}

void plat_frame(void) {
    uint64_t t0 = get_ticks();
    prof_watchdog_kick();
    if (t_frame_end)
        acc_game += t0 - t_frame_end;
    frame++;

    keyinput = (uint16_t)(0x3FF & ~read_controller());
    if (test.active) {
        uint16_t pressed = 0;
        for (int i = 0; i < test.nspans; i++)
            if (frame >= test.spans[i].from && frame <= test.spans[i].to)
                pressed |= test.spans[i].keys;
        keyinput = (uint16_t)(0x3FF & ~pressed);
        if (test.slow_us)
            wait_ticks(TICKS_FROM_US(test.slow_us));
    }

    uint64_t t1 = get_ticks();
    int shot_now = 0;
    for (int i = 0; i < test.nshots; i++)
        if (test.shots[i] == frame)
            shot_now = 1;
#ifdef USE_RDP
    get_frame_view();
    int drawn = 0;
    uint64_t t2 = t1;
    if (test.nodraw && !shot_now) {
        /* fast test runs: nothing drawn, still paced by the display */
        surface_t *disp = display_get();
        t2 = get_ticks();
        display_show(disp);
    } else if ((test.active && !test.frameskip) || shot_now || !frame_should_skip(t1)) {
        surface_t *disp = display_get();
        t2 = get_ticks();
        frame_synced(t1, t2);
        draw_frame_rdp(disp, test.active && shot_now);
        drawn = 1;
    } else {
        stat_skipped++;   /* the VI keeps showing the previous frame */
    }
    uint64_t t3 = get_ticks();
#else
    int drawn = 1;
    surface_t *disp = display_get();
    uint64_t t2 = get_ticks();
    if (!test.nodraw || shot_now)
        draw_frame_cpu(disp);
    uint64_t t3 = get_ticks();
    if (overlay)
        overlay_draw(disp);
    if (test.active && shot_now)
        fb_dump(frame, disp);
    display_show(disp);
#endif
    overlay_account(t_frame_end ? t0 - t_frame_end : 0, t3 - t2, drawn);
    acc_wait += t2 - t1;
    acc_draw += t3 - t2;

    audio_flush();
    flush_save();

    if ((frame % 120) == 0) {
        uint64_t now = get_ticks();
        debugf("f%d: %.2f ms/frame (game %.2f, vsync wait %.2f, draw %.2f) rom misses %lu\n", frame,
               (double)TICKS_TO_US(now - t_last) / 120000.0, (double)TICKS_TO_US(acc_game) / 120000.0,
               (double)TICKS_TO_US(acc_wait) / 120000.0, (double)TICKS_TO_US(acc_draw) / 120000.0,
               (unsigned long)rom_cache_stats.misses);
        debugf("f%d: audio blocks %lu dropped %lu queued %d; frames skipped %lu\n", frame,
               (unsigned long)stat_audio_blocks, (unsigned long)stat_audio_drops, (int)(a_wr - a_rd),
               (unsigned long)stat_skipped);
        stat_audio_blocks = stat_audio_drops = 0;
        stat_skipped = 0;
        debugf("f%d: tlb refills %lu emulated %lu (last va %08lX pc %08lX) vram marks %lu\n", frame,
               (unsigned long)tlb_stats.rom_refills, (unsigned long)tlb_stats.emulated,
               (unsigned long)tlb_stats.last_va, (unsigned long)tlb_stats.last_epc,
               (unsigned long)tlb_stats.vram_marks);
#ifdef USE_RDP
        const gba_render_stats_t *rs = gba_render_get_stats();
        debugf("f%d: render us total %lu wait %lu track %lu bg %lu obj %lu raster %lu; "
               "rects %lu obj %lu field %d\n", frame,
               (unsigned long)rs->us_total, (unsigned long)rs->us_wait, (unsigned long)rs->us_track,
               (unsigned long)rs->us_bg, (unsigned long)rs->us_obj, (unsigned long)rs->us_raster,
               (unsigned long)rs->rects, (unsigned long)rs->obj_drawn, ws_field_view_active());
#endif
        acc_game = acc_wait = acc_draw = 0;
        t_last = now;
    }

    if (test.active && test.profile_from == frame)
        prof_start(500);
    if (test.active && frame >= test.frames) {
        prof_report();
        debugf("test done at frame %d\n", frame);
        emux_ioctl_exit();
        for (;;) { }
    }
    t_frame_end = get_ticks();
}

void plat_sleep(void) {
    /* GBA sleep: wait until L+R+Select (L/Z + R + C-Down) is held */
    for (;;) {
        surface_t *disp = display_get();
        memset(disp->buffer, 0, (size_t)disp->stride * disp->height);
        display_show(disp);
        uint16_t k = read_controller();
        if ((k & 0x304) == 0x304)
            break;
    }
}

/* ------------------------------------------------------------ boot */

/* Deep guest call chains become deep C recursion; libdragon's main stack is
 * 64KB, so the game runs on its own stack.  gba_boot() never returns. */
static void __attribute__((noreturn)) run_game_on_stack(void *top) {
    __asm__ volatile(
        "move $sp, %0\n"
        "jal  gba_boot\n"
        "nop\n"
        :: "r"((uintptr_t)top - 64) : "memory");
    __builtin_unreachable();
}

/* Show an error on screen forever (real hardware has no debug console). */
void plat_fatal(const char *msg) {
    debugf("FATAL: %s\n", msg);
    for (;;) {
        surface_t *d = display_get();
        graphics_fill_screen(d, graphics_make_color(0, 0, 64, 255));
        graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
        graphics_draw_text(d, 16, 16, GAME_TITLE);
        graphics_draw_text(d, 16, 28, "N64 port - cannot continue:");
        /* wrap the message at 36 columns */
        char line[40];
        int y = 52, n = 0;
        for (const char *p = msg;; p++) {
            if (*p == '\n' || *p == 0 || n == 36) {
                line[n] = 0;
                graphics_draw_text(d, 16, y, line);
                y += 12;
                n = 0;
                if (!*p)
                    break;
                if (*p != '\n')
                    line[n++] = *p;
                continue;
            }
            line[n++] = *p;
        }
        display_show(d);
    }
}

int main(void) {
    debug_init_emulog();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_DISABLED);
    if (!is_memory_expanded())
        plat_fatal("This game needs 8MB of RAM (Expansion Pak).\n\n"
                   "Analogue 3D: open this game's settings in the Library "
                   "and turn the Expansion Pak on, then restart.\n\n"
                   "N64: insert an Expansion Pak.");
    rdpq_init();
#ifdef USE_RDP
    gba_render_init();
#endif
    joypad_init();
    audio_init(16004, AUDIO_INIT_LATENCY_MS(100));
    if (dfs_init(DFS_DEFAULT_LOCATION) != DFS_ESUCCESS)
        plat_fatal("The ROM's filesystem (with the GBA game data) was not found. "
                   "Rebuild the ROM with your own GBA dump.");
    sram_init();
    debugf(GAME_SHORT_TITLE " N64: memory %d bytes, audio %d Hz\n", get_memory_size(), audio_get_frequency());
    load_test_script();
    if (test.active)
        prof_watchdog_start();
    load_save();
    rom_cache_init(ROM_CACHE_BYTES);
    rom_cache_preload();
    debugf("rom cache: %lu pages preloaded\n", (unsigned long)rom_cache_stats.preloaded);
#ifdef GBA_TLB
    tlb_init();
    const char *bad = tlb_selftest();
    if (bad) {
        static char msg[160];
        snprintf(msg, sizeof msg, "Hardware self-test failed: %s.\n\n"
                 "This console/FPGA handles the CPU's TLB differently from an N64.", bad);
        plat_fatal(msg);
    }
    if (!gba_vram_track_ok)
        debugf("self-test: TLB modification exceptions unavailable, VRAM tracking off\n");
#endif
    t_last = get_ticks();
    uint8_t *stack = memalign(16, GAME_STACK);
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    debugf("heap: %d of %d bytes used, %d free\n", hs.used, hs.total, hs.total - hs.used);
    run_game_on_stack(stack + GAME_STACK);
}

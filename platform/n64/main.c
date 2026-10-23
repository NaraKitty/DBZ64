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
#include "ppu_soft.h"

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
    int frames;
    int shots[64], nshots;
    keyspan_t spans[256];
    int nspans;
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
        } else if (sscanf(line, "shot %d", &a) == 1) {
            if (test.nshots < 64)
                test.shots[test.nshots++] = a;
        } else if (sscanf(line, "keys %d-%d:%95s", &a, &b, k) == 3) {
            if (test.nspans < 256)
                test.spans[test.nspans++] = (keyspan_t){ a, b, parse_keys(k) };
        } else if (sscanf(line, "keys %d:%95s", &a, k) == 2) {
            if (test.nspans < 256)
                test.spans[test.nspans++] = (keyspan_t){ a, a, parse_keys(k) };
        }
    }
    fclose(f);
    debugf("test mode: %d frames, %d shots, %d key spans\n", test.frames, test.nshots, test.nspans);
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

static uint16_t read_controller(void) {
    joypad_poll();
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    joypad_buttons_t b = in.btn;
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

#define ABUF_FRAMES 4096
static int16_t abuf[ABUF_FRAMES * 2];
static int alen;   /* stereo frames queued */

void plat_audio_block(const int8_t *left, const int8_t *right) {
    if (alen + 16 > ABUF_FRAMES)
        return;   /* the AI is not draining (should not happen) */
    int16_t *o = abuf + alen * 2;
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (int16_t)(left[i] * 256);
        o[2 * i + 1] = (int16_t)(right[i] * 256);
    }
    alen += 16;
}

static void audio_flush(void) {
    if (!alen)
        return;
    int pushed = audio_push(abuf, alen, false);
    if (pushed > 0 && pushed < alen)
        memmove(abuf, abuf + pushed * 2, (size_t)(alen - pushed) * 4);
    if (pushed > 0)
        alen -= pushed;
    /* keep latency bounded if the AI falls behind */
    if (alen > ABUF_FRAMES / 2)
        alen = 0;
}

/* ------------------------------------------------------------ saves */

/* SRAM layout: 16-byte header {"DBZ2", version, crc32, 0} + 8KB EEPROM
 * image, mirrored at 0x4000 so a power cut during a write keeps one copy. */
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
    if (memcmp(buf, "DBZ2", 4) || ver != 1 || crc != crc32_buf(buf + 16, sizeof gba_eeprom_data))
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
    memcpy(buf, "DBZ2", 4);
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

/* ------------------------------------------------------------ video */

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

/* ------------------------------------------------------------ frame */

static uint64_t t_last;

void plat_frame(void) {
    frame++;

    keyinput = (uint16_t)(0x3FF & ~read_controller());
    if (test.active) {
        uint16_t pressed = 0;
        for (int i = 0; i < test.nspans; i++)
            if (frame >= test.spans[i].from && frame <= test.spans[i].to)
                pressed |= test.spans[i].keys;
        keyinput = (uint16_t)(0x3FF & ~pressed);
    }

    surface_t *disp = display_get();
    draw_frame_cpu(disp);
    if (test.active)
        for (int i = 0; i < test.nshots; i++)
            if (test.shots[i] == frame)
                fb_dump(frame, disp);
    display_show(disp);

    audio_flush();
    flush_save();

    if ((frame % 120) == 0) {
        uint64_t now = get_ticks();
        debugf("f%d: tlb refills %lu emulated %lu vram marks %lu\n", frame,
               (unsigned long)tlb_stats.rom_refills, (unsigned long)tlb_stats.emulated,
               (unsigned long)tlb_stats.vram_marks);
        debugf("f%d: %.2f ms/frame, rom misses %lu evictions %lu\n", frame,
               (double)TICKS_TO_US(now - t_last) / 120.0 / 1000.0,
               (unsigned long)rom_cache_stats.misses, (unsigned long)rom_cache_stats.evictions);
        t_last = now;
    }

    if (test.active && frame >= test.frames) {
        debugf("test done at frame %d\n", frame);
        emux_ioctl_exit();
        for (;;) { }
    }
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

int main(void) {
    debug_init_emulog();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_DISABLED);
    rdpq_init();
    joypad_init();
    audio_init(16004, AUDIO_INIT_LATENCY_MS(100));
    dfs_init(DFS_DEFAULT_LOCATION);
    sram_init();
    debugf("DBZ LoG2 N64: memory %d bytes, audio %d Hz\n", get_memory_size(), audio_get_frequency());
    load_test_script();
    load_save();
    rom_cache_init(ROM_CACHE_BYTES);
#ifdef GBA_TLB
    tlb_init();
#endif
    t_last = get_ticks();
    uint8_t *stack = memalign(16, GAME_STACK);
    run_game_on_stack(stack + GAME_STACK);
}

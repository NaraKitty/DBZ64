/*
 * Host (PC/Linux) test harness: runs the recompiled game headless, renders
 * frames with the reference software PPU and dumps PNG screenshots, a WAV of
 * the audio output and the EEPROM save.  Used to validate the runtime and
 * the recompiler before running on N64.
 *
 *   dbz --rom baserom.gba [--frames N] [--shot F,F,..] [--every K]
 *       [--out DIR] [--keys "F-F:KEY+KEY;F:KEY"] [--save file] [--wav file]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "gba.h"
#include "io.h"
#include "rom.h"
#include "ppu_soft.h"

void gba_boot(void);
extern uint32_t gba_miss_log[256];
extern unsigned gba_miss_count;

/* ------------------------------------------------------------ ROM */

static uint32_t *rom_words;
static uint32_t rom_size;
/* --rom-profile: keep ROM on the slow path and count reads per 4KB page */
static int rom_profile;
static uint32_t rom_page_reads[0x800];
static int rom_page_first_frame[0x800];
static int frame;

void gba_rom_map_init(void) {
    for (uint32_t off = 0; off < 0x01000000; off += 0x1000)
        gba_mem_map_rom_page(0x08000 + (off >> 12),
                             (off < rom_size && !rom_profile) ? (const uint8_t *)rom_words + off : NULL);
}

uint32_t gba_rom_read32(uint32_t a) {
    uint32_t off = a & 0x01FFFFFC;
    if (off < rom_size) {
        if (rom_profile) {
            uint32_t pg = off >> 12;
            if (!rom_page_reads[pg]++)
                rom_page_first_frame[pg] = frame;
        }
        return rom_words[off >> 2];
    }
    /* past the end of the ROM the bus returns the halfword address */
    uint32_t h = (a >> 1) & 0xFFFF;
    return h | ((h + 1) << 16);
}

const void *gba_rom_ptr(uint32_t addr, uint32_t len) {
    uint32_t off = addr & 0x01FFFFFF;
    if (off + len > rom_size)
        return NULL;
    return (const uint8_t *)rom_words + off;
}

static void load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    rom_size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    rom_words = malloc(rom_size + 4);
    uint8_t *b = (uint8_t *)rom_words;
    if (fread(b, 1, rom_size, f) != rom_size) { perror("read"); exit(1); }
    fclose(f);
#if GBA_BIG_ENDIAN
    for (uint32_t i = 0; i < rom_size / 4; i++)
        rom_words[i] = __builtin_bswap32(rom_words[i]);
#endif
}

/* ------------------------------------------------------------ options */

static int max_frames = 600;
static int every = 0;
static int shots[256], nshots;
static const char *outdir = ".";
static const char *save_path;
static const char *wav_path;

typedef struct { int from, to; uint16_t keys; } keyspan_t;
static keyspan_t spans[256];
static int nspans;
static uint16_t cur_keys = 0x3FF;

static uint16_t parse_keys(const char *s) {
    static const char *names[10] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l" };
    uint16_t pressed = 0;
    char buf[64];
    while (*s) {
        int n = 0;
        while (*s && *s != '+' && n < 63) buf[n++] = *s++;
        buf[n] = 0;
        if (*s == '+') s++;
        for (int i = 0; i < 10; i++)
            if (!strcmp(buf, names[i])) pressed |= (uint16_t)(1 << i);
    }
    return pressed;
}

static void parse_spans(const char *s) {
    char *dup = strdup(s), *save = NULL;
    for (char *tok = strtok_r(dup, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        keyspan_t k;
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        if (sscanf(tok, "%d-%d", &k.from, &k.to) != 2)
            k.to = k.from = atoi(tok);
        k.keys = parse_keys(colon + 1);
        if (nspans < 256) spans[nspans++] = k;
    }
    free(dup);
}

/* ------------------------------------------------------------ PNG */

static uint32_t crc_table[256];

static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    put32(f, len);
    uint32_t c = crc32_upd(0, (const uint8_t *)type, 4);
    fwrite(type, 1, 4, f);
    if (len) { fwrite(data, 1, len, f); c = crc32_upd(c, data, len); }
    put32(f, c);
}

/* uncompressed (stored deflate blocks) RGB PNG */
static void write_png(const char *path, const uint16_t *fb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_len = (size_t)h * (1 + 3 * w);
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (1 + 3 * w);
        row[0] = 0;
        for (int x = 0; x < w; x++) {
            uint16_t c = fb[y * w + x];
            uint8_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            row[1 + 3 * x] = (uint8_t)((r << 3) | (r >> 2));
            row[2 + 3 * x] = (uint8_t)((g << 3) | (g >> 2));
            row[3 + 3 * x] = (uint8_t)((b << 3) | (b >> 2));
        }
    }
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(zlen), *p = z;
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        *p++ = off + n >= raw_len;
        *p++ = n & 0xFF; *p++ = n >> 8; *p++ = ~n & 0xFF; *p++ = (~n >> 8) & 0xFF;
        memcpy(p, raw + off, n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
        p += n;
    }
    uint32_t ad = (b << 16) | a;
    *p++ = ad >> 24; *p++ = ad >> 16; *p++ = ad >> 8; *p++ = ad;
    chunk(f, "IDAT", z, (uint32_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

/* ------------------------------------------------------------ audio */

static int16_t *wav_buf;
static size_t wav_len, wav_cap;

void plat_audio_block(const int8_t *left, const int8_t *right) {
    if (!wav_path)
        return;
    if (wav_len + 32 > wav_cap) {
        wav_cap = wav_cap ? wav_cap * 2 : 1 << 20;
        wav_buf = realloc(wav_buf, wav_cap * sizeof(int16_t));
    }
    for (int i = 0; i < 16; i++) {
        wav_buf[wav_len++] = (int16_t)(left[i] * 256);
        wav_buf[wav_len++] = (int16_t)(right[i] * 256);
    }
}

static void write_wav(void) {
    if (!wav_path || !wav_len)
        return;
    FILE *f = fopen(wav_path, "wb");
    if (!f) return;
    uint32_t rate = 15978, bytes = (uint32_t)(wav_len * 2);
    uint32_t hdr[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746D66, 16,
                         0x00020001, rate, rate * 4, 0x00100004, 0x61746164, bytes };
    fwrite(hdr, 4, 11, f);
    fwrite(wav_buf, 2, wav_len, f);
    fclose(f);
}

/* ------------------------------------------------------------ save */

void plat_save_block(uint32_t block) { (void)block; }

static void load_save(void) {
    memset(gba_eeprom_data, 0xFF, sizeof gba_eeprom_data);
    if (!save_path) return;
    FILE *f = fopen(save_path, "rb");
    if (!f) return;
    if (fread(gba_eeprom_data, 1, sizeof gba_eeprom_data, f) == 0) { }
    fclose(f);
}

static void write_save(void) {
    if (!save_path || !gba_eeprom_dirty) return;
    FILE *f = fopen(save_path, "wb");
    if (!f) return;
    fwrite(gba_eeprom_data, 1, sizeof gba_eeprom_data, f);
    fclose(f);
}

/* ------------------------------------------------------------ hooks */

static uint16_t fb[240 * 160];

/* --dump-state F,F,..: write the video state (IO, palette, VRAM, OAM and the
 * per-line register snapshots) as GBA little-endian bytes, for renderer
 * tests.  Layout: "GBAS" u32 version=1, io[0x400], pal[0x400],
 * vram[0x18000], oam[0x400], u32 line_regs_valid, line_regs[160] (packed
 * little-endian: pa,pb,pc,pd u16; x,y u32; hofs[4],vofs[4] u16). */
static int dumps[256], ndumps;

static void put_mem(FILE *f, const uint32_t *words, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++)
        fputc(((const uint8_t *)words)[i ^ GBA_BX], f);
}

static void put_le(FILE *f, uint32_t v, int n) {
    for (int i = 0; i < n; i++)
        fputc((v >> (8 * i)) & 0xFF, f);
}

static void dump_state(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite("GBAS", 1, 4, f);
    put_le(f, 1, 4);
    put_mem(f, gba_mem.io, 0x400);
    put_mem(f, gba_mem.pal, 0x400);
    put_mem(f, gba_mem.vram, 0x18000);
    put_mem(f, gba_mem.oam, 0x400);
    put_le(f, (uint32_t)gba_line_regs_valid, 4);
    for (int l = 0; l < 160; l++) {
        const gba_line_regs_t *s = &gba_line_regs[l];
        put_le(f, s->bg2pa, 2); put_le(f, s->bg2pb, 2); put_le(f, s->bg2pc, 2); put_le(f, s->bg2pd, 2);
        put_le(f, s->bg2x, 4); put_le(f, s->bg2y, 4);
        for (int i = 0; i < 4; i++) put_le(f, s->hofs[i], 2);
        for (int i = 0; i < 4; i++) put_le(f, s->vofs[i], 2);
    }
    fclose(f);
}
static volatile int frames_since_alarm;
static time_t t_start;

static void finish(void) {
    write_wav();
    write_save();
    double secs = difftime(time(NULL), t_start);
    fprintf(stderr, "[host] %d frames in %.0fs; %u dispatch misses\n", frame, secs, gba_miss_count);
    for (unsigned i = 0; i < gba_miss_count; i++)
        fprintf(stderr, "  miss %08X %s\n", (unsigned)(gba_miss_log[i] & ~1u), (gba_miss_log[i] & 1) ? "T" : "A");
    if (rom_profile) {
        char path[512];
        snprintf(path, sizeof path, "%s/rom_profile.txt", outdir);
        FILE *f = fopen(path, "w");
        if (f) {
            for (int pg = 0; pg < 0x800; pg++)
                if (rom_page_reads[pg])
                    fprintf(f, "%08X %u %d\n", 0x08000000u + pg * 0x1000u,
                            (unsigned)rom_page_reads[pg], rom_page_first_frame[pg]);
            fclose(f);
        }
    }
}

void plat_frame(void) {
    frame++;
    frames_since_alarm++;
    uint16_t pressed = 0;
    for (int i = 0; i < nspans; i++)
        if (frame >= spans[i].from && frame <= spans[i].to)
            pressed |= spans[i].keys;
    cur_keys = (uint16_t)(0x3FF & ~pressed);
    int want = every && frame % every == 0;
    for (int i = 0; i < nshots; i++)
        if (shots[i] == frame) want = 1;
    for (int i = 0; i < ndumps; i++)
        if (dumps[i] == frame) {
            char path[512];
            snprintf(path, sizeof path, "%s/state_%05d.bin", outdir, frame);
            dump_state(path);
            want = 1;
        }
    if (want) {
        char path[512];
        ppu_soft_render(fb, 240);
        snprintf(path, sizeof path, "%s/frame_%05d.png", outdir, frame);
        write_png(path, fb, 240, 160);
    }
    if (frame >= max_frames) {
        finish();
        exit(0);
    }
}

uint16_t plat_keyinput(void) { return cur_keys; }

void plat_sleep(void) { plat_log("guest entered Stop (sleep)"); }

void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[gba f%d] ", frame);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void on_alarm(int sig) {
    (void)sig;
    if (frames_since_alarm == 0) {
        fprintf(stderr, "[host] watchdog: no frame for 10s (frame %d)\n", frame);
        gba_panic("watchdog");
    }
    frames_since_alarm = 0;
    alarm(10);
}

int main(int argc, char **argv) {
    const char *rom = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--rom")) { rom = v; i++; }
        else if (!strcmp(a, "--frames")) { max_frames = atoi(v); i++; }
        else if (!strcmp(a, "--every")) { every = atoi(v); i++; }
        else if (!strcmp(a, "--out")) { outdir = v; i++; }
        else if (!strcmp(a, "--keys")) { parse_spans(v); i++; }
        else if (!strcmp(a, "--keys-file")) {
            FILE *kf = fopen(v, "r");
            if (kf) {
                static char kb[1 << 16];
                size_t n = fread(kb, 1, sizeof kb - 1, kf);
                kb[n] = 0;
                fclose(kf);
                for (char *p = kb; *p; p++)
                    if (*p == '\n' || *p == '\r') *p = ';';
                parse_spans(kb);
            }
            i++;
        }
        else if (!strcmp(a, "--save")) { save_path = v; i++; }
        else if (!strcmp(a, "--wav")) { wav_path = v; i++; }
        else if (!strcmp(a, "--rom-profile")) { rom_profile = 1; }
        else if (!strcmp(a, "--dump-state")) {
            char *dup = strdup(v), *sv = NULL;
            for (char *t = strtok_r(dup, ",", &sv); t && ndumps < 256; t = strtok_r(NULL, ",", &sv))
                dumps[ndumps++] = atoi(t);
            free(dup);
            i++;
        }
        else if (!strcmp(a, "--shot")) {
            char *dup = strdup(v), *sv = NULL;
            for (char *t = strtok_r(dup, ",", &sv); t && nshots < 256; t = strtok_r(NULL, ",", &sv))
                shots[nshots++] = atoi(t);
            free(dup);
            i++;
        } else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (!rom) { fprintf(stderr, "usage: %s --rom file.gba [options]\n", argv[0]); return 2; }
    load_rom(rom);
    load_save();
    t_start = time(NULL);
    signal(SIGALRM, on_alarm);
    alarm(10);
    gba_boot();
    return 0;
}

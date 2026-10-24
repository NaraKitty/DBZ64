/*
 * io.h - GBA I/O registers, DMA, timers, IRQs, EEPROM and the frame
 * scheduler.  Guest time only advances inside Halt (SWI 2): the scheduler
 * walks through the events of the frame (audio FIFO blocks, HBlank lines,
 * VBlank) and delivers IRQs exactly where the game waits for them.
 */
#ifndef IO_H
#define IO_H

#include "gba.h"

/* register offsets */
enum {
    REG_DISPCNT = 0x000, REG_DISPSTAT = 0x004, REG_VCOUNT = 0x006,
    REG_BG0CNT = 0x008, REG_BG0HOFS = 0x010,
    REG_BG2PA = 0x020, REG_BG2X = 0x028, REG_BG3PA = 0x030, REG_BG3X = 0x038,
    REG_WIN0H = 0x040, REG_WININ = 0x048, REG_WINOUT = 0x04A, REG_MOSAIC = 0x04C,
    REG_BLDCNT = 0x050, REG_BLDALPHA = 0x052, REG_BLDY = 0x054,
    REG_SOUNDCNT_L = 0x080, REG_SOUNDCNT_H = 0x082, REG_SOUNDCNT_X = 0x084,
    REG_FIFO_A = 0x0A0, REG_FIFO_B = 0x0A4,
    REG_DMA0SAD = 0x0B0,
    REG_TM0CNT = 0x100,
    REG_KEYINPUT = 0x130, REG_KEYCNT = 0x132,
    REG_IE = 0x200, REG_IF = 0x202, REG_WAITCNT = 0x204, REG_IME = 0x208,
    REG_HALTCNT = 0x301,
};

enum {
    IRQ_VBLANK = 1 << 0, IRQ_HBLANK = 1 << 1, IRQ_VCOUNT = 1 << 2,
    IRQ_TIMER0 = 1 << 3, IRQ_DMA0 = 1 << 8, IRQ_KEYPAD = 1 << 12, IRQ_GAMEPAK = 1 << 13,
};

#define GBA_CYCLES_PER_LINE  1232u
#define GBA_LINES            228u
#define GBA_CYCLES_PER_FRAME (GBA_CYCLES_PER_LINE * GBA_LINES)

void gba_io_reset(void);
uint32_t gba_io_read32(uint32_t off);
uint32_t gba_io_read16(uint32_t off);
uint32_t gba_io_read8(uint32_t off);
void gba_io_write32(uint32_t off, uint32_t v);
void gba_io_write16(uint32_t off, uint32_t v);
void gba_io_write8(uint32_t off, uint32_t v);

/* register state of a finished immediate incrementing 16-bit DMA of n
 * units, for natives that copy the data themselves */
void gba_dma_settle16(int ch, uint32_t sad, uint32_t dad, uint32_t n);
/* the same for a 32-bit transfer of n words (n >= 1) */
void gba_dma_settle32(int ch, uint32_t sad, uint32_t dad, uint32_t n);

/* EEPROM (bit-serial over 0x0D000000) */
int gba_eeprom_active(void);
uint32_t gba_eeprom_read(void);
void gba_eeprom_write(uint32_t v);
extern uint8_t gba_eeprom_data[0x2000];
extern int gba_eeprom_dirty;

/* BIOS services and scheduling */
void gba_halt(void);
void gba_stop(void);
void gba_soft_reset(void) __attribute__((noreturn));
void gba_irq_service(void);

/* per-scanline register snapshots recorded while HBlank IRQs run */
typedef struct {
    uint16_t bg2pa, bg2pb, bg2pc, bg2pd;
    uint32_t bg2x, bg2y;
    uint16_t hofs[4], vofs[4];
} gba_line_regs_t;
extern gba_line_regs_t gba_line_regs[160];
extern int gba_line_regs_valid;      /* set when HBlank IRQs ran this frame */

/* time */
extern uint32_t gba_frame_cycle;     /* 0 .. GBA_CYCLES_PER_FRAME-1 */
extern uint32_t gba_frame_count;

/* ---- platform hooks (implemented by platform/<host|n64>) ---- */
/* Called at the start of VBlank: draw lines 0..159 from the current state
 * and present; also polls input. */
void plat_frame(void);
/* Current KEYINPUT value (active low, 10 bits). */
uint16_t plat_keyinput(void);
/* 16 stereo samples produced by the FIFO DMA (signed 8-bit). */
void plat_audio_block(const int8_t *left, const int8_t *right);
/* EEPROM block (8 bytes) changed. */
void plat_save_block(uint32_t block);
/* Stop/sleep: return when the wake key combination is held. */
void plat_sleep(void);
/* unrecoverable error: show msg and stop (default: abort) */
void plat_fatal(const char *msg) __attribute__((noreturn));
/* diagnostics */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif

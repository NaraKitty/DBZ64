/*
 * ppu_soft.h - reference software GBA renderer (scanline based).
 *
 * Reads VRAM / palette / OAM straight from gba_mem and the display
 * registers through io16()/io32().  When gba_line_regs_valid is set, BG2
 * affine parameters and BG0-3 scroll come from the per-line snapshots the
 * HBlank IRQs left in gba_line_regs[] instead of the live registers.
 *
 * Output pixels are GBA BGR555 (red in bits 0..4) with bit 15 clear.
 */
#ifndef PPU_SOFT_H
#define PPU_SOFT_H

#include <stdint.h>

#define PPU_SOFT_WIDTH  240
#define PPU_SOFT_HEIGHT 160

/* Render the whole 240x160 frame; pitch_pixels is the fb row stride. */
void ppu_soft_render(uint16_t *fb, int pitch_pixels);

/* Render one scanline (0..159) into out[0..239].  Stateless: the affine
 * reference point of a line is derived from BGxX/BGxY + line * PB/PD (or
 * the snapshot), so lines can be rendered in any order. */
void ppu_soft_render_line(int line, uint16_t *out);

#endif

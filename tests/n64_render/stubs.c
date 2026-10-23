/*
 * Minimal definitions of the runtime symbols the renderer uses, so the
 * test ROM links without the runtime sources.
 */
#include "gba.h"
#include "io.h"

gba_mem_t gba_mem __attribute__((aligned(64)));
gba_line_regs_t gba_line_regs[160];
int gba_line_regs_valid;
volatile uint32_t gba_vram_dirty;

/* the harness decides what the next gba_vram_take_dirty() reports */
uint32_t stub_dirty_mask;

uint32_t gba_vram_take_dirty(void) {
    uint32_t d = stub_dirty_mask;
    gba_vram_dirty = 0;
    return d;
}

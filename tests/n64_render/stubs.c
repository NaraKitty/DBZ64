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

/* and what the next gba_vram_take_units() reports */
uint32_t stub_units[96];
int stub_units_any;

int gba_vram_take_units(uint32_t words[96]) {
    if (!stub_units_any)
        return 0;
    for (int i = 0; i < 96; i++) {
        words[i] = stub_units[i];
        stub_units[i] = 0;
    }
    stub_units_any = 0;
    return 1;
}

uint32_t gba_vram_take_dirty(void) {
    uint32_t d = stub_dirty_mask;
    gba_vram_dirty = 0;
    return d;
}

/*
 * interp.h - ARM7TDMI interpreter used as a safety net for guest code that
 * has no recompiled version (and as a reference when debugging codegen).
 *
 * The interpreter works directly on the global `cpu` state and the memory
 * accessors in gba.h.  Whenever it reaches a BL / BX / jump whose target has
 * recompiled code it hands control to that code (gba_call semantics for
 * calls, returning for jumps).
 */
#ifndef INTERP_H
#define INTERP_H

#include "gba.h"

/* Interpret starting at target (bit0 = Thumb) as a call: returns when the
 * code transfers control to the link value that was in cpu.r[14] at entry.
 * Sets cpu.next_pc to that value. */
void gba_interp_call(uint32_t target);

/* Interpret starting at target until control reaches an address that has
 * recompiled code (or the current cpu.r[14] return sentinel); sets
 * cpu.next_pc to that address and returns. */
void gba_interp_jump(uint32_t target);

/* Interpret exactly one instruction at cpu pc (debug / differential use). */
uint32_t gba_interp_step(uint32_t pc_with_thumb_bit);

#endif

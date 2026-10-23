#ifndef DISPATCH_H
#define DISPATCH_H
#include "gba.h"

typedef struct {
    uint32_t addr;          /* guest address | 1 for Thumb */
    gba_func_t fn;
} gba_func_entry_t;

extern const gba_func_entry_t gba_func_table[];
extern const unsigned gba_func_count;
/* gba_func_mod[i]: what gba_func_table[i] may change, from the recompiler's
 * ABI summary (r0..r14 = bits 0..14, N/Z/C/V = bits 16..19) */
extern const uint32_t gba_func_mod[];

/* Recompiled code keeps the registers a function saves and restores in C
 * locals, and its callers rely on the ABI summary instead of cpu.r[]: after
 * it returns, cpu.r[] can hold stale values for registers the function
 * preserves.  Code that continues from cpu.r[] after running recompiled code
 * (the interpreter, the top-level loop) saves the registers before and puts
 * back the preserved ones after a normal return. */
typedef struct {
    uint32_t r[15];
    uint32_t n, z, c, v;
} gba_saved_regs_t;

void gba_save_regs(gba_saved_regs_t *s);
void gba_restore_preserved(const gba_saved_regs_t *s, uint32_t target);

#endif

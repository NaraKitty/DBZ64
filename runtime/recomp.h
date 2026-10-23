/*
 * recomp.h - helpers used by generated code (included by every generated file).
 */
#ifndef RECOMP_H
#define RECOMP_H

#include "gba.h"

/* ---- shifter with carry-out; amount 0..255 from a register ---- */

static inline uint32_t sh_lsl(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? 0 : v << s; }
static inline uint32_t sh_lsr(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? 0 : v >> s; }
static inline uint32_t sh_asr(uint32_t v, uint32_t s) { s &= 0xFF; return (uint32_t)((int32_t)v >> (s >= 32 ? 31 : s)); }
static inline uint32_t sh_ror(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

static inline uint32_t sh_lsl_c(uint32_t v, uint32_t s, uint32_t *c) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *c = (v >> (32 - s)) & 1; return v << s; }
    *c = s == 32 ? (v & 1) : 0;
    return 0;
}
static inline uint32_t sh_lsr_c(uint32_t v, uint32_t s, uint32_t *c) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *c = (v >> (s - 1)) & 1; return v >> s; }
    *c = s == 32 ? (v >> 31) : 0;
    return 0;
}
static inline uint32_t sh_asr_c(uint32_t v, uint32_t s, uint32_t *c) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *c = (v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    *c = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t sh_ror_c(uint32_t v, uint32_t s, uint32_t *c) {
    s &= 0xFF;
    if (s == 0) return v;
    s &= 31;
    if (s == 0) { *c = v >> 31; return v; }
    *c = (v >> (s - 1)) & 1;
    return (v >> s) | (v << (32 - s));
}

/* ---- flag helpers ---- */
#define ADD_V(a, b, r) ((~((a) ^ (b)) & ((a) ^ (r))) >> 31)
#define SUB_V(a, b, r) ((((a) ^ (b)) & ((a) ^ (r))) >> 31)

/* ---- stack fast path: the game's stacks live in IWRAM ---- */
#ifdef RECOMP_CHECK_STACK
void gba_bad_stack(uint32_t a);
#define STK_CHECK(a) do { if (unlikely(((a) >> 24) != 3)) gba_bad_stack(a); } while (0)
#else
#define STK_CHECK(a) ((void)0)
#endif
#define STK32(a) gba_mem.iwram[((a) & 0x7FFCu) >> 2]

static inline uint32_t sp_rd32(uint32_t a) { STK_CHECK(a); return STK32(a); }
static inline void sp_wr32(uint32_t a, uint32_t v) { STK_CHECK(a); STK32(a) = v; }

/* direct IWRAM / EWRAM access when the address is a compile-time constant */
#define IW32(a) gba_mem.iwram[((a) & 0x7FFCu) >> 2]
#define IW16(a) (*(uint16_t *)((uint8_t *)gba_mem.iwram + (((a) & 0x7FFEu) ^ GBA_HX)))
#define IW8(a)  (*(uint8_t *)((uint8_t *)gba_mem.iwram + (((a) & 0x7FFFu) ^ GBA_BX)))
#define EW32(a) gba_mem.ewram[((a) & 0x3FFFCu) >> 2]
#define EW16(a) (*(uint16_t *)((uint8_t *)gba_mem.ewram + (((a) & 0x3FFFEu) ^ GBA_HX)))
#define EW8(a)  (*(uint8_t *)((uint8_t *)gba_mem.ewram + (((a) & 0x3FFFFu) ^ GBA_BX)))

/* ---- I/O registers at compile-time-known addresses (offset into 0x04000000) ---- */
uint32_t gba_io_read32(uint32_t off);
uint32_t gba_io_read16(uint32_t off);
uint32_t gba_io_read8(uint32_t off);
void gba_io_write32(uint32_t off, uint32_t v);
void gba_io_write16(uint32_t off, uint32_t v);
void gba_io_write8(uint32_t off, uint32_t v);

/* ---- control transfer out of a function ---- */
/* run the function at target (lr unchanged); 0 if unknown (caller unwinds) */
int gba_jump(uint32_t target);
void gba_exc_return(void);                /* movs pc / ldm ^ with pc */
void gba_undefined(uint32_t pc, uint32_t raw);
/* entered a function the recompiler assumed unreachable (see tools/game.toml) */
void gba_unexpected(uint32_t addr);
void gba_msr(uint32_t value, uint32_t mask, int spsr);
uint32_t gba_mrs(int spsr);
void gba_stm_user(uint32_t addr, uint32_t list);
void gba_ldm_user(uint32_t addr, uint32_t list);

/* per-function call depth guard (debug builds) */
#ifdef RECOMP_TRACE
void gba_trace_enter(uint32_t addr);
#define TRACE_ENTER(a) gba_trace_enter(a)
#else
#define TRACE_ENTER(a) ((void)0)
#endif

#endif

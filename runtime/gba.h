/*
 * gba.h - core contract between recompiled code and the GBA runtime.
 *
 * Guest memory is stored as native 32-bit words: an aligned 32-bit guest
 * access is a plain host load/store on both little-endian hosts (PC test
 * build) and the big-endian N64.  Byte and halfword accesses XOR the low
 * address bits with GBA_BX / GBA_HX to land on the right lane.
 *
 * All guest memory accesses go through a 4KB page table indexed by
 * (addr >> 12) for the 256MB GBA address space.  Each entry is a host
 * pointer to the start of the page; bit 0 set means "slow path" (I/O,
 * unmapped, ROM page not resident on N64, EEPROM, ...).
 */
#ifndef GBA_H
#define GBA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(N64) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define GBA_BIG_ENDIAN 1
#define GBA_BX 3u
#define GBA_HX 2u
#else
#define GBA_BIG_ENDIAN 0
#define GBA_BX 0u
#define GBA_HX 0u
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ------------------------------------------------------------ memory */

#define GBA_EWRAM_SIZE 0x40000u
#define GBA_IWRAM_SIZE 0x8000u
#define GBA_IO_SIZE    0x400u
#define GBA_PAL_SIZE   0x400u
#define GBA_VRAM_SIZE  0x18000u
#define GBA_OAM_SIZE   0x400u
#define GBA_ROM_SIZE   0x800000u

/* GBA_TLB (N64 build): guest RAM is also mapped through the VR4300 TLB at
 * the guest's own addresses (platform/n64/tlb.c), so generated code accesses
 * it with plain loads/stores.  Each region must then be aligned to its TLB
 * page size, and stay congruent mod 8KB so the virtually indexed D-cache sees
 * the TLB view and the runtime's KSEG0 view as the same lines. */
#ifdef GBA_TLB
#define GBA_TLB_ALIGN(n) __attribute__((aligned(n)))
#define GBA_PAL_ALLOC 0x2000u
#else
#define GBA_TLB_ALIGN(n)
#define GBA_PAL_ALLOC GBA_PAL_SIZE
#endif

typedef struct gba_mem {
    uint32_t ewram[GBA_EWRAM_SIZE / 4] GBA_TLB_ALIGN(0x40000);
    uint32_t iwram[GBA_IWRAM_SIZE / 4] GBA_TLB_ALIGN(0x4000);
    uint32_t vram[GBA_VRAM_SIZE / 4] GBA_TLB_ALIGN(0x4000);
    uint32_t pal[GBA_PAL_ALLOC / 4] GBA_TLB_ALIGN(0x2000);
    uint32_t oam[GBA_PAL_ALLOC / 4] GBA_TLB_ALIGN(0x2000);
    uint32_t io[GBA_IO_SIZE / 4];      /* raw register backing store */
    uint32_t bios[0x4000 / 4];
} gba_mem_t;

/* alignment comes from the type (64 bytes, or the TLB page sizes) */
extern gba_mem_t gba_mem;

/* page tables, 64K entries each (addr bits 27..12) */
extern uintptr_t gba_rd_page[0x10000];
extern uintptr_t gba_wr_page[0x10000];

#define GBA_PAGE_SLOW 1u

uint32_t gba_rd32_slow(uint32_t a);
uint32_t gba_rd16_slow(uint32_t a);
uint32_t gba_rd8_slow(uint32_t a);
void gba_wr32_slow(uint32_t a, uint32_t v);
void gba_wr16_slow(uint32_t a, uint32_t v);
void gba_wr8_slow(uint32_t a, uint32_t v);

/* VRAM write tracking for renderer caches: bit n = 4KB VRAM page n (0..23)
 * was written since the last gba_vram_take_dirty().  Pages are write-
 * protected until their first write in a frame. */
extern volatile uint32_t gba_vram_dirty;
uint32_t gba_vram_take_dirty(void);
/* mark [a, a+len) dirty and writable (bulk writers like DMA call this first) */
void gba_mem_touch(uint32_t a, uint32_t len);

/* host pointer for a guest address in a fast page, or NULL */
static inline void *gba_host_ptr(uint32_t a) {
    uintptr_t p = gba_rd_page[(a >> 12) & 0xFFFFu];
    return (p & GBA_PAGE_SLOW) ? NULL : (void *)(p + (a & 0xFFFu));
}

static inline void *gba_host_wptr(uint32_t a) {
    uintptr_t p = gba_wr_page[(a >> 12) & 0xFFFFu];
    return (p & GBA_PAGE_SLOW) ? NULL : (void *)(p + (a & 0xFFFu));
}

#ifdef GBA_TLB
/* Guest memory is mapped at the guest's own addresses: one load/store plus
 * the 28-bit address mask (the GBA ignores the top bits; it also keeps junk
 * pointers out of KSEG0).  I/O, unmapped addresses, non-resident ROM,
 * write-protected VRAM and unaligned accesses fault into
 * platform/n64/tlb.c, which emulates the access with ARM semantics. */
#define GBA_GA(a) ((uintptr_t)((a) & 0x0FFFFFFFu))
static inline uint32_t rd32(uint32_t a) { return *(volatile const uint32_t *)GBA_GA(a); }
static inline uint32_t rd16(uint32_t a) { return *(volatile const uint16_t *)(GBA_GA(a) ^ GBA_HX); }
static inline uint32_t rd8(uint32_t a) { return *(volatile const uint8_t *)(GBA_GA(a) ^ GBA_BX); }
/* an odd LDRSH faults; the handler returns the signed byte */
static inline int32_t rd16s(uint32_t a) { return *(volatile const int16_t *)(GBA_GA(a) ^ GBA_HX); }
static inline void wr32(uint32_t a, uint32_t v) { *(volatile uint32_t *)GBA_GA(a) = v; }
static inline void wr16(uint32_t a, uint32_t v) { *(volatile uint16_t *)(GBA_GA(a) ^ GBA_HX) = (uint16_t)v; }
static inline void wr8(uint32_t a, uint32_t v) { *(volatile uint8_t *)(GBA_GA(a) ^ GBA_BX) = (uint8_t)v; }
#else
/* ARM7 semantics: unaligned LDR rotates, unaligned LDRH rotates by 8,
 * unaligned stores ignore the low bits.  The slow path handles all of it. */
static inline uint32_t rd32(uint32_t a) {
    uintptr_t p = gba_rd_page[(a >> 12) & 0xFFFFu];
    if (unlikely((p & GBA_PAGE_SLOW) | (a & 3u)))
        return gba_rd32_slow(a);
    return *(const uint32_t *)(p + (a & 0xFFFu));
}

static inline uint32_t rd16(uint32_t a) {
    uintptr_t p = gba_rd_page[(a >> 12) & 0xFFFFu];
    if (unlikely((p & GBA_PAGE_SLOW) | (a & 1u)))
        return gba_rd16_slow(a);
    return *(const uint16_t *)(p + ((a & 0xFFFu) ^ GBA_HX));
}

static inline uint32_t rd8(uint32_t a) {
    uintptr_t p = gba_rd_page[(a >> 12) & 0xFFFFu];
    if (unlikely(p & GBA_PAGE_SLOW))
        return gba_rd8_slow(a);
    return *(const uint8_t *)(p + ((a & 0xFFFu) ^ GBA_BX));
}

static inline int32_t rd16s(uint32_t a) {
    if (unlikely(a & 1u))   /* LDRSH from an odd address loads a signed byte */
        return (int8_t)rd8(a);
    return (int16_t)rd16(a);
}

static inline void wr32(uint32_t a, uint32_t v) {
    uintptr_t p = gba_wr_page[(a >> 12) & 0xFFFFu];
    if (unlikely(p & GBA_PAGE_SLOW)) {
        gba_wr32_slow(a, v);
        return;
    }
    *(uint32_t *)(p + (a & 0xFFCu)) = v;
}

static inline void wr16(uint32_t a, uint32_t v) {
    uintptr_t p = gba_wr_page[(a >> 12) & 0xFFFFu];
    if (unlikely(p & GBA_PAGE_SLOW)) {
        gba_wr16_slow(a, v);
        return;
    }
    *(uint16_t *)(p + ((a & 0xFFEu) ^ GBA_HX)) = (uint16_t)v;
}

static inline void wr8(uint32_t a, uint32_t v) {
    uintptr_t p = gba_wr_page[(a >> 12) & 0xFFFFu];
    if (unlikely(p & GBA_PAGE_SLOW)) {
        gba_wr8_slow(a, v);
        return;
    }
    *(uint8_t *)(p + ((a & 0xFFFu) ^ GBA_BX)) = (uint8_t)v;
}

#endif

/* direct I/O register view (byte offset into the 1KB register file) */
static inline uint16_t io16(uint32_t off) {
    return *(const uint16_t *)((const uint8_t *)gba_mem.io + (off ^ GBA_HX));
}
static inline void io16_set(uint32_t off, uint16_t v) {
    *(uint16_t *)((uint8_t *)gba_mem.io + (off ^ GBA_HX)) = v;
}
static inline uint32_t io32(uint32_t off) {
    return gba_mem.io[off >> 2];
}

/* ------------------------------------------------------------ CPU */

enum {
    CPSR_MODE_USR = 0x10, CPSR_MODE_FIQ = 0x11, CPSR_MODE_IRQ = 0x12,
    CPSR_MODE_SVC = 0x13, CPSR_MODE_ABT = 0x17, CPSR_MODE_UND = 0x1B,
    CPSR_MODE_SYS = 0x1F,
};
#define CPSR_T 0x20u
#define CPSR_F 0x40u
#define CPSR_I 0x80u

typedef struct gba_cpu {
    uint32_t r[16];
    /* flags kept unpacked as 0/1 */
    uint32_t n, z, c, v;
    uint32_t cpsr_ctl;      /* CPSR bits 0..7 (mode, T, F, I) */
    uint32_t spsr;          /* SPSR of the current mode */
    /* banked registers: r13/r14 per mode, SPSRs */
    uint32_t bank_usr[2], bank_irq[2], bank_svc[2], bank_fiq[2];
    uint32_t bank_abt[2], bank_und[2];
    uint32_t spsr_irq, spsr_svc, spsr_fiq, spsr_abt, spsr_und;
    uint32_t fiq_r8_12[5], usr_r8_12[5];
    /* the guest address an indirect branch / return wants to go to */
    uint32_t next_pc;
} gba_cpu_t;

extern gba_cpu_t cpu;

uint32_t cpu_get_cpsr(void);
void cpu_set_cpsr(uint32_t v, uint32_t field_mask);
uint32_t cpu_get_spsr(void);
void cpu_set_spsr(uint32_t v, uint32_t field_mask);
/* restore CPSR from SPSR (exception return: movs pc / ldm ^) */
void cpu_return_from_exception(void);

/* ------------------------------------------------------------ dispatch */

typedef void (*gba_func_t)(void);

/* Run the code at guest address 'target' (bit 0 = Thumb) as a call:
 * returns when the callee returns to cpu.r[14] as it was at entry. */
void gba_call(uint32_t target);
/* Look up a recompiled function; NULL if not present. */
gba_func_t gba_lookup(uint32_t addr, int thumb);

/* software interrupt from guest code; 'thumb' gives the caller's state */
void gba_swi(uint32_t num, uint32_t ret_addr, int thumb);

/* called on loop back-edges flagged by the recompiler (idle detection) */
void gba_idle_hint(uint32_t pc);

/* fatal error with guest context */
void gba_panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif

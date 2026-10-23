/*
 * tlb.c - guest memory mapped through the VR4300 TLB (GBA_TLB builds).
 *
 * Generated code accesses guest RAM with plain loads/stores at the guest's
 * own addresses (masked to 28 bits, see gba.h).  Wired TLB entries map:
 *
 *   0x02000000 EWRAM   256KB page (odd half mirrors it)
 *   0x03000000 IWRAM   2 x 16KB, mirrored again at 0x03FF8000 (BIOS view)
 *   0x05000000 palette 4KB page,  0x07000000 OAM 4KB page
 *   0x06000000 VRAM    16KB pages, 0x06018000 mirrors 0x06010000
 *
 * ROM pages are mapped on demand from the ROM cache (rom_n64.c) into the
 * remaining entries.  Everything else faults here:
 *   - TLB miss on resident/loadable ROM: install a mapping and retry;
 *   - TLB modification on clean VRAM: mark the page dirty, make it writable;
 *   - I/O, unmapped regions, unaligned accesses: emulate the faulting MIPS
 *     load/store through the runtime's slow paths with ARM semantics, then
 *     skip it (emulating the branch when it sits in a delay slot).
 */
#include <libdragon.h>
#include "gba.h"
#include "tlb.h"

#define LO_C_CACHED   (3u << 3)
#define LO_D          (1u << 2)
#define LO_V          (1u << 1)
#define LO_G          (1u << 0)

#define PM_4K   0x00000000u
#define PM_16K  0x00006000u
#define PM_256K 0x0007E000u

enum { E_EWRAM, E_IWRAM, E_IWRAM_HI, E_PAL, E_OAM, E_VRAM0, E_VRAM1, E_VRAM2, E_VRAM3, N_WIRED };

tlb_stats_t tlb_stats;
static exception_handler_t prev_handler;

static inline uint32_t phys(const void *p) {
    return (uint32_t)(uintptr_t)p & 0x1FFFFFFFu;
}

static inline uint32_t lo(uint32_t pa, uint32_t flags) {
    return ((pa >> 12) << 6) | LO_C_CACHED | LO_G | flags;
}

static void tlb_write(int index, uint32_t va, uint32_t pagemask, uint32_t lo0, uint32_t lo1) {
    C0_WRITE_INDEX(index);
    C0_WRITE_PAGEMASK(pagemask);
    C0_WRITE_ENTRYHI(va & ~0x1FFFu);
    C0_WRITE_ENTRYLO0(lo0);
    C0_WRITE_ENTRYLO1(lo1);
    C0_TLBWI();
}

/* ------------------------------------------------------------ VRAM */

/* 16KB VRAM pages 0..5; bit set = writable (dirty since last take) */
static uint32_t vram_writable;

static uint32_t vram_lo(int page16) {
    return lo(phys(gba_mem.vram) + (uint32_t)page16 * 0x4000u,
              LO_V | ((vram_writable >> page16) & 1 ? LO_D : 0));
}

static void vram_rewrite(void) {
    tlb_write(E_VRAM0, 0x06000000, PM_16K, vram_lo(0), vram_lo(1));
    tlb_write(E_VRAM1, 0x06008000, PM_16K, vram_lo(2), vram_lo(3));
    tlb_write(E_VRAM2, 0x06010000, PM_16K, vram_lo(4), vram_lo(5));
    tlb_write(E_VRAM3, 0x06018000, PM_16K, vram_lo(4), vram_lo(5));
}

void tlb_vram_set_writable(uint32_t page4k, int writable) {
    uint32_t bitm = 1u << (page4k >> 2);
    uint32_t nw = writable ? (vram_writable | bitm) : (vram_writable & ~bitm);
    if (nw != vram_writable) {
        disable_interrupts();
        vram_writable = nw;
        vram_rewrite();
        enable_interrupts();
    }
}

/* ------------------------------------------------------------ ROM */

#define ROM_FIRST_ENTRY N_WIRED

void tlb_rom_unmap(uint32_t va16k) {
    /* drop any TLB entry covering this 16KB ROM page */
    disable_interrupts();
    uint32_t pair = va16k & ~0x7FFFu;
    C0_WRITE_PAGEMASK(PM_16K);
    C0_WRITE_ENTRYHI(pair);
    C0_TLBP();
    uint32_t idx = C0_INDEX();
    if (!(idx & 0x80000000u) && idx >= ROM_FIRST_ENTRY) {
        /* park it on an address nothing uses */
        tlb_write((int)idx, 0x0F000000u + idx * 0x8000u, PM_16K, 0, 0);
    }
    enable_interrupts();
}

static int rom_refill(uint32_t va) {
    if (va < 0x08000000u || va >= 0x08000000u + GBA_ROM_SIZE)
        return 0;
    uint32_t pair = va & ~0x7FFFu;
    const void *even = rom_page16_load(pair, va < pair + 0x4000u);
    const void *odd = rom_page16_load(pair + 0x4000u, va >= pair + 0x4000u);
    uint32_t lo0 = even ? lo(phys(even), LO_V) : 0;
    uint32_t lo1 = odd ? lo(phys(odd), LO_V) : 0;
    C0_WRITE_PAGEMASK(PM_16K);
    C0_WRITE_ENTRYHI(pair);
    C0_TLBP();
    uint32_t idx = C0_INDEX();
    C0_WRITE_PAGEMASK(PM_16K);
    C0_WRITE_ENTRYHI(pair);
    C0_WRITE_ENTRYLO0(lo0);
    C0_WRITE_ENTRYLO1(lo1);
    if (!(idx & 0x80000000u) && idx >= ROM_FIRST_ENTRY) {
        C0_WRITE_INDEX(idx);
        C0_TLBWI();
    } else {
        C0_TLBWR();
    }
    tlb_stats.rom_refills++;
    return 1;
}

/* ------------------------------------------------------------ emulation */

static inline int64_t sx32(uint32_t v) { return (int64_t)(int32_t)v; }

/* Next PC for the branch at epc whose delay slot faulted (register values
 * must be read before the delay-slot instruction's result is written). */
static uint32_t branch_next(reg_block_t *r, uint32_t epc) {
    uint32_t br = *(const uint32_t *)(uintptr_t)epc;
    uint32_t op = br >> 26, rs = (br >> 21) & 31, rt = (br >> 16) & 31;
    int64_t a = (int64_t)r->gpr[rs], b = (int64_t)r->gpr[rt];
    uint32_t taken = epc + 4 + ((uint32_t)(int32_t)(int16_t)br << 2);
    uint32_t not_taken = epc + 8;
    switch (op) {
    case 0x00:
        if ((br & 0x3F) == 0x08)
            return (uint32_t)a;                                  /* jr */
        if ((br & 0x3F) == 0x09) {                               /* jalr */
            uint32_t rd = (br >> 11) & 31;
            if (rd)
                r->gpr[rd] = (uint64_t)sx32(epc + 8);
            return (uint32_t)a;
        }
        break;
    case 0x01: {
        int link = rt & 0x10;
        int cond = (rt & 1) ? (a >= 0) : (a < 0);
        if (link)
            r->gpr[31] = (uint64_t)sx32(epc + 8);
        return cond ? taken : not_taken;
    }
    case 0x02:
        return ((epc + 4) & 0xF0000000u) | ((br & 0x03FFFFFFu) << 2);
    case 0x03:
        r->gpr[31] = (uint64_t)sx32(epc + 8);
        return ((epc + 4) & 0xF0000000u) | ((br & 0x03FFFFFFu) << 2);
    case 0x04: case 0x14: return a == b ? taken : not_taken;
    case 0x05: case 0x15: return a != b ? taken : not_taken;
    case 0x06: case 0x16: return a <= 0 ? taken : not_taken;
    case 0x07: case 0x17: return a > 0 ? taken : not_taken;
    case 0x11:
        if (rs == 0x08) {                                        /* bc1f/bc1t */
            int c = (r->fc31 >> 23) & 1;
            return (c == (int)(rt & 1)) ? taken : not_taken;
        }
        break;
    }
    tlb_stats.unknown_branch++;
    return not_taken;
}

static int emulate_access(reg_block_t *r, uint32_t va) {
    uint32_t epc = r->epc;
    int bd = (r->cr & C0_CAUSE_BD) != 0;
    uint32_t at = bd ? epc + 4 : epc;
    uint32_t insn = *(const uint32_t *)(uintptr_t)at;
    uint32_t op = insn >> 26, rt = (insn >> 16) & 31;
    uint32_t next = bd ? branch_next(r, epc) : epc + 4;
    uint64_t res = 0;
    int load = 1;
    switch (op) {
    case 0x20: res = (uint64_t)(int64_t)(int8_t)gba_rd8_slow(va ^ GBA_BX); break;     /* lb */
    case 0x24: res = gba_rd8_slow(va ^ GBA_BX); break;                                 /* lbu */
    case 0x21: {                                                                       /* lh */
        uint32_t g = va ^ GBA_HX;
        res = (g & 1) ? (uint64_t)(int64_t)(int8_t)gba_rd8_slow(g)
                      : (uint64_t)(int64_t)(int16_t)gba_rd16_slow(g);
        break;
    }
    case 0x25: res = gba_rd16_slow(va ^ GBA_HX); break;                                /* lhu */
    case 0x23: res = (uint64_t)sx32(gba_rd32_slow(va)); break;                         /* lw */
    case 0x27: res = gba_rd32_slow(va); break;                                         /* lwu */
    case 0x28: load = 0; gba_wr8_slow(va ^ GBA_BX, (uint32_t)r->gpr[rt]); break;       /* sb */
    case 0x29: load = 0; gba_wr16_slow(va ^ GBA_HX, (uint32_t)r->gpr[rt]); break;      /* sh */
    case 0x2B: load = 0; gba_wr32_slow(va, (uint32_t)r->gpr[rt]); break;               /* sw */
    default:
        return 0;
    }
    if (load && rt)
        r->gpr[rt] = res;
    r->epc = next;
    tlb_stats.emulated++;
    tlb_stats.last_va = va;
    tlb_stats.last_epc = at;
    return 1;
}

int tlb_trace = 64;   /* log the first faults (debugging aid) */

static void tlb_exception(exception_t *ex) {
    reg_block_t *r = ex->regs;
    uint32_t va = (uint32_t)C0_BADVADDR();
    /* rd32o/wr32o add the offset after masking: a junk base just below
     * 0x10000000 lands in the first 4KB above it; wrap like the GBA bus */
    if (va >= 0x10000000u && va < 0x10001000u && va != (uint32_t)r->epc)
        va &= 0x0FFFFFFFu;
    if (tlb_trace > 0) {
        tlb_trace--;
        uint32_t at = (uint32_t)r->epc + ((r->cr & C0_CAUSE_BD) ? 4 : 0);
        debugf("[tlb] code %d va %08lX epc %08lX insn %08lX\n", (int)ex->code,
               (unsigned long)va, (unsigned long)r->epc, (unsigned long)*(uint32_t *)(uintptr_t)at);
    }
    switch (ex->code) {
    case EXCEPTION_CODE_TLB_MODIFICATION:
        if (va >= 0x06000000u && va < 0x06020000u) {
            uint32_t off = va & 0x1FFFFu;
            if (off >= 0x18000u)
                off -= 0x8000u;
            gba_mem_touch(0x06000000u + off, 1);
            tlb_stats.vram_marks++;
            return;
        }
        break;
    case EXCEPTION_CODE_TLB_LOAD_I_MISS:
        if (va == (uint32_t)r->epc)
            break;                       /* instruction fetch: a real crash */
        if (rom_refill(va))
            return;
        /* fall through */
    case EXCEPTION_CODE_TLB_STORE_MISS:
    case EXCEPTION_CODE_LOAD_I_ADDRESS_ERROR:
    case EXCEPTION_CODE_STORE_ADDRESS_ERROR:
        if (va < 0x10000000u && va != (uint32_t)r->epc && emulate_access(r, va))
            return;
        break;
    default:
        break;
    }
    prev_handler(ex);
}

/* ------------------------------------------------------------ setup */

void tlb_init(void) {
    if ((phys(gba_mem.ewram) & 0x3FFFFu) || (phys(gba_mem.iwram) & 0x3FFFu) ||
        (phys(gba_mem.vram) & 0x3FFFu) || (phys(gba_mem.pal) & 0x1FFFu) || (phys(gba_mem.oam) & 0x1FFFu))
        gba_panic("guest memory is not aligned for the TLB (gba_mem at %08lX)",
                  (unsigned long)phys(&gba_mem));
    disable_interrupts();
    for (int i = 0; i < 32; i++)
        tlb_write(i, 0x0F000000u + (uint32_t)i * 0x8000u, PM_16K, 0, 0);
    uint32_t ew = phys(gba_mem.ewram), iw = phys(gba_mem.iwram);
    tlb_write(E_EWRAM, 0x02000000, PM_256K, lo(ew, LO_V | LO_D), lo(ew, LO_V | LO_D));
    tlb_write(E_IWRAM, 0x03000000, PM_16K, lo(iw, LO_V | LO_D), lo(iw + 0x4000, LO_V | LO_D));
    tlb_write(E_IWRAM_HI, 0x03FF8000, PM_16K, lo(iw, LO_V | LO_D), lo(iw + 0x4000, LO_V | LO_D));
    tlb_write(E_PAL, 0x05000000, PM_4K, lo(phys(gba_mem.pal), LO_V | LO_D), lo(phys(gba_mem.pal), LO_V | LO_D));
    tlb_write(E_OAM, 0x07000000, PM_4K, lo(phys(gba_mem.oam), LO_V | LO_D), lo(phys(gba_mem.oam), LO_V | LO_D));
    vram_writable = 0x3F;
    vram_rewrite();
    C0_WRITE_WIRED(N_WIRED);
    enable_interrupts();
    prev_handler = register_exception_handler(tlb_exception);
}

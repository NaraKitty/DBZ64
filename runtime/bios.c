/*
 * bios.c - native BIOS services and CPU mode handling.
 *
 * The games call SWI 0 (SoftReset), 2 (Halt), 3 (Stop), 6 (Div) and, in
 * Buu's Fury's link-cable and wireless libraries, 0x0B (CpuSet); newlib's dead
 * semihosting stubs (Thumb 0xAB, ARM 0x123456) are ignored.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "gba.h"
#include "io.h"
#include "recomp.h"

/* ------------------------------------------------------------ CPSR */

static uint32_t *bank_for(uint32_t mode) {
    switch (mode & 0x1F) {
    case CPSR_MODE_IRQ: return cpu.bank_irq;
    case CPSR_MODE_SVC: return cpu.bank_svc;
    case CPSR_MODE_FIQ: return cpu.bank_fiq;
    case CPSR_MODE_ABT: return cpu.bank_abt;
    case CPSR_MODE_UND: return cpu.bank_und;
    default: return cpu.bank_usr;     /* USR and SYS share registers */
    }
}

static uint32_t *spsr_for(uint32_t mode) {
    switch (mode & 0x1F) {
    case CPSR_MODE_IRQ: return &cpu.spsr_irq;
    case CPSR_MODE_SVC: return &cpu.spsr_svc;
    case CPSR_MODE_FIQ: return &cpu.spsr_fiq;
    case CPSR_MODE_ABT: return &cpu.spsr_abt;
    case CPSR_MODE_UND: return &cpu.spsr_und;
    default: return NULL;
    }
}

uint32_t cpu_get_cpsr(void) {
    return (cpu.n << 31) | (cpu.z << 30) | (cpu.c << 29) | (cpu.v << 28) | cpu.cpsr_ctl;
}

static void switch_mode(uint32_t newmode) {
    uint32_t old = cpu.cpsr_ctl & 0x1F;
    newmode &= 0x1F;
    if (bank_for(old) != bank_for(newmode)) {
        uint32_t *ob = bank_for(old), *nb = bank_for(newmode);
        ob[0] = cpu.r[13];
        ob[1] = cpu.r[14];
        cpu.r[13] = nb[0];
        cpu.r[14] = nb[1];
    }
    cpu.cpsr_ctl = (cpu.cpsr_ctl & ~0x1Fu) | newmode;
}

void cpu_set_cpsr(uint32_t v, uint32_t mask) {
    if (mask & 8) {
        cpu.n = v >> 31;
        cpu.z = (v >> 30) & 1;
        cpu.c = (v >> 29) & 1;
        cpu.v = (v >> 28) & 1;
    }
    if (mask & 1) {
        if ((v & 0x1F) != (cpu.cpsr_ctl & 0x1F))
            switch_mode(v & 0x1F);
        cpu.cpsr_ctl = (cpu.cpsr_ctl & 0x1F) | (v & 0xE0);
    }
}

uint32_t cpu_get_spsr(void) {
    uint32_t *s = spsr_for(cpu.cpsr_ctl);
    return s ? *s : cpu_get_cpsr();
}

void cpu_set_spsr(uint32_t v, uint32_t mask) {
    uint32_t *s = spsr_for(cpu.cpsr_ctl);
    if (!s)
        return;
    uint32_t m = 0;
    if (mask & 1) m |= 0x000000FF;
    if (mask & 2) m |= 0x0000FF00;
    if (mask & 4) m |= 0x00FF0000;
    if (mask & 8) m |= 0xFF000000;
    *s = (*s & ~m) | (v & m);
}

void cpu_return_from_exception(void) {
    cpu_set_cpsr(cpu_get_spsr(), 0xF);
}

/* generated-code entry points */
void gba_msr(uint32_t value, uint32_t mask, int spsr) {
    if (spsr)
        cpu_set_spsr(value, mask);
    else
        cpu_set_cpsr(value, mask);
}

uint32_t gba_mrs(int spsr) {
    return spsr ? cpu_get_spsr() : cpu_get_cpsr();
}

void gba_exc_return(void) {
    cpu_return_from_exception();
}

void gba_stm_user(uint32_t addr, uint32_t list) {
    /* store user-bank registers (r13/r14 from the USR bank) */
    for (int r = 0; r < 16; r++) {
        if (!(list & (1u << r)))
            continue;
        uint32_t v = cpu.r[r];
        if ((r == 13 || r == 14) && bank_for(cpu.cpsr_ctl) != cpu.bank_usr)
            v = cpu.bank_usr[r - 13];
        wr32(addr, v);
        addr += 4;
    }
}

void gba_ldm_user(uint32_t addr, uint32_t list) {
    for (int r = 0; r < 15; r++) {
        if (!(list & (1u << r)))
            continue;
        uint32_t v = rd32(addr);
        if ((r == 13 || r == 14) && bank_for(cpu.cpsr_ctl) != cpu.bank_usr)
            cpu.bank_usr[r - 13] = v;
        else
            cpu.r[r] = v;
        addr += 4;
    }
}

void gba_undefined(uint32_t pc, uint32_t raw) {
    gba_panic("undefined instruction %08X at %08X", (unsigned)raw, (unsigned)pc);
}

void gba_unexpected(uint32_t addr) {
    plat_log("WARNING: entered %08X, assumed unreachable by the recompiler; register "
             "summaries of its callers may be wrong", (unsigned)addr);
}

void gba_bad_stack(uint32_t a) {
    gba_panic("stack access outside IWRAM: %08X", (unsigned)a);
}

/* ------------------------------------------------------------ SWI */

static void swi_div(void) {
    int32_t num = (int32_t)cpu.r[0], den = (int32_t)cpu.r[1];
    if (den == 0) {
        /* BIOS returns +-1 / num for division by zero */
        cpu.r[0] = num < 0 ? (uint32_t)-1 : 1;
        cpu.r[1] = (uint32_t)num;
        cpu.r[3] = 1;
        return;
    }
    if (num == INT32_MIN && den == -1) {
        cpu.r[0] = (uint32_t)INT32_MIN;
        cpu.r[1] = 0;
        cpu.r[3] = (uint32_t)INT32_MIN;
        return;
    }
    int32_t q = num / den, r = num % den;
    cpu.r[0] = (uint32_t)q;
    cpu.r[1] = (uint32_t)r;
    cpu.r[3] = (uint32_t)(q < 0 ? -q : q);
}

/* CpuSet (0x0B) / CpuFastSet (0x0C): r0 source, r1 destination, r2 = count
 * (bits 0-20) | fill (bit 24) | 32-bit units (bit 26; always for FastSet,
 * whose count rounds up to 8 words).  Like the BIOS, a source below EWRAM is
 * refused and the addresses are forced to the unit alignment. */
static void swi_cpuset(int fast) {
    uint32_t src = cpu.r[0], dst = cpu.r[1], ctl = cpu.r[2];
    uint32_t n = ctl & 0x1FFFFFu;
    int fill = (ctl >> 24) & 1;
    if ((src >> 24) < 0x02)
        return;
    if (fast)
        n = (n + 7) & ~7u;
    if (fast || (ctl & (1u << 26))) {
        src &= ~3u;
        dst &= ~3u;
        uint32_t v = rd32(src);
        for (uint32_t i = 0; i < n; i++) {
            wr32(dst, fill ? v : rd32(src));
            dst += 4;
            if (!fill)
                src += 4;
        }
    } else {
        src &= ~1u;
        dst &= ~1u;
        uint32_t v = rd16(src);
        for (uint32_t i = 0; i < n; i++) {
            wr16(dst, fill ? v : rd16(src));
            dst += 2;
            if (!fill)
                src += 2;
        }
    }
    cpu.r[0] = src;
    cpu.r[1] = dst;
}

void gba_swi(uint32_t num, uint32_t ret_addr, int thumb) {
    (void)ret_addr;
    (void)thumb;
    switch (num) {
    case 0x00:
        gba_soft_reset();
    case 0x02:
        gba_halt();
        break;
    case 0x03:
        gba_stop();
        break;
    case 0x06:
        swi_div();
        break;
    case 0x07: {   /* DivArm: r0/r1 swapped */
        uint32_t t = cpu.r[0];
        cpu.r[0] = cpu.r[1];
        cpu.r[1] = t;
        swi_div();
        break;
    }
    case 0x0B:
        swi_cpuset(0);
        break;
    case 0x0C:
        swi_cpuset(1);
        break;
    case 0x12:     /* ARM 'swi 0x123456' semihosting exit (dead code) */
    case 0xAB:     /* Thumb semihosting (dead code) */
        break;
    default:
        plat_log("unhandled SWI %02X at %08X", (unsigned)num, (unsigned)ret_addr);
        break;
    }
}

/* ------------------------------------------------------------ panic */

void gba_panic(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_log("PANIC: %s", buf);
    for (int i = 0; i < 16; i += 4)
        plat_log("r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X", i, (unsigned)cpu.r[i], i + 1,
                 (unsigned)cpu.r[i + 1], i + 2, (unsigned)cpu.r[i + 2], i + 3, (unsigned)cpu.r[i + 3]);
    plat_log("next_pc=%08X cpsr=%08X", (unsigned)cpu.next_pc, (unsigned)cpu_get_cpsr());
    char msg[320];
    snprintf(msg, sizeof msg, "%s\n\nguest pc %08X lr %08X sp %08X", buf,
             (unsigned)cpu.next_pc, (unsigned)cpu.r[14], (unsigned)cpu.r[13]);
    plat_fatal(msg);
}

/* platforms with a screen override this to show the message */
__attribute__((weak, noreturn)) void plat_fatal(const char *msg) {
    (void)msg;
    abort();
}

/*
 * runtime.c - boot, soft reset and the top-level run loop.
 */
#include <setjmp.h>
#include <string.h>
#include "gba.h"
#include "io.h"
#include "rom.h"

void gba_mem_init(void);
void gba_dispatch_init(void);
void gba_run_from(uint32_t target, uint32_t sentinel_lr);

#define BOOT_SENTINEL 0xFFFFFFF0u

static jmp_buf reset_jmp;

void gba_soft_reset(void) {
    longjmp(reset_jmp, 1);
}

/* CPU state the BIOS leaves when it jumps to the cartridge */
static void cpu_boot_state(void) {
    memset(&cpu, 0, sizeof cpu);
    cpu.cpsr_ctl = CPSR_MODE_SYS;
    cpu.r[13] = 0x03007F00;
    cpu.bank_usr[0] = 0x03007F00;
    cpu.bank_svc[0] = 0x03007FE0;
    cpu.bank_irq[0] = 0x03007FA0;
}

void gba_boot(void) {
    static int dispatch_ready;
    if (!dispatch_ready) {
        gba_dispatch_init();
        dispatch_ready = 1;
    }
    setjmp(reset_jmp);
    gba_mem_init();
    gba_io_reset();
    cpu_boot_state();
    gba_run_from(0x08000000u, BOOT_SENTINEL);
    gba_panic("guest returned from the cartridge entry point");
}

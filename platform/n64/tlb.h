#ifndef TLB_H
#define TLB_H

#include <stdint.h>

typedef struct {
    uint32_t rom_refills;
    uint32_t emulated;
    uint32_t vram_marks;
    uint32_t unknown_branch;
    uint32_t last_va, last_epc;     /* most recent emulated access */
} tlb_stats_t;

extern tlb_stats_t tlb_stats;

/* map guest RAM and install the fault handler (after gba_mem is placed) */
void tlb_init(void);
/* VRAM write protection, 4KB page index (0..23; 16KB granularity inside) */
void tlb_vram_set_writable(uint32_t page4k, int writable);
/* forget the mapping of a 16KB ROM page (it is being evicted) */
void tlb_rom_unmap(uint32_t va16k);

/* rom_n64.c: host pointer to the resident 16KB ROM page at guest address
 * va16k, loading it when 'load' is set; NULL if not resident */
const void *rom_page16_load(uint32_t va16k, int load);

#endif

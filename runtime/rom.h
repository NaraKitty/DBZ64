/*
 * rom.h - cartridge ROM access.  The ROM image is kept in native 32-bit word
 * order (like all guest memory).  Platforms decide how much of it is
 * resident: the PC build maps all 8MB, the N64 build pages it from the cart.
 */
#ifndef ROM_H
#define ROM_H

#include "gba.h"

/* map resident ROM pages into the page tables (called by gba_mem_init) */
void gba_rom_map_init(void);
/* slow-path read of an aligned word from a non-resident page */
uint32_t gba_rom_read32(uint32_t aligned_addr);
/* used by platforms that page ROM in on demand */
void gba_mem_map_rom_page(uint32_t page, const void *host);

/* host pointer to a native-word copy of [addr, addr+len) or NULL */
const void *gba_rom_ptr(uint32_t addr, uint32_t len);

#endif

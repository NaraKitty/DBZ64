/*
 * bss_gap.ld - appended to the linker script the N64 Makefile makes from
 * libdragon's n64.ld (BUILD_DIR/dbz.ld).
 *
 * Guest memory (gba_mem, runtime/gba.h) must be aligned to 256KB, the TLB
 * page that maps EWRAM, so .bss gets that alignment too.  dbz.ld puts
 * gba_mem first in .bss (otherwise the few bytes of plain .bss that come
 * first, from crtbegin.o and newlib, would push it to the next 256KB
 * boundary), which leaves one padding: between the end of .sbss and the
 * start of .bss, anything from 0 to 256KB depending on the code size.  These
 * symbols tell main.c where it is, and the ROM cache uses it.
 */
__dbz_bss_gap_start = ADDR(.sbss) + SIZEOF(.sbss);
__dbz_bss_gap_end = ADDR(.bss);

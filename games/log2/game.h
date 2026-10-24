/*
 * game.h - Dragon Ball Z: The Legacy of Goku II (USA, ALFE): what the runtime
 * and the platforms need to know about this game.  Code discovery facts for
 * the tools are in game.toml next to this file.  Built with GAME=log2.
 */
#ifndef GAME_H
#define GAME_H

#define GAME_TITLE          "Dragon Ball Z: The Legacy of Goku II"
#define GAME_SHORT_TITLE    "DBZ LoG2"
#define GAME_CODE           "ALFE"
/* the only supported dump (tools/mkrom.py checks it) */
#define GAME_ROM_SHA1       "18e0715dec419f3501c301511530d2edcd590f8b"

/* N64: cartridge header title (20 chars max) and the SRAM save magic */
#define GAME_N64_ROM_TITLE  "DBZ LEGACY GOKU2"
#define GAME_SRAM_MAGIC     "DBZ2"

/* crt0 copies the hand-written ARM image ROM 0x087FD6BC -> IWRAM 0x03000000
 * (0xE7C bytes; game.toml [[code_copy]] declares the code part) */
#define GAME_IWRAM_IMAGE_SRC  0x087FD6BCu
#define GAME_IWRAM_IMAGE_SIZE 0x00000E7Cu

/*
 * IRQs: [0x03007FFC] = 0x03000824, an ARM dispatcher in IWRAM, replaced by
 * gba_irq_service() in runtime/io.c.  Handler table (u32 per IRQ bit) and
 * the "fired" u16 at table+0x38.  Handlers are called with lr = the Thumb
 * 'bx r4' trampoline; r4 holds the dispatcher's continuation.
 */
#define GAME_IRQ_DISPATCHER 0x03000824u
#define GAME_IRQ_TABLE      0x03000E3Cu
#define GAME_IRQ_FLAGS      0x03000E74u
#define GAME_IRQ_RET_LR     0x03000951u      /* Thumb 'bx r4' at 0x03000950 */
#define GAME_IRQ_RET_R4     0x03000934u      /* dispatcher continuation */
/* the order the dispatcher tests IE & IF: DMA2 (sound) first, then bits
 * 0..12; a lone GamePak IRQ (bit 13) hangs the original */
#define GAME_IRQ_ORDER 0x0400, 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, \
                       0x0040, 0x0080, 0x0100, 0x0200, 0x0800, 0x1000

/* The mode-1 world map's HBlank handler (Thumb 0x08011F88, native body in
 * games/log2/native.c) is served in the scheduler without the dispatcher
 * round trip (runtime/io.c).  That needs the game never to wait for HBlank
 * itself: its only Halt (SWI 2, 0x08020D6E) is the flag wait loop
 * 0x08020D40, whose callers wait for VBlank (1) or DMA2 (0x400). */
#define GAME_HBLANK_NATIVE    0x08011F89u
#define GAME_HBLANK_NATIVE_FN native_m1_hblank

/* hand-written IWRAM routines replaced by runtime/native.c ([[native]]) */
#define GAME_NATIVE_MIX_VOICE 0x03000520u
#define GAME_NATIVE_CLIP      0x03000704u

/* guest function lookup hash: 2^bits slots, at least twice the function count */
#define GAME_DISPATCH_HASH_BITS 13

/* 320x240 view: the field-layer resource factory (host --ws-decode-test) */
#define GAME_WS_RES_FACTORY 0x080056A1u

#endif

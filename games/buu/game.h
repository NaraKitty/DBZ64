/*
 * game.h - Dragon Ball Z: Buu's Fury (USA, BG3E): what the runtime and the
 * platforms need to know about this game.  Code discovery facts for the
 * tools are in game.toml next to this file.  Built with GAME=buu.
 */
#ifndef GAME_H
#define GAME_H

#define GAME_TITLE          "Dragon Ball Z: Buu's Fury"
#define GAME_SHORT_TITLE    "DBZ Buu's Fury"
#define GAME_CODE           "BG3E"
/* the only supported dump (tools/mkrom.py checks it) */
#define GAME_ROM_SHA1       "f1c4b07554d2a3b1ad2f325307051e775ce68087"

/* N64: cartridge header title (20 chars max) and the SRAM save magic */
#define GAME_N64_ROM_TITLE  "DBZ BUUS FURY"
#define GAME_SRAM_MAGIC     "DBZB"

/* crt0 copies the hand-written ARM image ROM 0x087B79A4 -> IWRAM 0x03000000
 * (0x1004 bytes; game.toml [[code_copy]] declares the code part) */
#define GAME_IWRAM_IMAGE_SRC  0x087B79A4u
#define GAME_IWRAM_IMAGE_SIZE 0x00001004u

/*
 * IRQs: [0x03007FFC] = 0x03000B6C, an ARM dispatcher in IWRAM (the same code
 * as LoG2's except for the test order), replaced by gba_irq_service() in
 * runtime/io.c.  Handler table (u32 per IRQ bit) and the "fired" u16 at
 * table+0x38.  Handlers are called with lr = the Thumb 'bx r4' trampoline;
 * r4 holds the dispatcher's continuation.
 */
#define GAME_IRQ_DISPATCHER 0x03000B6Cu
#define GAME_IRQ_TABLE      0x03000FB0u
#define GAME_IRQ_FLAGS      0x03000FE8u
#define GAME_IRQ_RET_LR     0x03000C99u      /* Thumb 'bx r4' at 0x03000C98 */
#define GAME_IRQ_RET_R4     0x03000C7Cu      /* dispatcher continuation */
/* the order the dispatcher tests IE & IF: DMA2 (sound), Serial and Timer3
 * (link cable library) first, then bits 0..5, 8, 9, 11, 12; a lone GamePak
 * IRQ (bit 13) hangs the original */
#define GAME_IRQ_ORDER 0x0400, 0x0080, 0x0040, 0x0001, 0x0002, 0x0004, 0x0008, \
                       0x0010, 0x0020, 0x0100, 0x0200, 0x0800, 0x1000

/* hand-written IWRAM routines replaced by runtime/native.c ([[native]]):
 * byte-identical to LoG2's 0x03000520 / 0x03000704 */
#define GAME_NATIVE_MIX_VOICE 0x03000868u
#define GAME_NATIVE_CLIP      0x03000A4Cu

/* guest function lookup hash: 2^bits slots, at least twice the function count */
#define GAME_DISPATCH_HASH_BITS 14

/* 320x240 view: the field-layer resource factory (host --ws-decode-test) */
#define GAME_WS_RES_FACTORY 0x08008087u

#endif

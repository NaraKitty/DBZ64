# DBZ: The Legacy of Goku II — N64 static recompilation

Goal: run the GBA game *Dragon Ball Z: The Legacy of Goku II* (USA, `ALFE`,
SHA-1 `18e0715dec419f3501c301511530d2edcd590f8b`) natively on a Nintendo 64
at a full 60 FPS. The view is 320×240 (more of the world is visible and the
HUD is re-anchored), with full music and sound effects and N64 controls.

No ROM, BIOS, game data or ROM-derived source is stored in this repository.
The recompiler regenerates the C code from the user's own ROM.

## Pipeline

```
baserom.gba ──tools/analyze.py──▶ functions.json ──tools/recomp.py──▶ gen/*.c
              games/<game>/game.toml ──┘                                │
runtime/*.c (GBA hardware model) + games/<game>/*.c                     │
  + platform/{host,n64}/*.c ◀───────────────────────────────────────────┘
        │
        ├─ host build (WSL gcc): headless PNG/WAV harness for validation
        └─ N64 build (libdragon): dbz.z64
```

### Games (`games/<game>/`)
The tools, runtime and platforms are shared by Webfoot's GBA *Dragon Ball Z*
games (the same engine and toolchain). Each game directory holds:
- `game.toml`: code discovery and code generation facts for the tools: the
  section `[layout]`, the crt0 `[[code_copy]]`, jump tables, extra entries,
  `noreturn`/`assume_unreachable` library functions, `[[native]]`
  replacements, the IWRAM IRQ code only the runtime enters (`[recomp]`), and
  the game's hook sites.
- `game.h`: what the runtime and the platforms need: the IRQ dispatcher's
  handler table, flags word, return sentinel and test order, the ROM identity
  (game code, SHA-1), the N64 cartridge title and SRAM save magic.
- C sources compiled into that game's build (the 320×240 field view
  `widescreen.c`).

`GAME=log2|buu` selects the game in `platform/host/build.sh`, the N64
Makefile and the run scripts; `tools/recomp.py --game NAME` and
`tools/mkrom.py --game NAME` read the same directory. Builds of different
games need different output directories.

### Buu's Fury (`games/buu/`)
The same engine with about twice LoG2's code: 4435 functions and 142k guest
instructions (3.3 MB of MIPS), plus Nintendo's link-cable and wireless
adapter libraries and ARM soft-float code. What differs from LoG2:
- Section layout and IWRAM image (ROM 0x087B79A4, 0x1004 bytes; only the
  code part, up to 0x03000F44, is declared to the recompiler).
- The IRQ dispatcher (0x03000B6C) tests Serial and Timer3 right after DMA2.
- EEPROM library V124 (64 Kbit, as LoG2); SWI 0x0B CpuSet in the link
  libraries; the IWRAM routines memcpy32/memset32/memcpy16/memset16 replace
  most DMA3 copies. They are native (`games/buu/native.c`), with the LZ
  decoder, the tile copies and the field map streamer (see the natives
  table below).
- The sound engine, mixer and FIFO model are LoG2's (`native_mix_voice` and
  `native_clip` apply unchanged).
- The 320×240 field view (`games/buu/widescreen.c`, `docs/WIDESCREEN.md`,
  "Buu's Fury") is LoG2's design with this game's addresses. Three things
  are new: the view also stays inside what the parallax layers can show,
  an empty layer class is accepted, and the field view stays on while the
  game pauses the field. Some emitters add signed y offsets as unsigned
  bytes, and their hooks correct for it.

Status: the host build plays from power-on through the logos, title, a new
game and the story intro into gameplay with no dispatch misses. It matches
the interpreter (RAM identical at every 500th VBlank outside the dead stack,
identical frames, audio and save). Against the mGBA oracle, with input
replayed by key read (the gameplay script, three 60,000-frame random-input
runs, credits, sleep, soft reset, multiplayer menu, idle title): 99.9% of
the pictures match exactly. The rest are frames where the game writes
during scan-out on hardware. The game state in RAM is identical apart from
sound-mixer timing, the stack and a CPU-load meter (0x03000FEC). Saves
match byte for byte. Option
changes are saved to EEPROM and read back at boot. The N64 build with the
RDP renderer runs the same script in ares with every checked frame
identical to the host's (128 checkpoints from the logos to Snake Way in the
native view; 48 screenshots of three runs in the 320×240 field view), and
keeps the save in SRAM. CPU time in ares: field play 5.4 ms game + 4.1 ms
draw at 320×240 (4.5 + 3.7 ms in the native view), dialogue with portraits
6.7 + 4.3 ms, logos and the animated title 3.4 + 1.1 ms (measurements
below). Code is 3.5 MB of MIPS and BSS 1 MB; the ROM cache gets 1.26 MB.

Open: the field's blend frames are expensive for the RDP (see
"Measurements"); the Mode-7 style overworld (HBlank per-line BG2
affine) and the scenes that write MOSAIC were not reached with scripted
input (the renderer's per-line affine and mosaic paths are checked with
synthetic states); register summaries of the ARM soft-float routines
(their save/restore of r4–r11 is not recognised, which widens the indirect
call summary by r8–r11); the link-cable and wireless modes only reach
"Waiting for a friend..." (cancel works; serial is not modelled); a game
progress save (only the options block has been written so far); coverage
beyond the start of the story (about 1,060 of the 4,435 functions have run).

### Code discovery (`tools/analyze.py`)
For LoG2 (`games/log2/game.toml`):
- Recursive descent from crt0, AgbMain (0x08000158) and the constructor table
  at 0x08023588.
- Seeds from the reference recomp's `game.toml` (753 verified function entries
  and jump tables).
- Prologue sweep and vtable/function-pointer harvesting over 0x080000C0–0x08030000.
- Hand-written ARM code copied by crt0 from ROM 0x087FD6BC to IWRAM 0x03000000
  (0xE1C bytes).
- Misses logged by the runtime are fed back as seeds.

### Code generation (`tools/recomp.py`, `tools/abi.py`)
- **One C function per guest function.** Registers are C locals and the flags
  are four 0/1 locals.
- **Compare and branch fuse** into one C comparison. Unused flag computations
  are left to the C compiler to remove.
- **Literal-pool loads become constants.** ROM is immutable, so the generator
  reads the value at compile time.
- **Stack accesses** (base register `sp`) take an IWRAM fast path.
- **Interprocedural register summaries** (`abi.py`) compute `IN` (registers a
  function reads) and `MOD` (registers it may change) for each function.
  - A call site stores only the callee's `IN` registers and reloads only its
    live `MOD` registers.
  - Save/restore through the stack (`push {r4-r7,lr} … pop`) is recognised by
    value-numbering entry values through registers and stack slots.
  - A transfer counts as a return only when its target is provably the entry
    `lr`.
- **Control flow:**
  - In-function branches are `goto`s, and jump tables are `switch`es.
  - Every function remembers its entry `lr`. An indirect transfer to that value
    is a C return.
  - Any other target goes through the function's dispatch block. It tries local
    resume labels first. Failing that it calls a known function deeper
    (`gba_jump`), or unwinds with `cpu.next_pc` set so a frame further up the C
    stack can continue there.
  - Callers check `cpu.next_pc` against the link value after every call. This
    keeps non-local returns correct.

## Runtime (`runtime/`)

**Memory** (`gba.h`, `mem.c`):
- Guest memory is stored as native 32-bit words. Byte accesses use `addr ^ 3`
  and halfword accesses `addr ^ 2` on the big-endian N64.
- The PC build uses a 4 KB page table.
- The N64 build maps guest RAM through the VR4300 TLB at the guest's own
  addresses, so an access is one `lw`. It keeps no page tables (512 KB): the
  runtime's few host-pointer lookups compute the page from the region.
  - I/O, unaligned and unmapped accesses fault into a handler
    (`platform/n64/tlb.c`) that emulates them.
  - Clean VRAM is write-protected in the TLB, so the first write to a page
    marks it dirty for the renderer.
- ROM lives on the cartridge and is paged into a cache in 16 KB pages
  (`platform/n64/rom_n64.c`), at most 2 MB (LoG2 2 MB, Buu's Fury 1.26 MB):
  the RAM between `.sbss` and `.bss`, which the 256 KB alignment of guest
  memory leaves unused (0–256 KB depending on the code size), then what the
  heap has left after the renderer and the game stack. The N64 Makefile
  links with a copy of libdragon's `n64.ld` that puts guest memory first in
  `.bss` (`platform/n64/bss_gap.ld`), so no padding is left inside `.bss`.
  - A miss is a PI DMA with interrupts off. The game's hot pages
    (`game.toml` `[rom] preload`, found with `tools/rom_hotset.py` from host
    traces) are therefore loaded at boot; at most 7/8 of the cache is
    pinned that way.
  - Eviction is LRU-approximate.

**Timing model** (`io.c`):
- Guest code runs in zero time. Virtual time advances only inside `Halt`
  (SWI 2), which is where the game waits for its IRQ flags.
- The scheduler walks the frame's events:
  - an audio FIFO block every 16 samples (DMA2 IRQ)
  - HBlank lines, only while the mode-1 world map's HBlank IRQ is enabled
  - VBlank at line 160: render and present first, then raise the VBlank IRQ
- The game's IWRAM IRQ dispatcher (LoG2 0x03000824; `GAME_IRQ_*` in
  `game.h`) is replaced by a native version with the same priority and
  IE/IF/flag semantics. Handlers run as recompiled code.
- An HBlank handler with a native body (`GAME_HBLANK_NATIVE`: LoG2's line
  table copy `0x08011F88`) is served in place: the dispatcher's flag word and
  stack effects are reproduced and the handler runs directly, and all the
  HBlank lines up to the next other event (audio block, timer, VBlank) go in
  one batch. This needs the game never to wait for HBlank (its only Halt loop
  waits for VBlank or DMA2). Same RAM, I/O and audio as the generic path.
- The BIOS is not executed. SWI 0/2/3/6 are native.

**Native replacements** (`runtime/native.c`, `[[native]]` in `game.toml`):
- A few hot routines are replaced by C with exactly the same results.
- Each one sets the memory, registers and flags its callers can observe,
  taken from the recompiler's summary of the original.
- They are verified bit-exact on the host: pictures and audio over 8000
  frames, with and without them.

| Routine | What it does | Why |
|---|---|---|
| `0x03000520`, `0x03000704` | The sound mixer's voice loop and clip/pack | Hottest audio code |
| `0x03000480` | Strided 8x8 sprite-tile gather | Per-access translation overhead |
| `0x080054A4` | Field-layer tilemap rebuild | Replaces ~256 DMAs per frame, and only writes (and dirties VRAM) when a map really changes |
| `0x08011F88` (`games/log2/native.c`) | Mode-1 HBlank handler: the next line's BG2 affine registers from the line table | Five emulated I/O accesses per line, 228 lines per frame |
| `0x0801278A` (`games/log2/native.c`) | Mode-1 sky panorama into its hardware map | 64 DMA3 row copies (4 emulated I/O stores each) per frame |

Buu's Fury (`games/buu/native.c`; the mixer pair at 0x03000868/0x03000A4C
and the tile gather at 0x030004C0 are the shared ones):

| Routine | What it does | Why |
|---|---|---|
| `0x030007FC`, `0x03000768`, `0x030007A8`, `0x03000858` | memcpy16, memcpy32, memset32, memset16 | The game's VRAM, palette and OAM uploads and most RAM copies |
| `0x08007D98` | Field map streamer: 64 memcpy16 row copies per layer and frame | Rows already in place are skipped, so VRAM stays clean |
| `0x03000040` | Bit-stream LZ decoder | Resources, map chunks, sprites; the animated title decodes every frame |
| `0x03000388` | Linear bitmap to 8x8 tiles | Title animation |

The copy and fill natives write VRAM through host pointers and mark it
dirty only where the data changes; the renderer then re-checks only real
changes.

**Audio:**
- The game's own tracker and mixer run as recompiled code, apart from the
  native routines above.
- The runtime consumes the two 1 KB ring buffers like the DMA1/DMA2 sound FIFOs
  would: 15,978 Hz, signed 8-bit, FIFO A on the left and FIFO B on the right.

**Saves:** the 64 Kbit (8 KB) EEPROM's bit-serial protocol is emulated. On N64
it is stored in 32 KB cartridge SRAM.

## Measurements (ares, N64 + Expansion Pak)

CPU time per frame. "Game" covers the recompiled code plus the runtime
(IRQs, mixing, DMA). "Draw" is the RDP renderer's CPU side. The RDP itself
works in parallel.

| Scene (320×240 view, RDP renderer) | Game | Draw | Spare of 16.7 ms |
|---|---|---|---|
| Logos and title | 1.2–2.4 ms | 0.5–1 ms | ~13 ms |
| Field, quiet | 5.5 ms | 1.7 ms | ~9 ms |
| Field, heaviest stretch measured | 6.6–7.1 ms | 3.8–4.0 ms | ~5.5 ms |
| Mode-1 world map, 320×240 | 7.5–7.9 ms | 9.6 ms heading north, 12–17 ms turning or streaming | none: drawing skips frames |
| Mode-1 world map, native view (`wsoff`) | 6.5–7.0 ms | 6.4 ms heading north, 7.6–12 ms otherwise | ~0–3 ms |

The world map started at 20 ms of game time (an emulated I/O trap for every
register access of the per-line HBlank handler, 228 lines a frame) and a
16 ms draw (14 ms of it the CPU rasteriser for the affine ground). Game time
is now under 8 ms, so music and game speed stay steady. At 320×240 the
ground's CPU rasteriser (46,000 pixels, 7–13 ms) keeps the draw over the
frame budget, and the platform skips drawing to keep the game on time.

On that heaviest stretch, the same frame started this round of work at
10.5 + 7.5 ms, which missed 60 fps. Mid-game cartridge reads went from 76
to 0 over the scripted playthrough.

Buu's Fury (RDP renderer; means of the 120-frame windows of each scene of
the gameplay script, every frame drawn):

| Scene | Native view: game + draw | 320×240 field view: game + draw | Before the merge with the blend cells (native) |
|---|---|---|---|
| Logos and the animated title | 3.5 + 1.0 ms | 3.4 + 1.1 ms | 3.3 + 0.8 ms |
| Story intro scenes | 4.2 + 1.5 ms | 4.8 + 1.6 ms | 4.0 + 1.4 ms |
| Snake Way dialogue with portraits | 5.5 + 4.3 ms | 6.7 + 4.3 ms | 5.2 + 2.0 ms |
| Snake Way field play | 4.5 + 3.7 ms | 5.4 + 4.1 ms (heaviest 11.8 ms) | 4.3 + 1.6 ms |

"Before" is the renderer before the blend cells (part A of the Buu's Fury
bring-up, measured the same way).  The draw time of dialogue and field play
grew by about 2 ms: the game keeps BLDCNT `0x1041` there (BG0 a first
target at 12/0, OBJs the 2nd target), and the 2nd-target rule makes most
of BG0's cells go through the tagged 32-bit path (a plain pass plus one
draw per rectangle).  On the console that path is the expensive part
(estimated 15-22 ms of RDP time per frame in the native view and 29-41 ms
at 320×240, `docs/RDP_COST.md`), so these frames are RDP-bound there.
The gameplay script misses the ROM cache 78 times in the native build
(93 with the view), almost all at the story's scene changes (part A: 113).
LoG2 over the same merge (its 8000-frame timing script): field play 5.2 +
1.5 ms and the heaviest window 9.7 ms (main: 5.3 + 1.5 and 10.1 ms).

**Pacing** (`platform/n64/main.c`, `frame_pace`): the game runs on its own
real-time schedule, one GBA frame per 267.52 audio samples at the AI's
exact rate, and video never holds it up. Ahead of schedule it waits. More
than half a frame behind, the frame is not drawn (at most four in a row).
While the RDP is still busy with the previous frame or no framebuffer is
free, it waits only as long as this frame had to spare, then leaves the
frame undrawn. So a scene whose RDP work runs long (blend frames on real
hardware) loses drawn frames, while game speed, music and sound effects
stay steady. The CPU timer and the AI's clock come from different crystals,
so `pace_audio` nudges the schedule every frame (at most 1/32 of a frame)
toward keeping all but one of libdragon's audio buffers full; after a stall
the queue is refilled with silence at once.

**Memory:**
- Code: 1.66 MB of MIPS from 67.5k guest instructions, with TLB-mapped memory
  accesses (Buu's Fury: 3.5 MB).
- BSS: 0.9 MB (Buu's Fury: 1 MB), of which guest memory is 512 KB. It was
  1.6 MB while the N64 build kept guest page tables and guest memory sat
  behind 255 KB of padding.
- ROM page cache: 2 MB, with 1 MB of heap to spare (Buu's Fury: 1.26 MB,
  what is left).
- The game stack's top is placed at offset 0x1000 of the D-cache's 8 KB
  index space; where it falls was worth up to 1.2 ms per frame.

**Fault-emulated accesses** run at about 3 per frame in steady state: the sound
IRQ handler's DMA register writes, which use a register-relative address.
Buu's Fury adds 8 in field play: the field layers' scroll registers, also
written through a register (0x08007F2E).

**Verification:**
- The host build is deterministic.
- The native mixer routines produce byte-identical audio to the recompiled
  originals.
- Frames are compared against the software reference renderer, and that
  renderer was fuzzed against an independent model.

## Debugging recompiled code (host)

Guest time only advances in Halt, so the host build, the N64 build and the
interpreter all see exactly the same game state at every VBlank. That makes
the interpreter (`runtime/interp.c`) a reference for the recompiled code:

1. Generate a traced copy of the code: `tools/recomp.py ... OUTDIR --game G
   --trace`. Every function then starts with `if (TRACE_ENTER(addr)) return;`.
2. Build it with `DEBUG=1 GAME=G platform/host/build.sh OUTDIR BUILD`, which
   adds `platform/host/debug.c`: it keeps the last 4096 function entries and
   prints them when the run dies.
3. `GBA_INTERP_FN=lo-hi[,lo-hi...]` runs the functions entered in those
   ranges, and everything they call, in the interpreter. (A dispatch miss
   interprets only the missing function: the interpreter puts back the
   registers its recompiled callees preserve, see `runtime/dispatch.h`.)
   `08000000-08000001` (the reset entry) interprets the whole game. If a
   failure disappears when a range is interpreted, bisect the range down to
   the function whose recompiled code misbehaves.
4. `--dump-ram F,F,..` writes IWRAM and EWRAM at those VBlanks
   (`ram_NNNNN.bin`, the mGBA oracle's layout). Compare a recompiled and an
   interpreted run, or the oracle. Only the dead stack area may differ:
   recompiled code does not keep dead registers' values, which functions
   still push.

## N64 platform (`platform/n64/`)

**Video:**
- The RDP draws GBA layers from guest VRAM as CI8 texture rectangles with TLUT
  palettes.
- Saturating additive blends (EVA+EVB > 16) use a half-intensity pass so the
  blender cannot overflow.
- Affine BGs are rasterised on the CPU. A wrapping 8bpp affine BG2 of up
  to 512x512 (the mode-1 world map's ground) is sampled from a resolved
  texture (map and tiles already looked up, 8x8 tiles in map order) kept in
  the memory of text rings 2/3, which modes 1/2 do not use, through its
  cached alias. It follows VRAM through the BG chunk tracking. Per pixel:
  one load with dilated fixed-point coordinates (three instructions advance
  both); unrotated lines copy their texel row first; a line with the
  parameters of the line above is a copy.
- Alpha blending goes through a 32-bit intermediate, which is slow on
  hardware, so only the 8x8 cells where a first target can have a pixel use
  it; the rest of the frame takes the plain COPY path.  The game keeps BLDCNT
  set for an empty BG0 in its field maps and during dialogues, so those
  frames cost the same as ordinary ones.  The cost model behind this and its
  calibration on the Analogue 3D: `docs/RDP_COST.md`.
- Where drawing one OBJ at a time is not the GBA's OBJ layer, the OBJ pixels
  of a rectangle are composited on the CPU with the reference renderer's
  rules: a later OBJ with a better priority over an earlier one (its
  transparent texels still pass their priority down), OBJ mosaic, and an
  OBJ that blends over another OBJ. The composed pixels are drawn as one
  item per (priority, semi-transparency) and take part in the blend cells
  like any OBJ (their cells: the boxes of the OBJs with that code inside the
  rectangle); the other OBJs are clipped around the rectangle.
- BG mosaic: text BGs with mosaic are drawn in software (every cell counts
  as touched for the blend cells), affine and bitmap BGs apply it in their
  software raster (an affine BG2 with mosaic never uses the resolved
  texture).

**320×240 view:**
- On field maps, background margins come from the game's authored map chunks.
  The decompressor is ported from the reference mod.
- Actor culling bounds are widened at the exact read sites.
- Sprite coordinates come out of the sprite builder as full signed values.
- HUD objects are re-anchored to the screen edges.
- The mode-1 world map fills 320×240 too: the ground plane's per-line table
  is extended below the screen with the game's own formula, the sky and haze
  layers are extended, and the minimap moves to the corner.
- Other screens show the native 240×160 frame centred.
- Buu's Fury has the same field view (its own addresses and hook sites);
  its other screens, the Mode-7 style overworld included, stay native.
- Details, verification and limitations: `docs/WIDESCREEN.md`.

**Controls:**

| GBA | N64 |
|---|---|
| D-pad | D-pad or analog stick |
| A, B, Start, R | A, B, Start, R |
| L | L or Z |
| Select | C-Down |

## References
- `github.com/mstan/DragonBallZLegacyofGokuIIRecomp` and
  `github.com/mstan/gbarecomp` (PolyForm Noncommercial): a PC recomp of the
  same ROM. We used its function seeds, the IRQ and audio findings, and the
  widescreen field-plane technique.
- GBATEK, for hardware semantics.

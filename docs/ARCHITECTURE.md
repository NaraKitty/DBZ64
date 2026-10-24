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
- C sources compiled into that game's build (for LoG2, the 320×240 field view
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
- No 320×240 field view yet: `games/buu/widescreen.c` always gives the
  native view.

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
RDP renderer runs the same script in ares with every checked frame (55,
from the logos to the options menu) identical to the host's, and keeps the
save in SRAM. CPU time in ares: field play 4.5 ms game + 1.7 ms draw,
dialogue with portraits 5.5 + 2.1 ms, the animated title 5.4 + 0.9 ms
(measurements below). Code is 3.4 MB of MIPS and BSS 1.15 MB; the ROM
cache gets the remaining 1.2 MB.

Open: the field view; the Mode-7 style overworld (HBlank per-line BG2
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
  (`platform/n64/rom_n64.c`): what the heap has left after code, BSS, the
  renderer and the game stack, at most 2 MB (LoG2 2 MB, Buu's Fury 1.25 MB).
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
  - HBlank lines, only while the Mode-7 scene's HBlank IRQ is enabled
  - VBlank at line 160: render and present first, then raise the VBlank IRQ
- The game's IWRAM IRQ dispatcher (LoG2 0x03000824; `GAME_IRQ_*` in
  `game.h`) is replaced by a native version with the same priority and
  IE/IF/flag semantics. Handlers run as recompiled code.
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

On that heaviest stretch, the same frame started this round of work at
10.5 + 7.5 ms, which missed 60 fps. Mid-game cartridge reads went from 76
to 0 over the scripted playthrough.

Buu's Fury (native 240×160 view, RDP renderer; means of the 120-frame
windows of each scene of the gameplay script, then a 16,600-frame stretch
of random field play):

| Scene | Game | Draw | Before this round (game + draw) |
|---|---|---|---|
| Logos | 2.1 ms | 0.7 ms | 2.1 + 0.7 ms |
| Title (animated, decoded every frame) | 5.4 ms | 0.9 ms | 6.8 + 0.9 ms |
| Story intro scenes | 4.1–4.2 ms | 1.0–2.5 ms | 4.8–5.4 + 2.0–2.9 ms |
| Snake Way dialogue with portraits | 5.5 ms | 2.1 ms | 7.1 + 3.5 ms |
| Snake Way field play | 4.5 ms | 1.7 ms | 5.7 + 2.5 ms |
| Status, journal, options | 3.3 ms | 0.8 ms | 3.5 + 0.9 ms |
| Random field play, median window | 4.5 ms | 1.5 ms | 5.7 + 2.2 ms |
| Random field play, heaviest window | 5.6 ms | 2.0 ms | 8.9 + 2.2 ms (with ROM cache misses) |

"Before" is 34d29df, whose RDP build ran out of RDRAM for the 2 MB ROM
cache (it is measured with the cache cut to 768 KB, which then missed 930
times in that random stretch).  The ROM cache now misses only when new
content is first used: none during the script's field play, 14 in the
random stretch.  LoG2 over the same round: its heaviest stretch 9.2 ->
8.7 ms (game + draw), quiet field play unchanged.

When a frame runs over anyway, the platform skips drawing (at most two
frames in a row) instead of slowing the game down.

**Memory:**
- Code: 1.66 MB of MIPS from 67.5k guest instructions, with TLB-mapped memory
  accesses (Buu's Fury: 3.4 MB).
- BSS: 1.15 MB in both games (1.6 MB before the N64 build dropped the
  guest page tables).
- ROM page cache: 2 MB (Buu's Fury: 1.2 MB, what is left).
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
- The Mode-7 layer is drawn in software.
- Where drawing one OBJ at a time is not the GBA's OBJ layer, the OBJ pixels
  of a rectangle are composited on the CPU with the reference renderer's
  rules: a later OBJ with a better priority over an earlier one (its
  transparent texels still pass their priority down), OBJ mosaic, and an
  OBJ that blends over another OBJ.
- BG mosaic: text BGs with mosaic are drawn in software, affine and bitmap
  BGs apply it in their software raster.

**320×240 view:**
- On field maps, background margins come from the game's authored map chunks.
  The decompressor is ported from the reference mod.
- Actor culling bounds are widened at the exact read sites.
- Sprite coordinates come out of the sprite builder as full signed values.
- HUD objects are re-anchored to the screen edges.
- Other screens show the native 240×160 frame centred.
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

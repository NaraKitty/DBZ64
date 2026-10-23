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
  most DMA3 copies (memcpy16 is a Duff's device whose computed-jump targets
  are seeded in game.toml).
- The sound engine, mixer and FIFO model are LoG2's (`native_mix_voice` and
  `native_clip` apply unchanged).
- No 320×240 field view yet: `games/buu/widescreen.c` always gives the
  native view.

Status: the host build plays from power-on through the logos, title, a new
game and the story intro into gameplay with no dispatch misses, matching the
interpreter (RAM identical at every checked VBlank outside the dead stack)
and the mGBA oracle's audio. The N64 build (CPU renderer) runs the same
script in ares with frames identical to the host's.

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
- The N64 build will map guest RAM through the VR4300 TLB at the guest's own
  addresses, so an access is one `lw`. I/O and unmapped addresses fault into a
  handler.

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

**Audio:**
- The game's own tracker and mixer run as recompiled code.
- The runtime consumes the two 1 KB ring buffers like the DMA1/DMA2 sound FIFOs
  would: 15,978 Hz, signed 8-bit, FIFO A on the left and FIFO B on the right.

**Saves:** the 64 Kbit (8 KB) EEPROM's bit-serial protocol is emulated. On N64
it is stored in 32 KB cartridge SRAM.

## Measurements (ares, N64 + Expansion Pak)

These figures are game-logic CPU time per frame (recompiled code plus the
runtime, including IRQs, mixing and DMA), with drawing excluded:

| Scene | Time per frame |
|---|---|
| Logos and title | 1–2.3 ms |
| Story cutscenes | ~8 ms |
| Field gameplay | 8–10.5 ms |

The frame budget is 16.7 ms. The RDP renderer's CPU side has to fit in the
remainder; the RDP itself works in parallel.

**Memory:**
- Code: 1.66 MB of MIPS from 67.5k guest instructions, with TLB-mapped memory
  accesses.
- BSS: 1.5 MB.
- ROM page cache: 2 MB.

**Fault-emulated accesses** run at about 3 per frame in steady state: the sound
IRQ handler's DMA register writes, which use a register-relative address.

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
   ranges, and everything they call, in the interpreter.
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

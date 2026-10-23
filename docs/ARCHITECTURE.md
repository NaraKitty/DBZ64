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
                                                                       │
runtime/*.c (GBA hardware model) + platform/{host,n64}/*.c ◀───────────┘
        │
        ├─ host build (WSL gcc): headless PNG/WAV harness for validation
        └─ N64 build (libdragon): dbz.z64
```

### Code discovery (`tools/analyze.py`)
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
- The IWRAM IRQ dispatcher at 0x03000824 is replaced by a native version with
  the same priority and IE/IF/flag semantics. Handlers run as recompiled code.
- The BIOS is not executed. SWI 0/2/3/6 are native.

**Audio:**
- The game's own tracker and mixer run as recompiled code.
- The runtime consumes the two 1 KB ring buffers like the DMA1/DMA2 sound FIFOs
  would: 15,978 Hz, signed 8-bit, FIFO A on the left and FIFO B on the right.

**Saves:** the 64 Kbit (8 KB) EEPROM's bit-serial protocol is emulated. On N64
it is stored in 32 KB cartridge SRAM.

## N64 platform (`platform/n64/`) — in progress

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

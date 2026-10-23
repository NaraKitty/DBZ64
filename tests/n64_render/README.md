# N64 GBA renderer test harness

Validates `platform/n64/render` (the RDP renderer) against `runtime/ppu_soft.c`
(the software reference) on GBA display states, running the test ROM headless in
ares.

## Files

| File | Purpose |
|---|---|
| `main.c` | Test ROM: loads states from DFS, renders, dumps framebuffers and timings over the debug channel |
| `stubs.c` | `gba_mem`, `gba_line_regs*`, `gba_vram_take_dirty()` so the ROM links without the runtime |
| `host_ref.c` | Host program: renders a state with `ppu_soft` to a raw 240x160 BGR555 file |
| `mksynth.py` | Generates the synthetic states (features the game captures do not cover) |
| `build.sh` | WSL: builds `host_ref`, generates states and references, builds the ROM |
| `run.sh` | Git Bash: runs the ROM in ares and calls `fbdiff.py` |
| `fbdiff.py` | Parses the ares log, diffs every dump against its reference, prints the report |
| `Makefile` | libdragon build, run from the build directory (see `build.sh`) |

## Running

```sh
# WSL (Ubuntu): build references, states and the ROM into /mnt/d/dbz-n64/build/n64_render
bash tests/n64_render/build.sh
# Git Bash on Windows: run ares headless and diff (writes build/n64_render/diff/*.png)
bash tests/n64_render/run.sh
```

`build.sh [OUT] [STATES_DIR]` takes the game captures (`state_NNNNN.bin`) from
`/mnt/d/dbz-n64/runs/states` by default.  Environment variables:

* `ONLY="state_03000 synth_obj1d"`: limit the state list
* `NO_CALIB=1`: skip the in-ROM blend calibration (saves about 20 s)
* `EXTRA_CFLAGS`: e.g. `-DRDPQ_VALIDATE` (libdragon's RDP validator, slow),
  `-DRDPQ_VALIDATE -DRDPQ_LOG_FRAME=3 -DRDPQ_LOG_FRAME2=-1` (log every RDP
  command of frame 3), `-DMICROBENCH` (CPU memory micro-benchmarks),
  `-DPREFILL` (poison display buffers before rendering), `-DREADBACK_CHECK`,
  `-DBENCH` (32-frame CPU averages per state for the dirty patterns steady /
  scroll / obj / bgmap / all; `-DBENCH_SCEN=k` one of them), `-DPROFILE=50`
  (PC sampling every 50 us inside `gba_render_frame`, implies the bench flow;
  `-DPROFILE_EXACT` samples single instructions instead of 32-byte lines),
  `-DBENCH_WIDE` (bench / profile the 320x240 view with the hooks of the
  WIDE test, like the in-game field view)

From Git Bash the WSL step is
`MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash /home/nara/dbzn64/tests/n64_render/build.sh`.

## What the ROM checks

For every state in `rom:/states.txt`:

1. **Reference comparison**: three renders: pass 0 after `gba_render_invalidate()`
   with all pages dirty (cold caches), pass 1 with nothing dirty (steady state),
   pass 2 with all 24 VRAM pages dirty but caches warm.  All three must be
   identical (`p0/p1/p2` column); pass 1 is dumped and compared with the
   `ppu_soft` reference over the GBA screen (40,40)-(280,200); the rest of the
   320x240 frame must be black (`border` column).
2. **`DYN`**: incremental cache test.  Scrolls every BG by (3+n, 2) pixels per
   frame for 24 frames with nothing dirty, then rewrites tile data, map entries
   and OBJ tiles and reports the touched pages dirty.  Each incremental result
   must equal a cold render of the same state.
3. **`WIDE`**: a 320x240 region with `bg_entry` / `obj_pos` hooks that return the
   hardware values; the 240x160 centre must equal the native render and the
   whole frame must equal `ppu_soft_render_view` with the same hooks.
   **`WIDE2`**: the same region without hooks (hardware map entries and OBJ
   positions around the GBA screen) against `ppu_soft_render_view` without hooks.

The test ROM is built with `-DGBA_RENDER_HWCHECK`: a texture load whose start
coordinate is outside [0, 1024) (the Analogue 3D wraps LOAD_BLOCK's start row
at 1024, ares does not) is logged as `ERROR HWCHECK` and shown by `fbdiff.py`.

Then the **blend calibration** renders BG2 over BG3 with every (EVA, EVB),
each 32x32 block containing all 1024 (a, b) colour value pairs per channel,
and compares every pixel with the GBA formula `min(31, (a*EVA + b*EVB) >> 4)`.

The `rdpEst` column is an estimate from the pixel and load counters (ares does
not model RDP timing): FILL/COPY 4 px/clk, 1-cycle 1 px/clk, 2-cycle 0.5 px/clk,
loads 8 bytes/clk, 24 clocks per rectangle and per load, at 62.5 MHz.

## Synthetic states (`mksynth.py`)

| State | Covers |
|---|---|
| `synth_bgtext` | 4bpp + 8bpp text BGs, palette banks, H/V flips, all 4 map sizes, 16-bit world scroll values |
| `synth_obj1d`, `synth_obj2d` | all 12 OBJ shapes/sizes, 8bpp + 4bpp, H/V/HV flips, priorities against BGs, OAM ties, X/Y wrap, 1D and 2D mapping |
| `synth_affobj` | affine OBJs: scale, non-uniform scale, rotation, double size, shear, matrix mirror, 4bpp, off-screen edges |
| `synth_mode7` | mode 1 BG2 512x512 wrap with per-line parameters (perspective), per-line BG1 scroll bands, EVA 6 / EVB 16 blend |
| `synth_sat9_16`, `synth_sat16_16`, `synth_sat14_6` | saturating blends (EVA+EVB > 16) of BG0 over BG1-3 and OBJs |
| `synth_mode4`, `synth_mode4_affine` | bitmap mode 4 (page 0 / page 1 + scaled), OBJ tiles below 512 hidden |
| `synth_mode3_dark` | bitmap mode 3 with darken |
| `synth_bright`, `synth_dark` | BLDY brighten / darken |
| `synth_fade` | menu-style fade (BLDCNT 0x3F5F, EVB 0) |
| `synth_cross` | crossfade EVA 9 / EVB 7 |
| `synth_semi` | semi-transparent OBJs |
| `synth_approx_bd` | first target directly over the backdrop, which is not a 2nd target |
| `synth_blank` | forced blank |
| `synth_objwrap` | OBJs at signed x -256..-233 (must not wrap to the right in a wider view), x / y wrap controls |
| `synth_semi_bright2` | semi-transparent OBJs over a layer that is both a 1st target of brighten and a 2nd target |
| `synth_semi_dark_bd` | the same with darken and the backdrop as 1st + 2nd target |
| `synth_bgedge` | text BG tiles past the end of BG VRAM, a 512x512 map reaching into OBJ VRAM, 16-bit scrolls |
| `synth_obj4_many` | 48 4bpp 64x64 OBJs (the per-frame scratch tile area fills several times) |
| `synth_mosaic_bg` | text BG mosaic (4bpp banks, 8bpp, 512-wide maps, 16-bit scrolls) with mosaic OBJs |
| `synth_mosaic_obj` | OBJ mosaic: regular (flips, 4bpp/8bpp) and affine OBJs, partly off-screen |
| `synth_mosaic_mode7` | mode 1 BG2 with per-line parameters and BG1 per-line scroll, both with mosaic |
| `synth_mosaic_mode4`, `synth_mosaic_mode3` | bitmap modes with BG2 mosaic (scaled mode 4, mode 3 with darken) |
| `synth_objprio` | the OBJ layer's transparent-texel priority rule: worse-priority OBJs under later better-priority ones, semi-transparent and affine ones, with alpha blending |

The renderer composites the OBJ pixels of a rectangle on the CPU where
drawing one OBJ at a time would not give the GBA's OBJ layer (see
`obj_cpu_begin` in `render_obj.c`); the `synth_obj*`, `synth_semi*`,
`synth_mosaic*` and `synth_objprio` states exercise it.  `synth_sat14_6`
is the one known difference: the 14/6 alpha blend has no exact RDP
configuration (off by one in some channels).

# RDP cost of the renderer

ares (paraLLEl-RDP) draws the RDP's output exactly but reports almost no RDP
time, so it cannot show how long a frame takes on a console.  On the Analogue
3D, frames that used the alpha-blend path took 36-46 ms of "draw" in the
performance overlay (about 17 frames drawn per second), while ordinary frames
took about 5 ms.  This document describes the cost model used to find and
remove that time, how it was calibrated, and what it predicts.

## The model (`tests/n64_render/rdpcost.py`)

The `CMDLOG` build of the renderer test ROM (`tests/n64_render/main.c`,
`EXTRA_CFLAGS="-DRDPQ_VALIDATE -DCMDLOG"`) logs, through rdpq's trace engine,
every command the RDP receives for one steady-state frame of each state, in
the 320x240 field view and in the native view.  `rdpcost.py` replays that
stream: render mode (cycle type, colour/Z read and write), colour image depth,
scissor, texture image and loads.  Every primitive is split into its spans
(scanlines) and charged:

```
span = max(pipeline, memory) + penalty * width + read latency (if it reads) + fixed
pipeline: FILL 16-bit 0.25 clk/px, FILL 32-bit 0.5, COPY 0.25, 1-cycle 1, 2-cycle 2
memory:   bytes of colour (2 or 4 per pixel) and Z (2) read and written / bw
```

plus fixed costs per primitive, command, sync, TMEM load (and per line of a
LOAD_TILE) and TLUT load.  At 62.5 MHz.

| Constant | Value | Meaning |
|---|---|---|
| `px_*` | see above | pixel pipeline, clocks per pixel |
| `bw` | 4 bytes/clk | effective RDRAM bandwidth of span and texture transfers (peak 8) |
| `z_px` | 8.5 clk/px | penalty of a span that reads or writes Z (calibrated, see below) |
| `span`, `span_fc` | 8, 2 clk | fixed cost per span, 1/2-cycle and fill/copy |
| `read_lat` | 30 clk | latency of a span that must read colour or Z first |
| `prim` | 20 clk | per rectangle |
| `load`, `load_row` | 30, 6 clk | per TMEM load, per LOAD_TILE line |
| `tlut` | 200 clk | per LOAD_TLUT |
| syncs | pipe 30, load 15, tile 8, full 150 | |

## Calibration

The overlay's "draw" D is not the RDP time.  With the pacing of
`platform/n64/main.c` at the time of the measurements, a drawn frame waits in
`gba_render_frame` for the previous frame's RDP work, and the two frames
after a long one are skipped (late), so per drawn frame

    RDP time R = D + 3 x game time (about 6 ms) - CPU drawing time (2-3 ms) = D + 16 ms

which also gives the observed ~17 frames per second (1000 / (D + 18)).  Targets:

| Scene (test state) | Measured D | Target R |
|---|---|---|
| Forest / dialogue with BLDCNT 14/6 (`state_57000`, `state_22100`) | 43.8-45.6 ms | 61 ms |
| Pepper Town rain (`state_17400`) | 46 ms | 62 ms |
| Houses with sunbeams (`state_22800`, `state_24000`) | 36-39 ms | 54 ms |
| Ordinary field frames (`state_04500`, `state_03300`) | ~5 ms (CPU-bound) | a few ms |

The ideal pipeline rates alone give 12-28 ms for these blend frames, so most of
their time is something the ideal model does not have.  Every 32-bit span the
old blend path drew also wrote or read Z (the 2nd-target tags were on in all of
these scenes), and those spans make up most of the frame, so a per-pixel
penalty on them fits the measurements: least squares over the five blend
states gives 8.5 clk/px (`rdpcost.py --fit`), with 22% rms error.  The
residuals have a pattern: the model puts the rain at 85 ms and the
dialogue/forest states at 46-47 ms although both measured about 45 ms of draw;
the user's dialogue scenes are probably heavier than the captured ones (more
layers or a non-empty BG0), or the rain's extra Z reads cost less than its Z
writes.

The measurements cannot say whether the penalty belongs to the Z traffic or to
32-bit colour spans in general, because in the old path they always came
together.  Two calibrations are therefore kept:

* **z** (default): `z_px = 8.5` on spans that read or write Z.  Plausible if
  the colour and Z streams thrash RDRAM pages (the old intermediate and its Z
  buffer were allocated back to back, in the same 1 MB bank).
* **c32**: `c32_px = 8.5` on every 1/2-cycle span with a 32-bit colour image.

`EXTRA_CFLAGS=-DRDPBENCH` builds a test ROM that measures the relevant cases
on the console (fills, COPY / 1-cycle / 2-cycle chunks into 16- and 32-bit
targets, with Z and blending, with the Z buffer in the colour buffer's 1 MB
bank and in another one) and shows them on screen; its numbers would settle
the question and the constants.

## Results

Estimated RDP time per frame, 320x240 field view (`rdpcost.py --compare`):

| Scene | before (z) | after (z) | before (c32) | after (c32) | 32-bit pixels after |
|---|---|---|---|---|---|
| Ordinary field (`state_04500`) | 2.8 | 2.8 | 2.8 | 2.8 | 0 |
| Dialogue, BG0 nearly empty (`state_22100`) | 47.2 | 3.9 | 47.2 | 4.2 | 640 |
| Forest, BG0 empty (`state_57000`) | 46.3 | 3.5 | 46.3 | 3.5 | 0 |
| Forest (`state_61000`) | 48.6 | 3.9 | 48.6 | 4.5 | 1,536 |
| Sunbeams 8/16 (`state_22800`) | 53.4 | 6.4 | 53.4 | 13.3 | 22,784 |
| Sunbeams 9/16 (`state_24000`) | 51.7 | 7.8 | 51.7 | 13.8 | 21,376 |
| Rain 16/12 (`state_17400`) | 85.3 | 16.8 | 85.3 | 40.1 | 76,800 |

The synthetic states added for these scenes (`mksynth.py`; no "before", they
are new): `synth_dialog` (14/6 with an empty BG0) 4.0 / 4.0,
`synth_dialog_corner` (one dialogue corner tile in BG0) 4.4 / 5.0,
`synth_sunbeam` 5.6 / 7.5, `synth_rain` (16/12 over an opaque BG3) 16.5 /
49.3, `synth_rain_hole` (the same with a transparent BG3 tile in every 7th
map entry: tagged cells scattered over every row, so every row is tagged, the
worst case) 75.8 / 75.8.

What changed (all exact: every framebuffer of the renderer test, 59 states
with their DYN/WIDE/SEQ sequences and the blend calibration, is identical to
the previous renderer's):

* **Blend region.**  The 32-bit path covers only rectangles of 8x8 cells
  where a first target (or a semi-transparent OBJ) may have an opaque pixel:
  per-tile "has an opaque pixel" flags of the BG layer cache and OBJ boxes.
  The game sets BLDCNT (BG0 first target, 14/6) all the time in the field
  around Goku's house and during dialogues, with BG0 empty or almost empty;
  those frames now draw like ordinary ones.
* **Tags only where needed.**  The renderer walks the layers bottom-up per
  cell, with per-tile "no transparent pixel" flags: a cell needs the Z tags
  only if a pixel under a first target there may not be a 2nd target.  The
  rain needs them in 40 cells (where BG3 has holes), the sunbeam house in 3.
  Tagless cells are drawn without Z and with each first target once.
* **Backdrop fill** skipped in cells an opaque tile covers before the first
  target.
* **Layers above the last first target** are drawn on the display after the
  final pass (16-bit, COPY / 1-cycle) instead of through the 32-bit path.
* **Sparse layers** in the 32-bit path: one rectangle per run of tiles with
  an opaque pixel, not per 64x32 chunk.
* **Final pass from the colours in view.**  The cheap 2x pass is used
  whenever the exact 2x configuration stays in range for the palette entries
  the first targets actually show; the TLUT pass drops its third pass when
  every blended pixel shows the one first target and one channel cannot reach
  31 (the rain: red and green stay below 31).

The masks cost CPU time: the layer cache keeps the per-tile flags as one
64-bit word per ring row and the palette index range of each slot (found
only when a TLUT final pass has to be chosen), OBJs are kept as boxes of
cells.  Every rectangle costs a draw of each item, so fragmented cell sets are
coarsened (still disjoint and exact, only more pixels go through the 32-bit
path or get tags): with at most 4 tagged rectangles and more than 8 tagless
ones, the tagless cells become one box per band of 4 cell rows (avoiding the
tagged rectangles); if there are still more than 16 rectangles, every cell row
with a tagged cell is tagged entirely and each band becomes one rectangle.
The masks and index ranges are cached between frames (they change only when
the layer cache refills a slot or the scroll moves to another cell), and
OBJs and layers above the last first target are drawn once over the whole
region.  CPU time of `gba_render_frame` measured in ares (`BENCH_WIDE`,
average steady / scrolling, ms):

| State | before | after |
|---|---|---|
| `state_22800` sunbeams | 1.3 / 1.6 | 2.7 / 4.3 |
| `state_24000` sunbeams | 2.2 / 2.5 | 4.0 / 5.4 |
| `state_17400` rain | 2.4 / 2.6 | 3.5 / 4.7 |
| `state_57000` forest, BG0 empty | 1.3 / 1.5 | 1.0 / 1.4 |
| `state_04500` ordinary | 0.63 / 0.90 | 0.66 / 1.02 |

The CPU draws the next frame while the RDP draws this one, so where the RDP
estimate stays above the CPU time the frame is still RDP-bound.

The rain is the case that stays above 12 ms: its first target covers the whole
screen with saturating 16/12, so every pixel goes through the 32-bit path and
the TLUT final pass (EVA+EVB = 28: no linear final pass can be exact, the
saturated values span more than the combiner's range).  Its cost is the BG
layers in 32 bits, the blend and two final passes over 76,800 pixels and
about 0.9 MB of TMEM loads.  Under the c32 calibration the sunbeam houses
would also stay above 12 ms.

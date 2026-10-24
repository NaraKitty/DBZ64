# 320x240 field view

On field maps the N64 shows a real 320x240 window onto the authored world: the
map is not stretched. Field maps include towns, the overworld, and the real-time
battles and bosses that run on field maps. The HUD and dialogue are re-anchored
toward the screen edges, and location banners run the full width. Maps smaller
than the view are continued past their edges (see "Margin fill"). The pause
menu and the file select fill the view too (see "Menus"), and so does the
mode-1 world map (flying over the overworld; see "Mode-1 world map"). Every
other screen (title, logos) shows the native 240x160 frame centred at (40,40)
with a black border.

The interface is `runtime/widescreen.h`. The LoG2 code is in
`games/log2/widescreen.c`, and the hook sites are in `games/log2/game.toml`
(section "320x240 view"). The host reference renderer is
`ppu_soft_render_view()`.

## Interface

```c
ws_frame_begin();      // every VBlank before drawing (runtime/io.c calls it)
ws_view_t v;
ws_get_view(&v);       // native view, or the field view with hooks
```

`ws_view_t` has the same layout as `gba_view_t` in
`platform/n64/render/gba_render.h`:

- `ox, oy` is where GBA screen pixel (0,0) lands in the framebuffer.
- `[x0,x1) x [y0,y1)` is the GBA-screen region to draw.
- In the field view, `ox = -OX` and `oy = -OY`. The region is `[OX, OX+320) x [OY, OY+240)`, with -80 <= OX, OY <= 0.

There are two hooks:

- **`bg_entry(bg, cx, cy, &e)`** takes a world tile cell. World pixel = the full 16-bit
  BGxHOFS + screen x. It returns 0 to mean "use the hardware map with its usual wrap".
  - It always returns 0 inside the native screen, so the native area is drawn
    from exactly the hardware state.
  - It also returns 0 for wrap layers.
- **`obj_pos(i, &x, &y)`** gives the full signed top-left of OAM entry i's bounding
  box. The sprite is drawn there once, with no 9-bit/8-bit wrap.

The culling widening is only active when all of these hold:
- `ws_enabled` is set.
- The scene is a field.
- The platform called `ws_get_view()` during the last frame.

A platform that still renders natively therefore gets the unmodified game.

## Scene detection (`detect_field`)

All of these must hold:

- DISPCNT is mode 0 and not forced blank, with OBJ on.
- The field present has run in the last 3 VBlanks: `View+0x34` changes.
- Map size is plausible.
- For every BG:
  - BGxCNT uses screen block 28+n, size 0.
  - The layer object at `0x0300107C+4n` has vtable `0x08023868` (field) or `0x08023C44` (wrap).
  - The object's BG byte equals n.

Individual BGs may be disabled.

## View rectangle

The camera of the shown frame is `View+0x2C/+0x30`. These are L and T as stored by
the last layer update at `0x080052B0`, so they match the scroll registers on screen. Map size is
`View+0x54/+0x58`.

```
x = L-40 clamped to [0, W-320]   (W < 320: centred, (W-320)/2)
y = T-40 clamped to [0, H-240]   (H < 240: centred)
x, y clamped so the native 240x160 stays inside;  OX = x-L, OY = y-T
```

Shake moves the whole view, as it does natively. Small maps (108 maps are
narrower than 320, 27 shorter than 240) are centred, and their margins are
filled from the map itself (next section). Gameplay is unchanged.

## Background margins

Field layer objects (vtable `0x08023868`) are laid out like this:

| Offset | Meaning |
|---|---|
| +4 | BG index |
| +8 | resource |
| +0x1C..0x1F | chunk index owned by each of the 4 slots |
| +0x28 + slot*0x800 | decoded 32x32-tile chunks |

The resource (factory `0x080056A1`) holds:

| Offset | Meaning |
|---|---|
| +4/+8 | parallax fx/fy |
| +0xC/+0x10 | origin (22.10) |
| +0x14/+0x15 | cols/rows of 256 px chunks |
| +0x18 | descriptors |

For a margin cell, `bg_entry`:

1. Maps the cell to its chunk.
2. Takes the entry from the loaded slot if the layer has that chunk. This keeps runtime map edits.
3. Otherwise decodes the ROM descriptor into an LRU cache of 32 chunks (64 KB). The decoder is Webfoot's bit-stream LZ, ported from the reference mod's `decode_field_chunk`.

Chunk lookups are cached per layer, 6 runs per frame, so a 32-tile run costs one lookup.

Cells outside the clip are margin cells (next section). The clip is:
- **Factor-1.0 axes:** the map rectangle in layer space, `[origin, origin+W)`.
- **Other axes:** the chunk grid.

Wrap layers (`0x08023C44`) use the hardware ring, which is their authored repeat.

## Margin fill

A margin cell past an edge of the clip takes the authored entry of a cell
inside, by the rule of that edge (`margin_src`):

| Rule | Source of the k-th cell past the edge (k = 0 next to it) |
|---|---|
| blank | the all-zero tile (backdrop or lower layers show) |
| mirror | the k-th cell inside, with the flip bit of that axis toggled |
| repeat P | the cell P-1-(k mod P) inside |
| zero | entry 0 while still inside the map, blank past it |

Mirroring is seamless at the edge by construction, and keeps whatever the
edge is made of (trees, rock, sand, water). Corner cells apply both axes.

A layer's chunk grid can end inside the map (480x328 map `0x084EB64C`: BG2's
grid stops at y 256). The game's loader (`0x0800532E`) fills such quadrants
with the all-zero chunk at `0x081D9CB8`, so the GBA shows entry 0 there: that
side gets the **zero** rule, and the layer does not vote on the map edge.

**Map edges** (factor-1.0 axes) are decided per map from the authored
entries of a strip along each edge, as deep as the margin the view can show
there plus one cell for shake (`decide_edge`, up to 64 positions along the
edge, all factor-1.0 layers whose clip ends at the map edge, together). On
an axis the view does not exceed (x when W >= 320, y when H >= 240) only
shake can show a margin: the strip is one cell deep and sampled at 16
positions.
- **blank** when the outermost cells, or the whole strip, are at least half
  dark: every layer's tile is transparent or black (`tile_dark`, from VRAM
  and the palette). Rooms keep a black border inside the map, so interiors
  stay as they were.
- **repeat P** when, for some period P from 2 to 16 cells, at least 90% of
  the strip equals the strip P cells further in, on every layer at once (a
  regular forest edge).
- **mirror** otherwise.

A decision reads up to 64x24 cells per layer and may decode chunks from
ROM, a few ms of N64 CPU, so `update_edges` takes one edge per frame when a
map comes up and again 30 frames later, in case the tiles were still
loading. A round trip to the pause menu keeps them (they are keyed by the
map, not by the field view). Whenever an edge changes, `ws_map_generation()`
moves so the N64 renderer drops cached margins.

**Parallax layers** (factor != 1.0) are mirrored past their chunk grid on
those axes, so a backdrop one chunk wide reaches the view edges.

The rules were checked by building the host harness with
`-DWS_TEST_NARROW`, which clips every factor-1.0 layer to the native screen
of the frame, and comparing the filled margins with the real map around
them (`D:\dbz-n64\runs\ws_narrow`, outside the repo).

## Sprites

### Exact coordinates

Every sprite emitter packs coordinates the same way:
- x: `lsls rd,rs,#23 ; lsrs rd,rd,#7`
- y: `lsls rd,rs,#24 ; lsrs rd,rd,#24`

`[[hook_before]]` at 34 x sites and 38 y sites records the full signed value of `rs`
(`ws_px_rN` / `ws_py_rN`). Three y sites pack y-8/-16/-32 as y+0xF8/0xF0/0xE0, so their
hooks use `_m256`.

The build loop (`0x08006AD8`) is bracketed by these hooks:

| PC | Event |
|---|---|
| `0x08006BCE` | shadow OAM cleared |
| `0x08006BFE` / `0x08006C04` | before / after each object's emit; `[sp]` = OAM write pointer |
| `0x08006AAE`, `0x080070B0` | shadow uploaded |

After each emit, the entries the object wrote are matched in order against the
values recorded during that emit by their low 9/8 bits. A repeated value is reused, for
rows of sprites that share one y.

The side table is published when the shadow is uploaded. At render time it is used only if the
entry's attr0/attr1 still equal what was emitted.

### Fallback

Entries with no recovered value use a window decode:
- x: the 9-bit value in `[OX-64, OX+448)`.
- y: the 8-bit value in `[OY-16, OY+240)`.

### Classification by class

The class comes from the object's vtable.

- **World:**
  - The isVisible (vt+0xC) is one of the render-cull functions below.
  - Any unknown class also counts as world, so it keeps its native place in the view.
- **Screen-space:**
  - These culls: `0x0800775A`, `0x08019050`, `0x08007CDE`, `0x08012A28`, and the
    ScreenSprite cull `0x080264A4`.
  - These vtables:

    | Vtable | Object |
    |---|---|
    | `0x0802403C` | dialogue window, a 4x64x64 affine cluster |
    | `0x0802408C` | top-left HUD strip |
    | `0x0802527C` | icon at (216,8) |
    | `0x08024D40` | location banner |
    | `0x08024EBC` | ScreenSprite |
    | `0x08025748` | boss HP bar: two 32x8 sprites at (160,y) and (192,y) |
    | `0x080250D4` | 8x8 sprite at (119,139) |
    | `0x08025DC8` | 16x32 sprite at (4,25) |
    | `0x08025E84` | 32x8 sprite at (32,4) |
    | `0x08025B2C` | cursor placed from a fixed position table |

    The last five use the always-visible cull `0x080264A8`. Their emitters write
    constant screen positions. Before they were added, the boss HP bar was
    classed as world and floated in the playfield at native (160,8)+(40,40).
    Not every always-visible class is screen-space: `0x080255A4` (emit
    `0x0801021C`) subtracts the camera and stays world.

### Placing screen-space objects

1. Objects the GBA would not show are hidden: their box lies entirely off the native screen.
2. The rest are grouped into clusters. A cluster is all entries of one object plus any boxes that touch, within 2 px, so a portrait and its text box move together.
   - HUD pieces (`0x0802408C`, `0x0802527C`, `0x08025748`, `0x08025DC8`,
     `0x08025E84`) never join by touch. A dialogue window at the top overlaps
     the HUD strip, and joining them moved the HUD 32 px right while the window
     opened and closed. The centred window now covers only part of the HUD,
     which it hides completely on the GBA.
3. Each cluster keeps its place in the free space. A cluster at fraction t of the native
   screen's free space (240 or 160 minus its size) goes to fraction t of the 320x240
   screen's free space, inside a `ws_hud_inset` (8 px) margin for CRT overscan.
   - What sits at an edge stays at that edge, and what is centred stays centred.
   - Anything moving between them moves smoothly. The dialogue box slides between
     bottom and top with R/L; the earlier rule (snap to a third) made it jump mid-slide.

The HUD lands in the top-left corner. Dialogue boxes and banners land at bottom-centre, or
top-centre when the game puts them at the top.

### Location banners

A location banner (`0x08024D40`) is a row of affine sprites, 7 of 32x16 and
one 16x16 at y 128, covering exactly the native width; the game scales them
in and out. When the shadow OAM is uploaded (`ws_hook_oam_upload`, only while
the view is widened), `extend_banner` adds copies of the rightmost sprite,
16 px wide, 40 px out on each side, so the banner spans the whole 320 px.
That sprite holds only the stripes: the copies are made only if every pixel
row of it is one colour. They take free shadow slots (the overflow guard
keeps 16 free), share the affine parameters so they scale with the rest, are
flagged in the side table so they are not hidden as off-screen, and are
cleared by the next build.

## Menus

The pause menu (status, items, options and journal pages) and the file
select share one layout (`menu_regs`, `menu_starfield`): mode 0 with BG1 and BG3 on, BG1-3
control `0x1D83`/`0x1E83`/`0x1F83` (BG0 `0x1C83` while pages crossfade), no
field running, and BG3 a starfield with no empty cell in its 32x32 ring. The
title screen and logos do not match (BG1 is off there).

The view is the native frame at (40,40) with the region widened to
`[-40,280) x [-40,200)` and `menu_bg_entry` as the BG hook (sprites keep
their native place):
- **BG3**, the starfield, uses the hardware ring. It scrolls through its whole
  ring on the GBA, so it is seamless and fills the view.
- **BG0-2** (frame and page) are transparent outside the native frame,
  except rules: a row is continued when, scanning at most 6 cells in from
  each native edge, the first tile whose every pixel row is one colour has
  the same rows on both sides, is not transparent, and both edge cells show
  something. That tile repeats to the view edge. This continues the orange
  tab line and nothing else; the page panel touches one edge only and is
  left alone.

The margins depend on the edge columns, so `ws_map_generation()` moves when
their entries change (page switches), and on entering and leaving the menu.

## Culling and despawn (`games/log2/game.toml`)

The camera rectangle is at `0x0300109C`: L, T, R=L+240, B=T+160.

`[[override_load]]` routes the load of each bound through a widening function:
- `ws_cull_left/right/top/bottom` for render culling.
- `ws_despawn_*` for the off-screen despawn tests. These are gated by `ws_widen_despawn`, default 1.

They return the bounds of the 320x240 view, X0 / X0+320 / Y0 / Y0+240, computed from the live camera.

| Kind | Functions | Loads |
|---|---|---|
| render cull (all four sides) | 080071FA, 08007A84, 0800FD3E, 08011896, 080136A6, 08013C8E, 0801647A | 28 |
| despawn | 080138D2, 0801406A, 08014286, 08015FF8, 080161B4, 080168D6, 0801A144, **08013C22** | 32 |

`0x08013C22` was not in the research spec. It is the per-direction off-screen test of
the directional projectiles (ki blasts). Without it, blasts vanished at the
native edge.

Two culls compare a screen position against a constant native-screen rectangle
in ROM rather than against the camera. `ws_scull_*` shifts each bound by the
view offset of the live camera (left/top by OX/OY, right/bottom by OX+80/OY+80):

| Site | Rectangle | Hooks |
|---|---|---|
| isVisible `0x0800ECF8` (debris particles, vt `08024014`) | `{-7,-7,240,160}` at `0x086ADDD8` | loads at 0800ED28/2E/36/3C |
| rect test `0x0802805C`, called by the beam emitter `0x08014894` for each segment | `{0,0,240,160}` at `0x086E3604` | `ws_hook_recttest` records r1; loads at 0802805E/64/6E/74 widen only when r1 = `0x086E3604` |

Without these, debris vanished and beam segments were not drawn outside the
native screen. Both rectangles were found by searching ROM for `{x,y,240,160}`
records referenced from code. A third, `{-7,-7,240,160}` at `0x086F7A14`, is
used only by `0x08016BD4`, a cull of the mode-1 map's objects.

### How the sites were verified

- **Constant propagation:** CFG constant propagation over every recompiled function finds
  84 loads of the camera rectangle. All 60 override PCs resolve to the right field
  (`D:\dbz-n64\scratch\widescreen\verify_toml.py`, outside the repo). The 24 loads that are not overridden are:
  - screen-position computations: 080136B0/B8, 08013CBC/C0, 08014874/78, 08015C30/34, 080164D6/DE, 0801E440/48
  - game logic the spec says to leave native: 08015AC2.., 0801D8E4..
  - the mode-1 map's point test (08012D06..)
- **Runtime read-watch:** the camera page was made slow-path, and reads were attributed to
  generated functions through `addr2line`. This found only these readers:
  - the layer, view and native camera code
  - the cull functions above
  - `0x08013C22`

An overflow guard sits at `0x08006BFA`. While the widening is active, an emit is skipped
when fewer than 16 shadow OAM entries are left. The shadow ends right before
another global. In testing, at most 15 of the 128 entries were ever used.

## Host validation

These options apply to `platform/host/main.c`, and also through `tools/run_host.sh NAME F E <opts>`:

| Option | Effect |
|---|---|
| `--ws` | writes 320x240 `frame_N.png` through `ppu_soft_render_view` |
| `--ws-native` | also writes `native_N.png` |
| `--ws-debug` | prints view stats and a BG seam check; give it twice for per-slot sprite classes and positions |
| `--ws-off` | sets `ws_enabled = 0` |
| `--ws-nodespawn` | keeps the native despawn bounds |
| `--ws-force-decode` | ignores loaded chunk slots |
| `--ws-decode-test` | decodes every chunk of every field resource and writes CRCs |

Results:

- **Native output:** with the hooks compiled in but no `--ws`, frames are pixel-identical to the build without hooks. That covered 30 intro frames and 923 gameplay frames (every 20 frames to frame 18460).
- **Native view through `ppu_soft_render_view`** (`--ws --ws-off`): it is exactly the native frame at (40,40) with a black border, in all 184 frames compared.
- **Decoder:** all 5249 chunk descriptors decode, and every one matches an independent Python decoder (`--ws-decode-test`).
- **Seam check:** the authored map equals the hardware map on every native-screen cell, in every field frame checked.
- **Forced decode:** forcing ROM decode instead of the loaded slots gives pixel-identical 320x240 frames over the 923-frame run.
- **Game logic:**
  - With render widening only (`--ws --ws-nodespawn`), the player position and
    the camera trajectory match the native game on every frame of a
    30,000-frame random-input run. Game RAM differs only in render state: the
    shadow OAM, the sprite sort lists, the per-object sprite-frame decode
    buffer at `0x0203B61C`/`0x030069E0`, and stale stack.
  - With despawn widening (the default), outcomes can change. In the same run
    a ki blast that the GBA would have despawned at the native edge flew on and
    broke the tutorial boulder, which the native run left standing. That
    triggered Gohan's "That was great!" dialogue, and the runs diverged from
    frame 21461 on.
  - A blast that leaves the native screen keeps flying through the margin, as intended.

### Review run

A fork-based input explorer (in `D:\dbz-n64\scratch\wsreview`, outside the
repo) reached 16 field maps. They included:
- the tutorial and its night version
- Pepper Town (768x1024, with a rain wrap layer)
- Goku's house rooms: 256x224, 240x272, 240x408 and 328x320 (narrow and short maps)
- the 1024x1024 and 768x512 outdoor maps
- the Frieza arena (240x256, with a wrap layer in the side margins)
- the pink cloud map
- the pause/status menu, the journal, the file select and the game-over flow

Results over these runs:
- **Seam check:** 0 bad cells in 5,274 field frames.
- **Forced decode:** identical over 469 frames.
- **Native view:** `--ws --ws-off` equals the native frame at (40,40) in 198 frames.
- **Native game:** without `--ws`, game RAM is identical frame by frame to the
  build before this change.
- **Transitions:** the field-to-menu switch happens on black frames in both directions.

## Limitations

- **Coverage.** Not reached:
  - battles with many enemies (only the Frieza boss fight)
  - parallax (fx != 1.0) layers. ROM has about 20 of them, including static ones
    (fx = 0), some only one chunk (256 px) wide. Past the chunk grid they are
    mirrored; that was checked only with the clip test, not on such a map.
  - the outdoor small maps players reported (the forest clearing, a cave, a
    cliff path with a sky backdrop, a desert canyon): the explorer runs reach
    only interiors among the small maps. The margin rules were checked on
    clipped large maps instead (see "Margin fill").
  - beam attacks that use the `0x08014894` emitter (the fix is verified only by code)
- **Sprite classes.** Sprite classes that never appeared in testing are classified statically:
  - Unknown classes count as world. A HUD element of an unknown class stays at its
    native place and is not anchored.
  - A world-anchored object of a screen-space class would be moved by up to 40 px.
  - The tables in `widescreen.c` are the place to fix either case.
- **Emitters without the pack idiom** fall back to the window decode:
  - In testing that was only the banner, the HUD strip x, and constants.
  - A world sprite taller than 16 px that sits partly above the view top could then be placed 256 px too low.
- **Despawn widening is a gameplay change by design.** Projectiles can hit targets in the margin.
  - `ws_widen_despawn = 0` restores the native behaviour.
  - The other camera tests that look like game logic stay native: `0x08015AC0`, `0x0801D8E2`, `0x08012D04`.
- **Scripted entrances.** NPCs that wait just off the native screen are visible in the margin.
  For example, Androids 17 and 18 stand at the top of the view in the Pepper Town cutscene
  before they enter.
- **N64 renderer cache.** `platform/n64/render` re-queries `bg_entry` for a cell only when the
  cell enters the window or its hardware map cell changes (see `render_bg.c`). Hook data
  can change without either:
  - a map swap while the field present keeps running
  - a change of the blank tile
  - a runtime edit of a loaded chunk that is outside the ring

  In these cases the N64 platform must call `gba_render_invalidate_bg()`. A map change
  (`View+0x50`) is the obvious trigger. `ppu_soft` queries every cell of every frame and
  is not affected.
- **Windows (WIN0/WIN1)** are never used by this game. `ppu_soft` extends a window to the
  margins only when it spans the whole native width or height.
- **The mode-1 world map**: see the limitations at the end of its section.

## Mode-1 world map

The screen where the player flies over the world (`games/log2/ws_mode1.c`;
`widescreen.c` calls into it when the frame is not a field map). DISPCNT mode 1:

| Layer | What | Object (`0x0300107C` + 4n) |
|---|---|---|
| BG2 | affine ground plane, a 512x512 wrapping VRAM map streamed from a 4096x3328 world | vt `0x08023E7C` |
| BG1 | sky panorama: a 64x32-cell map at obj+8; the game DMAs the 32 columns in view into the hardware map and sets HOFS to the scroll | vt `0x08023A00` |
| BG0 | a field layer: the minimap outline (top right) and the haze band over the horizon (rows 7..11), 240 px wide | vt `0x08023868` |
| OBJ | player (`0x08025F28`) and shadow (`0x080250D4`), minimap markers (`0x08024064`, `0x08025C08`), ground landmarks (`0x08025E5C`, `0x08025F90`, `0x08025FB8`), the location banner | |

**Per-line parameters.** The HBlank handler `0x08011F88` copies entry
VCOUNT+1 of the 16-byte table at `[View+0x6C]` ({PA|PB, PC|PD, X, Y}) to the BG2
registers. `0x08012520` fills entries 56..159 every frame; entries 0..55 hold a
filler that the opaque sky hides. For line L, k = L-56:

```
A = [0x086D82DC + 4k]   B = [0x086D855C + 4k]     (ROM tables, 160 entries: lines 56..215;
                                                   A = 425984/(3k+104), B = 1024k/104, rounded down)
pa = (m0*A) >> 14       pc = (m1*A) >> 14
x  = ((((U*B) >> 10) + ip) * A >> 14) - 120*pa
y  = ((((V*B) >> 10) + lr) * A >> 14) - 120*pc
```

m0, m1, U, V, ip, lr are loop invariants computed from the camera. A
`[[hook_before]]` at `0x08012612` (before the loop) records them from r12, r14
and sp+0x14..0x20; it only reads. The view computes lines 160..199 with the
same formula and the tables' next entries, with 64-bit products: the game's
32-bit products wrap, which for its own lines only moves the reference point by
1024 px (the same texel of the wrapping map), but U*B and V*B start to wrap
below the native screen. The host check (`--ws-debug`) compares the model
with the hardware snapshots of lines 56..159 every shot: 0 lines differ
(pa/pc exactly, x/y modulo 1024 px) in every frame checked.

**The view.** The native screen at (40,40), region (-40,-40)-(280,200):

- BG2: lines 0..159 from the hardware snapshots; lines 160..199 from the model;
  lines above the screen (sky) repeat line 0's filler. Each line is affine, so
  x outside 0..239 is exact.
- BG1: outside the native screen the panorama (world cell cx shows panorama
  column cx & 63; rows above the sky repeat row 0, the top of the sky).
- BG0: rows that are full-width bands natively (the haze) extend their edge
  cells; the minimap (its non-band cells) moves to the top-right corner with
  `anchor()`'s rule, in whole cells, and the old cells become blank. Here
  `bg_entry` also answers inside the native screen. The analysis is redone when
  the layer's map entries change; when the minimap's entries change the map
  generation is bumped (the N64 renderer re-queries all cells).
- Sprites: the field rules (`ws_m1_sprites()`: world objects keep their place,
  screen-space objects such as the banner are anchored), except the player and
  its shadow, which stay where the ground puts them, and the minimap markers,
  which move with the minimap.

**What the game streams and culls** (while the view is shown; render state only):

- `0x0801204E` copies the ground map into the VRAM ring for the camera rect
  View+0x0C..0x18, which `0x08005DD4` makes the bounding box of the native
  screen's ground. Its four loads (`0x0801205A`, `68`, `72`, `7C`) get the rect
  widened to the wide view's far corners (points 256 px ahead, 165 px to the
  side), kept inside the decoded 3x3 blocks around the camera.
- The landmarks' isVisible `0x08012CEE`: the camera-rect test (loads at
  `0x08012D06`, `0C`, `14`, `1A`) uses the same widened rect, and the projected
  position tests (constants 240 and 160) get the position moved into the
  native range by `[[override_reg]]` (a new recompiler hook: before a PC,
  rN = func(registers)) when it lies in the wide view.
- `0x08016BD4` (vt `0x08025ADC`): its ROM rect {-7,-7,240,160} grows 40 px on
  every side.

**Verification.**

- Native runs (view hooks inactive) are identical to the build without them:
  RAM, frames, audio and the video state (I/O, VRAM, OAM, line registers) at
  every checkpoint of the world-map route, and RAM, frames and audio over the
  100,000-frame field route.
- The N64 renderer equals `ppu_soft_render_view` with the same hooks in all
  compared world-map frames (11, including a landmark in the right margin),
  native area and margins.

**Reaching it.** No story route was found; the game's hidden debug menu warps
there. Pokes (host `--poke`, N64 test script `poke`): frame 1995
`0203DF44=2` (title menu choice 2 = debug menu), start at 2000, frame 2200
`020344A0=2/4` and `020344AC=B0/4` (MAP TEST, record 176 = zone 6 area 0x63,
"THE WORLD"), A at 2210. The map records are the 0x38-byte table at ROM
`0x08409368`.

**Limitations.**

- Flying diagonally near a 256 px block edge, the outermost far corners of the
  wide view can show stale ground for a few frames: the game decodes only the
  3x3 blocks around the camera.
- Landmarks were seen only from a debug warp (no story flags set).
- Sprites of classes not seen in testing follow the field rules.

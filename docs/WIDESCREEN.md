# 320x240 field view

On field maps the N64 shows a real 320x240 window onto the authored world: the
map is not stretched. Field maps include towns, the overworld, and the real-time
battles and bosses that run on field maps. The HUD and dialogue are re-anchored
toward the screen edges. Every other screen (title, menus, status, logos and
the mode-1 map) shows the native 240x160 frame centred at (40,40) with a black
border.

The interface is `runtime/widescreen.h`. The LoG2 code is in
`games/log2/widescreen.c`, and the hook sites are in `games/log2/game.toml`
(section "320x240 view"). The host reference renderer is
`ppu_soft_render_view()`. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2. Buu's Fury uses the same design with its own
addresses (`games/buu/`). The last section, "Buu's Fury", lists what
differs; the sections before it describe LoG2.

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

Shake moves the whole view, as it does natively. On small maps (108 maps are
narrower than 320, 27 shorter than 240) the map is centred and the backdrop
shows around it. Gameplay is unchanged.

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

Cells outside the clip get an all-zero (transparent) tile. The clip is:
- **Factor-1.0 axes:** the map rectangle in layer space, `[origin, origin+W)`.
- **Other axes:** the chunk grid.

Wrap layers (`0x08023C44`) use the hardware ring, which is their authored repeat.

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
3. Each cluster snaps by the centre of its box: to the left/centre/right third and the
   top/middle/bottom third of the 320x240 screen, with a `ws_hud_inset` (8 px) margin for
   CRT overscan.

The HUD lands in the top-left corner. Dialogue boxes and banners land at bottom-centre, or
top-centre when the game puts them at the top.

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
    (fx = 0), some only one chunk (256 px) wide. Outside the chunk grid their
    margins are blank, so a static backdrop would not reach the view edges.
  - the mode-1 world map
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
- **The mode-1 map** (affine BG2 with HBlank matrices) stays native.

## Buu's Fury (`games/buu`)

Buu's Fury runs the same engine, so it uses the same design.
`games/buu/widescreen.c` is LoG2's code with the Buu's Fury addresses, plus
four additions:
- parallax-aware view placement
- one more layer class
- paused fields stay in the field view
- y-coordinate corrections for emitters that add signed offsets as unsigned
  bytes

The hook section of `games/buu/game.toml` ("320x240 view") is generated. A
script outside the repository (`mktoml.py` in `D:\dbz-n64\scratch\buu\wsview`)
checks every PC by disassembly:
- Each override PC must be the `ldr` of the camera-rect field, or ROM
  screen-rect field, that its function name says.
- Each pack site must be an `lsls #23` or `lsls #24` with the matching `lsrs`.

### Game facts

| | LoG2 | Buu's Fury |
|---|---|---|
| camera / view object | `0x03001090` | `0x030019A4` |
| camera rect L, T, R, B | `+0x0C..+0x18` | `+0x18..+0x24` (`0x030019BC..`) |
| shown L, T (field present) | `+0x2C/+0x30` (`0x080052B0`) | `+0x38/+0x3C` (`0x080092BC`) |
| presents counter | `+0x34` | `+0x40` (incremented at `0x080092E6`) |
| map, W, H | `+0x50`, `+0x54`, `+0x58` | `+0x8C`, `+0x94`, `+0x98` |
| layer set | `0x0300107C` | `0x03001990` (`[0x080716C4]`) |
| field layer | vtable `0x08023868` | vtable `0x08049410`, 0x204C bytes, ctor `0x08007B78` |
| owner bytes / chunks | `+0x1C` / `+0x28 + slot*0x800` | `+0x20` / `+0x4C + slot*0x800` |
| fx, fy / origin | resource `+4/+8` / `+0xC/+0x10` | layer `+0x3C/+0x40` / `+0x44/+0x48` |
| wrap layer | `0x08023C44` | `0x080498B0` (12 bytes) |
| empty layer | — | `0x08049548` (8 bytes) |
| resource factory | `0x080056A1` | `0x08008087` |
| sprite build | `0x08006AD8` | `0x080115B0` |
| clear / guard / emit begin / end | `0x08006BCE` / `BFA` / `BFE` / `C04` | `0x080116A6` / `6D2` / `6D6` / `6DC` |
| shadow upload | `0x08006AAE`, `0x080070B0` (DMA3) | `0x08011586`, `0x080117C6` (memcpy32 via `0x0803DA62`) |
| shadow OAM | `0x030022BC` | `0x03003498` (the sound state follows at `+0x400`) |
| return-0 stub (guard) | `0x080264AC` | `0x0804FC44` |

Notes on the table:
- **Origin.** The layer's origin is the resource origin plus the offset set
  by the set-origin method `0x08007FE8`, so `widescreen.c` reads the live layer
  fields.
- **Scroll.** The layer update writes the full 16-bit
  `(f*L + o + shake) >> 10` to BGxHOFS/VOFS. The forced update is
  `0x08007E84`, the incremental one `0x08007F2E`.
- **Ring.** The ring streamer `0x08007D98` copies chunk rows unchanged
  (memcpy16). It keeps a 256x256 window at the scroll minus (8, 48), the bias
  table at `0x08070800`.
- **Checked at runtime.** In every dump of a field frame:
  - the shown L/T equal BG0's scroll registers
  - each layer object carries its BG byte
  - the presents counter runs only on field maps

### Scene detection

The rule is LoG2's, with two changes.

**Empty layer.** The layer class `0x08049548` is also accepted. It is 8 bytes:
vtable and BG byte. Its forced update `0x08007A28` clears the layer's screen
block once. Its incremental update `0x08007A3E` is `bx lr`. King Yemma's office
and the West City maps use it for BG0 (and the office for BG2). Like a wrap
layer, it is drawn from the hardware map.

**Paused fields.** While B is held, the game is in its targeting mode: a
ring-shaped reticle appears and the player aims with the D-pad (scene
`0x0804D260`). The game then stops running the field present but keeps the
field on screen; in testing this lasted 40 to about 110 frames, on Snake Way
and in West City. The sprite build keeps running. LoG2's rule (a present in
the last 3 VBlanks) would drop to the centred native frame for those frames
and jump back afterwards. A frame without a present therefore keeps the
field view when all of these hold:
- the previous frame had the field view
- the layer set holds the same four objects as at the last present
- all eight scroll registers are unchanged

The camera cannot have moved, so the view stays the same.

**Not fields.** These stay native:
- menus (DISPCNT `0x0400`/`0x0500`, no OBJ)
- the scouter map screen (`0x1D40`/`0x1940`, other screen blocks)
- logos and the title screen
- the Mode-4 error screen
- the affine (HBlank Mode-7) overworld, which is not mode 0

The layer class `0x08049604` is a two-chunk strip whose HOFS follows camera
`+0xC`. It was never seen on a field map and is not accepted.

In the cases checked, the switch between the map screen and the field
happens on a black frame (a fade).

### View rectangle: parallax layers

LoG2's rule puts the view 40 px around the camera and clamps it to the map.
On Snake Way (992x416) that is not enough:
- **The cloud layers.** BG2 has f = 0.4/0.4 with origin (128,124). BG3 has
  0.2/0.4 with origin (192,124).
- **Where they are authored.** Their content starts exactly at the origin,
  which is all the GBA ever shows at camera L = 0.
- **What went wrong.** At L = 32 the view starts 32 px left of the camera, but
  the clouds' scroll moved only 6 px (0.2 * 32). The view's left edge in BG3 is
  166, which leaves 26 px of never-drawn backdrop: a black strip.

Fix: each field layer the frame shows (`ranges[]`, filled by `detect_field`)
contributes, per axis, the layer pixels the GBA can show:

```
[lo, hi) = [scroll(0), scroll(Cmax) + 240)
scroll(c) = (f*c + o) >> 10
Cmax = W - 240        (y axis: H - 160, and + 160 instead of + 240)
```

For camera c, the view origin must then satisfy:

```
X0 >= lo - scroll(c) + c
X0 + 320 <= hi - scroll(c) + c
```

`view_axis()` builds the interval that keeps the native screen inside the view
and intersects it with the map clamp. It then intersects it with each layer's
constraint, in BG order. A constraint is skipped when it would empty the
interval: static backdrops (f = 0), and maps too small for that layer. LoG2's
target (the camera minus 40, centred on small maps) is then clamped into the
result.

- **Layers with f = 1.0** give exactly the map clamp. Maps without parallax
  get LoG2's rectangle.
- **The widened culls and despawns** use the same `view_rect()`, so culling and
  drawing agree.
- **On Snake Way**, X0 >= 0.8 L for L < 200, and Y0 >= 0.6 T.
  - Near the map's left and top ends, the native screen sits up to 34 px off
    centre, as it does at any map edge.
  - Every margin pixel is one the GBA shows at some camera position.

### Sprites

**Pack sites.** There are 96 hooks: 51 x and 45 y, in 38 emitters. They come
from the research scan of `lsls #23 … lsrs #7` and `lsls #24 … lsrs #24`
pairs, rescanned with a 12-instruction window. The rescan found one more y
site, `0x0800F314`, the actor's auxiliary piece.

**Constant offsets (`_m256`).** Six y sites pack y-32, y-8, y-12, y-16 or y-3
as y + 0xE0/0xF8/0xF4/0xF0/0xFD:
- `0x0800B3DE` (dialogue window)
- `0x0800C3BC`, `0x0800F104`, `0x080179BC`, `0x08027182`, `0x080302F8`

Without the correction, the dialogue window got y = 344 instead of 88.

**Signed offsets loaded unsigned.** The main actor emitter `0x0800EF94` adds a
meta-sprite piece's y offset with `ldrb`, while x uses `ldrsb`.
- **Natively** -24 stored as 0xE8 works, because attr0 keeps only 8 bits.
- **The full value** is 256 too large. Without a correction, the actors'
  heads and bodies landed 256 px below their feet and left the view.

Game-specific hooks add the sign back:

| Hook | Sites | Value recorded |
|---|---|---|
| `ws_py_buu_piece` | `0x0800F242`, `0x0800F296` | r3, signed byte `[r0+1]`, signed halfword `[r2+0x16]` |
| `ws_px_buu_piece` | `0x0800F21A`, `0x0800F270` | r7, signed halfword `[r2+0x14]` |
| `ws_py_buu_shadow` | `0x0800F150` | r1 + signed r2 |
| `ws_py_buu_aux` | `0x0800F314` | r2, signed byte `[[actor+0x58]+0xD]` |
| `ws_py_buu_r4_b5` | `0x0802244A` | r4 + signed r5 |
| `ws_py_buu_r6_b`, `ws_py_buu_r4_b` | `0x08038984`, `0x080389D8`, `0x08038A30` | y = r5 + byte |

With these, every world sprite entry seen in testing got its exact x and y:
4,633 entries of 10 world classes. Only the screen-space HUD and banner
emitters have no pack idiom.

**Classes.**
- **World:** the render culls below. Any unknown class also counts as world.
- **Screen-space:** these classes.

  | Vtable | Object |
  |---|---|
  | `0x08049F60` | dialogue window and portrait (always visible, emit `0x0800B374`) |
  | `0x08049FEC` | HUD: icon and health / ki bars at (8,3) (emit `0x0800BCC8`); slides in from y = -6 |
  | `0x0804B3D8` | chapter title card |
  | `0x0804B528` | location banner, 8 sprites of 32x32 across y = 128 (cull `0x0800BF94`) |
  | `0x0804B500` | HUD bars at constant x 8/40/56 (emit `0x0800BE4E` → `0x0800B99C`) |
  | `0x0804CE28` | boss HP bar: two 32x8 sprites at (160,y) and (192,y), constant attr1 (emit `0x08019658`) |
  | ScreenSprite cull `0x0804FBCC` | |

- **HUD pieces that never cluster by touch:** `0x08049FEC`, `0x0804B500` and
  `0x0804CE28`.
- **Not seen in testing:** `0x0804B500` and `0x0804CE28`. They come from a
  static scan of always-visible classes (isVisible `0x0804FC40`/`0x0804FC3C`)
  whose emitters write a constant screen x.
- **Stay world:** always-visible classes whose emitters read the camera. One
  of them is the targeting reticle (vt `0x0804E41C`, emit `0x080223B2`).

### Culling and despawn

| Kind | Functions | Loads |
|---|---|---|
| render cull (`ws_cull_*`, all four sides) | 08019BF4, 0800CF42, 0800DA3E, 0802A904, 0801003A (3D-map anchor point, main box, auxiliary piece), 0800AC64, 0800CBA8, 0800D538, 0801809E, 08036B9C | 48 |
| despawn (`ws_despawn_*`) | 08019B7C (directional ki blasts, LoG2 08013C22), 0802C7EA, 08035A80, 0801C9C4, 08027562, 0802776E, 0802D940 | 28 |
| constant screen rect (`ws_scull_*`) | 11 culls (below) | 44 |
| rect test `0x080532C0` (`ws_rect_*`, LoG2 0802805C) | widened only when r1 is one of the beam emitters' screen rects, `0x0872505C` or `0x0872506C` | 4 + hook |
| same rect test (`ws_buu_camrect_*`, despawn) | widened only when r0 is the camera rect: the beam update `0x0802084C` (calls at `08020A46`, `08020B44`) removes a beam a few frames after its head leaves the camera | 4 |

The 11 constant screen-rect culls:
- `{-7,-7,240,160}`:
  - `0x08072540`: debris, `0800C4DA`
  - `0x086B813C`: `080163E6`
  - `0x0874D02C`: `08026F2A`
- `{0,0,240,160}`:
  - `0x087251C8`: `08021762`
  - `0x0874EAB4`, `0x0874EAEC`, `0x087502AC`, `0x08759510`, `0x0875F81C`,
    `0x0876CD28`, `0x0876E0CC`

**Left native: the targeting mode.** While B is held, the scene `0x0804D260`
(built by `0x080220D8`) lets the player aim. It uses three camera-rect
readers:
- `0x08012846` lists the targets inside the camera (at most 16).
- `0x0800FAFA` is the objects' answer to message 0x19, "is my box on screen?".
- `0x080224AE` clamps the aiming reticle to the camera rect. It reads the rect
  with an `ldm`, which `[[override_load]]` cannot reach.

All three stay native, so targets in the margin cannot be picked.
`0x080379C8` (vt+0x14) never ran and is left native as well.

The L/T-only readers compute screen positions. The cull functions' own
screen-position loads (`08019C22/26`, `0802A90E/16`, `08036BBC/C4`) are not
overridden.

**Seen working.** A ki blast fired downward is drawn in the bottom margin.
Its segments below y = 160 pass the widened rect test, which never emits
them natively. The beam keeps growing until it leaves the view instead of
the native screen.

**Runtime read-watch.** A scratch host build makes the camera's IWRAM page
slow-path and resolves every reader of `0x030019BC..0x030019C8` with
`addr2line`. It ran over the explorer path, `fuzz_field3` and `fuzz_all2`,
and found only these readers:
- the layer updates, the field present and set-map
- the culls `0800CF42`, `0801003A`, `08019BF4`, `08019B7C` and the rect test
- the L/T screen-position readers `08020CC8`, `080211A8`, `080223B2`
- the targeting mode's `08012846` and `080224AE`

### Validation (host)

The runs used four scripts:
- the bring-up's gameplay script (`D:\dbz-n64\buu\runs\keys_gameplay.txt`,
  10,200 frames)
- the review's random-input runs:
  - `fuzz_all1` (60,000 frames: Snake Way, the snake head, King Yemma's
    palace and office)
  - `fuzz_field3` and `fuzz_all2` (200,000 frames each)
- an explorer path, 100,200 frames:
  - It starts with `fuzz_all1` and continues with a novelty-search input
    explorer (`explore*.py` in the scratch directory) that runs the host build.
  - It meets King Yemma, goes through the door behind his desk to the plane,
    and reaches West City (two town maps, 616x424 and 560x424).

In total, 8 gameplay maps and 9 cutscene maps were shown in the field view.

Results:
- **Native output.** With the hooks compiled in, without `--ws`, output is
  identical to the build without hooks. That covers 1,377 files: 1,178 PNGs,
  190 RAM dumps, WAVs and saves.
- **Native view.** `--ws --ws-off` gives exactly the native frame at (40,40)
  in 778 frames.
- **Seam check.** 0 bad cells in 1,437 field frames (2,983,290 cells).
- **Forced decode.** Identical frames over 778 frames.
- **Decoder.** All 3,819 chunk descriptors of the 1,439 resources decode, and
  their CRCs match the independent Python decoder.
- **Game logic.**
  - `--ws --ws-nodespawn`: the map, camera, player position and scene pointer
    (every 10th frame) are identical to the native game. That holds over the
    gameplay script, `fuzz_all1` and the explorer path.
    - RAM differs only in render state: the shadow OAM, the actors'
      visibility flags `+0x60/+0x64` and screen positions, the sprite-frame
      buffers of actors visible only in the margin, a few IWRAM words at
      `0x03001E54..0x03001E7B`, and the stack.
  - `--ws` (with despawn widening): the map, camera and player position
    are identical too, including over `fuzz_field3` (200,000 frames). The
    only differences are heap addresses. For a while, the current scene
    object `[0x03000F90]` is allocated elsewhere, presumably because an
    object that lived longer under a widened despawn test still held its
    memory.
- **Sprites.** At most 33 of the 128 OAM entries were used, and the overflow
  guard never fired.

### N64

The N64 build always asks for the view, which turns the widening on.

**ROM cache.** The RDP renderer's buffers leave about 0.9 MB of heap. Next to
Buu's 3.3 MB of code, the 2 MB ROM cache of `platform/n64/main.c` does not fit:
"cannot allocate the ROM cache". The tests used a scratch copy of the sources
with a 512 KB cache.

**Results.** In ares, every 320x240 screenshot equals the host `--ws` frame
at 5-bit precision. Each run must start with a blank cartridge SRAM: ares
keeps it next to the test ROM (`dbz_test.ram` in the build directory), so a
run that follows one that saved starts from that save, while the host runs
start without one. The explorer path writes a game-progress save. The
screenshots:
- 13 of the gameplay script: intro scroll, dialogue, Snake Way with HUD, menu
- 16 of `fuzz_all1`: snake head, King Yemma's palace and office, beam, menus
- 19 of the explorer path: the room behind King Yemma's office, the plane,
  West City, and two paused-field frames (targeting mode on Snake Way and in
  West City). This run started from the save an earlier run left (the pixels
  match anyway) and used the code before the beam's camera test; the host
  frames at these 19 shots are the same with and without that test.

The test builds render with the RDP renderer. On the explorer path, with the
512 KB ROM cache, the game took 7 ms per frame in ares (median of the
120-frame averages; the highest was 13 ms).

### Limitations

- **Coverage.** Fights against enemies and the Mode-7 overworld were not
  reached. West City is the furthest point.
  - The boss HP bar and the second HUD class are classified statically.
  - Of the 11 constant screen-rect culls, only `0x080163E6` ran (one effect
    sprite). The others were checked by disassembly only.
  - 3D maps (`View+0x90` set) gate the anchor-point test of `0x0801003A`. No
    tested map had it.
- **Parallax.** A layer whose native range cannot hold the view keeps blank
  margins, as in LoG2: a static backdrop (f = 0), or a narrow map.
- **Unsigned offset corrections** assume that piece offsets of 128 or more are
  negative. A piece really 128+ px below its anchor would be misplaced by
  256. No such piece appeared.
- **Targeting mode.** While B is held, the reticle stops at the native
  screen's edge, and targets in the margin cannot be picked (see above).
- **Despawn widening is a gameplay change by design**, as in LoG2.
- **N64 memory.** The RDP build does not fit until the ROM cache or the
  renderer's memory shrinks. That belongs to the RDP bring-up for Buu's Fury.

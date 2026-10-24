# Dragon Ball Z on the Nintendo 64

**Game Boy Advance classics, recompiled to run natively on N64 hardware.**

![Platform](https://img.shields.io/badge/platform-Nintendo%2064-red)
![Also runs on](https://img.shields.io/badge/runs%20on-Analogue%203D-black)
![Resolution](https://img.shields.io/badge/view-320%C3%97240-blue)
![Target](https://img.shields.io/badge/target-60%20FPS-brightgreen)
![License](https://img.shields.io/badge/license-PolyForm%20Noncommercial-lightgrey)

| Game | Status |
|---|---|
| **Dragon Ball Z: The Legacy of Goku II** (USA) | Playable. Runs on real hardware (tested on the Analogue 3D). |
| **Dragon Ball Z: Buu's Fury** (USA) | In progress: plays at full speed on N64 (in ares) with the RDP renderer, with the 320×240 view on field maps. Not yet tested on hardware. |

This is not an emulator running on the N64. Every ARM and Thumb instruction
of the original cartridge (2,282 functions, 67,500 instructions for *Legacy of
Goku II*) is translated ahead of time into C. That C is compiled for the N64's
VR4300 CPU and linked against a runtime that stands in for the GBA hardware:
graphics, sound, DMA, timers, interrupts and saves. The result is a normal
N64 ROM that you can play on a console, on an Analogue 3D, or in an
emulator.

## Highlights

- **A true 320×240 view.** Field maps are not stretched. You see more of the
  world: the extra margins are drawn from the game's own map data. Enemies,
  projectiles and NPCs live in the wider view because the game's culling is
  widened at every check. The HUD and dialogue boxes are moved out to the
  screen edges, and location banners run the full width. Rooms smaller than
  the screen are continued past their edges with their own trees, rock and
  sand (interiors keep their black surround). The pause menu and file select
  fill the screen with their starfield, and the Mode-7 world map you fly over
  is drawn at 320×240 too. Title and logo screens stay pixel-exact and
  centred.
- **Hardware-accelerated graphics.** The GBA's tile and sprite engine is
  rebuilt on the N64's RDP. It covers background layers, sprites, flips,
  alpha blending, brightness effects and mosaic; where sprites overlap in
  ways one-at-a-time drawing gets wrong, their pixels are composited on the
  CPU first. Its output is pixel-identical to a
  reference software renderer in every frame tested. Translucent effects
  (dialogue boxes, sunbeams, rain) only take the slow blending route in the
  screen cells where they can actually show.
- **The real soundtrack.** The game's own music engine and sound effects run
  as recompiled code. Its hottest mixing loops are replaced by native C that
  produces byte-identical audio.
- **Saves.** The cartridge's EEPROM is emulated and stored in the N64's
  cartridge SRAM, with a checksum and a backup copy.
- **Built for real consoles.**
  - Guest memory is mapped through the VR4300's TLB, so a guest memory access
    is a single load or store.
  - The game is paced by the audio chip's clock and never waits for video:
    if drawing runs long, frames are dropped on screen while game speed,
    music and sound effects stay steady (at 60 Hz and at 50 Hz).
  - At boot, a self-test checks the CPU features the port relies on and names
    any that misbehave instead of showing a black screen.
- **Verified against a reference.**
  - The PC build of the same recompiled code was compared frame by frame
    with the mGBA emulator, covering pictures, audio and RAM.
  - The N64 build matches the PC build pixel for pixel.

## Controls

| GBA | N64 |
|---|---|
| D-pad | D-pad or analog stick |
| A, B | A, B |
| L | L or Z |
| R | R |
| Start | Start |
| Select | C-Down |

C-Up shows a performance overlay with fps, frames drawn, CPU time and
cartridge reads.

## Playing it

**You need your own copy of the game.** No ROM, BIOS or game data is included
in this repository. The build reads your cartridge dump and checks its SHA-1:

| Game | Dump | SHA-1 |
|---|---|---|
| The Legacy of Goku II | USA, `ALFE` | `18e0715dec419f3501c301511530d2edcd590f8b` |
| Buu's Fury | USA, `BG3E` | `f1c4b07554d2a3b1ad2f325307051e775ce68087` |

The ROM you build contains your dump, so don't share it.

**Hardware:**
- An N64 with an Expansion Pak (8 MB), or an Analogue 3D.
- A flash cart such as SummerCart64 or EverDrive 64.

**Analogue 3D:**
- The built-in Expansion Pak can be switched off per game. If the ROM says
  it is missing, turn it on in the game's settings in the Library.
- SummerCart64 is known to work well. EverDrive carts need current cart
  firmware.

**EverDrive 64:**
- The ROM header requests SRAM 256 Kbit saves. If your cart OS ignores
  that, set the save type to SRAM 32K by hand.
- Saves are written to the SD card when you reset back to the EverDrive menu,
  so do that before powering off.

**Emulators:** ares is recommended, as it is accurate enough for the
TLB-based memory model. Enable the Expansion Pak.

## Building

You need:
- WSL (Ubuntu) with the [libdragon](https://github.com/DragonMinded/libdragon)
  toolchain (`preview` branch) installed at `/opt/libdragon`.
- Python 3 with `capstone`.

The build runs through a space-free path to the repository:

```bash
ln -s "/mnt/c/path/to/this/repo" ~/dbzn64
```

1. **Find the game's code.** The ROM is only read:

   ```bash
   python3 tools/analyze.py baserom.gba games/log2/game.toml gen/analysis
   ```

2. **Translate it to C.** The output is derived from your ROM, so keep it
   local:

   ```bash
   python3 tools/recomp.py baserom.gba gen/analysis/functions.json gen/c 24 --game log2
   ```

3. **Build the N64 ROM** from a build directory outside the repository.
   `dbz.z64` is written to the current directory:

   ```bash
   mkdir -p ../n64build && cd ../n64build
   make -f ~/dbzn64/platform/n64/Makefile GAME=log2 BUILD_DIR=$PWD \
        GEN=$OLDPWD/gen/c BASEROM=$OLDPWD/baserom.gba
   ```

   The ROM contains your GBA dump, so don't share it, and keep it out of
   synced folders. From Git Bash on Windows, `tools/build_release.sh OUT.z64`
   runs the same build through WSL. It also removes any leftover test
   script, which would otherwise boot the ROM in test mode. The RDP renderer
   is the default; `USE_RDP=0` builds the CPU reference renderer.

A PC test build (`GAME=log2 platform/host/build.sh GEN OUT`) runs the same
recompiled code headless. It writes screenshots, audio and saves for
validation; it is the test bench every change is checked on.
`docs/ARCHITECTURE.md` explains how everything fits together, and
`docs/WIDESCREEN.md` covers the 320×240 view.

### Games

The framework is shared by Webfoot's GBA *Dragon Ball Z* games. Everything
specific to one game lives in `games/<game>/`: `game.toml` (code discovery),
`game.h` (what the runtime needs: IRQ dispatcher, save magic, ROM identity,
N64 title) and the game's own C sources. `GAME=` (default `log2`) selects the
game for the build scripts and the N64 Makefile; `tools/recomp.py` takes
`--game`.

| `GAME` | Game | Dump (SHA-1) | Status |
|---|---|---|---|
| `log2` | The Legacy of Goku II (USA, `ALFE`) | `18e0715dec419f3501c301511530d2edcd590f8b` | complete |
| `buu` | Buu's Fury (USA, `BG3E`) | `f1c4b07554d2a3b1ad2f325307051e775ce68087` | in progress: plays on the host build and on N64 with the RDP renderer at full speed; 320×240 view on field maps, other screens native 240×160 |

For Buu's Fury use `games/buu/game.toml`, `--game buu` and `GAME=buu` in the
steps above.

## How it works

```
cartridge dump ──analyze.py──▶ functions.json ──recomp.py──▶ C (one function per guest function)
                                                                   │
runtime/ (GBA hardware model)  +  platform/n64/ (TLB memory, RDP renderer,
                                                  audio, saves, input)
                                                                   │
                                                                   ▼
                                                             N64 ROM (.z64)
```

**Recompiler:**
- Guest registers become C locals.
- Compares fuse with their branches.
- Literal pools fold into constants.
- Interprocedural register summaries let a call save and reload only the
  registers it needs.
- Indirect jumps go through a dispatcher.

**Runtime:**
- Guest time only advances when the game waits for the next frame.
- Interrupts, sound DMA and VBlank are delivered at exactly the points the
  game expects them.

**Renderer:**
- It caches background tiles in texture memory and redraws only what
  changed.
- It reads guest VRAM directly and tracks writes to it through the TLB.

## Credits

- **mstan**, for the PC recompilations of these games:
  [DragonBallZLegacyofGokuIIRecomp](https://github.com/mstan/DragonBallZLegacyofGokuIIRecomp),
  [DragonBallZBuusFuryRecomp](https://github.com/mstan/DragonBallZBuusFuryRecomp)
  and [gbarecomp](https://github.com/mstan/gbarecomp). Their function seeds,
  hardware findings and widescreen technique made this port much faster to
  build.
- [libdragon](https://github.com/DragonMinded/libdragon), the open-source N64
  SDK.
- [ares](https://ares-emu.net/) and [mGBA](https://mgba.io/), used for
  testing and as a reference.
- GBATEK, the GBA hardware reference.

## License and legal

The source code in this repository is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE). Parts of it are based on
mstan's recomp, which uses the same license.

This is an unofficial, non-commercial fan and preservation project. It is not
affiliated with or endorsed by Nintendo, Bandai Namco, Atari, Webfoot
Technologies, Bird Studio/Shueisha, Toei Animation or any other rights
holder. *Dragon Ball Z* and all related names are trademarks of their
respective owners. No copyrighted game data is distributed.

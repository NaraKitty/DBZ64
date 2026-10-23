# Dragon Ball Z: The Legacy of Goku II — N64 port

This is a static recompilation of the Game Boy Advance game *Dragon Ball Z: The
Legacy of Goku II* (USA). It runs natively on the Nintendo 64.

The game's ARM/Thumb machine code is translated to C ahead of time. That C is
compiled for the N64's VR4300 CPU and linked against a small runtime that
models the GBA hardware the game uses:

- graphics, drawn with the N64's RDP
- the sound FIFOs feeding the game's own music and sound-effect mixer
- DMA, timers, interrupts and the EEPROM save

The game itself is not emulated.

**No game data is included.** You need your own dump of the cartridge. The
build checks it: USA rev 0, `ALFE`, SHA-1
`18e0715dec419f3501c301511530d2edcd590f8b`.

## Controls

| GBA | N64 |
|---|---|
| D-pad | D-pad or analog stick |
| A | A |
| B | B |
| L | L or Z |
| R | R |
| Start | Start |
| Select | C-Down |

## Display

The N64 outputs 320×240. On field maps (exploration and fights) the view is
truly wider and taller than the GBA's 240×160: more of the authored world
is visible, and the HUD sits at the screen edges. Title, menu and status
screens are drawn at their native size, centred.

## Requirements

- An N64 with an Expansion Pak (8 MB), or an accurate emulator such as ares.
- A flash cart that supports 32 KB SRAM saves.

## Building

You need WSL (Ubuntu) with the [libdragon](https://github.com/DragonMinded/libdragon)
toolchain (`preview` branch) installed at `/opt/libdragon`, and Python 3 with
`capstone`.

The build uses a space-free path to the repository, so first create a symlink:

```bash
ln -s "/mnt/c/path/to/DBZ LoG2 N64" ~/dbzn64
```

Then:

1. **Find the game's code** (the ROM is only read):

   ```bash
   python3 tools/analyze.py baserom.gba tools/game.toml gen/analysis
   ```

2. **Generate the C code.** It is derived from your ROM, so keep it local:

   ```bash
   python3 tools/recomp.py baserom.gba gen/analysis/functions.json gen/c 24
   ```

3. **Build the N64 ROM**, which writes `platform/n64/dbz.z64`:

   ```bash
   make -C platform/n64 GEN=$PWD/gen/c BASEROM=$PWD/baserom.gba
   ```

A PC test build (`platform/host/build.sh`) runs the same recompiled code
headless. It writes screenshots, audio and saves for validation. See
`docs/ARCHITECTURE.md` for how everything fits together.

## Credits

- The technique and several game facts build on the PC recompilation by
  mstan: [DragonBallZLegacyofGokuIIRecomp](https://github.com/mstan/DragonBallZLegacyofGokuIIRecomp)
  and [gbarecomp](https://github.com/mstan/gbarecomp).
- Built with [libdragon](https://github.com/DragonMinded/libdragon).
- Hardware reference: GBATEK.

*Dragon Ball Z* and related names are trademarks of their respective owners.
This is a non-commercial preservation and research project, not affiliated
with Nintendo, Atari, Webfoot Technologies or any rights holder.

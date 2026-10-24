#!/bin/bash
# Capture the blend-scene test states with the host build (WSL).
#
#   bash tests/n64_render/capture_states.sh [OUT_DIR] [HOST_DBZ] [ROM]
#     OUT_DIR   where state_NNNNN.bin go (default /mnt/d/dbz-n64/runs/states,
#               the directory tests/n64_render/build.sh reads)
#     HOST_DBZ  host build (default /mnt/d/dbz-n64/build/host/dbz)
#     ROM       your LoG2 dump (default /mnt/d/dbz-n64/rom/baserom.gba)
#
# The key traces in keys/ replay a new game (320x240 view, --ws) into scenes
# that use the alpha-blend path:
#   keys/blend_a.txt  17400  Pepper Town rain (BG0 16/12 over the town, dialogue box)
#                     22100  dialogue outside Goku's house (14/6, BG0 empty)
#                     22800  Goku's home: sunbeams (BG0 8/16) and a dialogue box
#                     24000  a house with sunbeams (BG0 9/16)
#   keys/blend_b.txt  57000, 61000  the forest by Goku's house (14/6 set all the time)
# The states hold the game's video memory: keep them out of the repository.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/mnt/d/dbz-n64/runs/states}"
DBZ="${2:-/mnt/d/dbz-n64/build/host/dbz}"
ROM="${3:-/mnt/d/dbz-n64/rom/baserom.gba}"
TMP="$(mktemp -d)"
mkdir -p "$OUT"
"$DBZ" --rom "$ROM" --frames 24001 --every 1000000 --keys-file "$HERE/keys/blend_a.txt" \
    --out "$TMP" --ws --dump-state 17400,22100,22800,24000 | tail -1
"$DBZ" --rom "$ROM" --frames 61001 --every 1000000 --keys-file "$HERE/keys/blend_b.txt" \
    --out "$TMP" --ws --dump-state 57000,61000 | tail -1
# (the 320x240 screenshots stay in $TMP: fbdiff.py cross-checks 240x160 ones)
cp "$TMP"/state_*.bin "$OUT"/
ls "$OUT"

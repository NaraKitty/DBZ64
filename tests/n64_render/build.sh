#!/bin/bash
# Build the renderer test ROM, the host reference renderer and the test states (WSL).
#
#   bash tests/n64_render/build.sh [OUT] [STATES_DIR]
#     OUT        build directory (default /mnt/d/dbz-n64/build/n64_render)
#     STATES_DIR game state captures (default /mnt/d/dbz-n64/runs/states)
#   env: EXTRA_CFLAGS (e.g. -DRDPQ_VALIDATE), ONLY="name name" to limit the state list,
#        NO_CALIB=1 to skip the in-ROM blend calibration
#
# Produces OUT/rtest.z64 (DFS = OUT/fs: state_*.bin, synth_*.bin, states.txt) and
# OUT/ref/<name>.raw (ppu_soft reference, 240x160 BGR555 LE) for every state.
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-/mnt/d/dbz-n64/build/n64_render}"
STATES="${2:-/mnt/d/dbz-n64/runs/states}"
export N64_INST="${N64_INST:-/opt/libdragon}"
export PATH="$N64_INST/bin:$PATH"

mkdir -p "$OUT/fs" "$OUT/ref" "$OUT/host"
find "$OUT/fs" -maxdepth 1 -type f -delete

# host reference renderer
gcc -O2 -std=gnu11 -Wall -I"$REPO/runtime" -o "$OUT/host/host_ref" \
    "$REPO/tests/n64_render/host_ref.c" "$REPO/runtime/ppu_soft.c"

# states: game captures + synthetic
names=""
for f in "$STATES"/state_*.bin; do
    [ -e "$f" ] || continue
    cp "$f" "$OUT/fs/"
    names="$names $(basename "${f%.bin}")"
done
synth=$(python3 "$REPO/tests/n64_render/mksynth.py" "$OUT/fs")
names="$names $synth"
if [ -n "$ONLY" ]; then names="$ONLY"; fi
echo $names | tr ' ' '\n' > "$OUT/fs/states.txt"
if [ -n "$NO_CALIB" ]; then echo "#nocalib" >> "$OUT/fs/states.txt"; fi

args=""
for n in $names; do
    args="$args $OUT/fs/$n.bin $OUT/ref/$n.raw $OUT/fs/$n.bin $OUT/ref/$n.wide.raw $OUT/fs/$n.bin $OUT/ref/$n.wide2.raw"
done
"$OUT/host/host_ref" $args

if [ "$(cat "$OUT/.cflags" 2>/dev/null)" != "$EXTRA_CFLAGS" ]; then
    rm -rf "$OUT/tests" "$OUT/platform"
    echo "$EXTRA_CFLAGS" > "$OUT/.cflags"
fi
make -C "$OUT" -f "$REPO/tests/n64_render/Makefile" BUILD_DIR="$OUT" EXTRA_CFLAGS="$EXTRA_CFLAGS" -j8 rtest.z64
echo "built $OUT/rtest.z64 with $(echo $names | wc -w) states"

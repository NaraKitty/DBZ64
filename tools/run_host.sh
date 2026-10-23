#!/bin/bash
# Run the host build headless into a run directory (WSL).
#   [GAME=log2|buu] bash tools/run_host.sh NAME FRAMES EVERY [extra dbz args...]
# LoG2 uses /mnt/d/dbz-n64/{build/host,runs,rom/baserom.gba}, another game G
# /mnt/d/dbz-n64/G/{build/host,runs} and rom/G_baserom.gba.
# If RUNS/NAME/keys.txt exists it is used as the input script
# (lines like "600-605:start" or "1700:a+b").
set -e
NAME="$1"; FRAMES="$2"; EVERY="$3"
shift 3 || true
GAME="${GAME:-log2}"
if [ "$GAME" = log2 ]; then
    BASE=/mnt/d/dbz-n64; ROM=/mnt/d/dbz-n64/rom/baserom.gba
else
    BASE=/mnt/d/dbz-n64/$GAME; ROM=/mnt/d/dbz-n64/rom/${GAME}_baserom.gba
fi
OUT="$BASE/runs/$NAME"
mkdir -p "$OUT"
cd "$OUT"
KEYARGS=()
[ -f keys.txt ] && KEYARGS=(--keys-file keys.txt)
timeout 900 $BASE/build/host/dbz --rom $ROM \
    --frames "$FRAMES" --every "$EVERY" "${KEYARGS[@]}" --out . --wav out.wav "$@" 2>&1 | tail -40

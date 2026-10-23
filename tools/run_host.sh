#!/bin/bash
# Run the host build headless into a run directory (WSL).
#   bash tools/run_host.sh NAME FRAMES EVERY [extra dbz args...]
# If /mnt/d/dbz-n64/runs/NAME/keys.txt exists it is used as the input script
# (lines like "600-605:start" or "1700:a+b").
set -e
NAME="$1"; FRAMES="$2"; EVERY="$3"
shift 3 || true
OUT="/mnt/d/dbz-n64/runs/$NAME"
mkdir -p "$OUT"
cd "$OUT"
KEYARGS=()
[ -f keys.txt ] && KEYARGS=(--keys-file keys.txt)
timeout 900 /mnt/d/dbz-n64/build/host/dbz --rom /mnt/d/dbz-n64/rom/baserom.gba \
    --frames "$FRAMES" --every "$EVERY" "${KEYARGS[@]}" --out . --wav out.wav "$@" 2>&1 | tail -40

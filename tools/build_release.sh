#!/bin/bash
# Build a playable N64 ROM (no test script in the filesystem) from Git Bash
# on Windows, and copy it to OUT.
#   bash tools/build_release.sh OUT.z64 [make args, e.g. USE_RDP=1]
# Environment: BD, WSLREPO, GENDIR as in tools/run_n64.sh (BD default n64rel).
# The ROM embeds your GBA dump (DFS rom.bin): keep it to yourself.
set -e
OUT="$1"
shift || true
BD="${BD:-n64rel}"
WSLREPO="${WSLREPO:-/home/nara/dbzn64}"
GENDIR="${GENDIR:-/mnt/d/dbz-n64/gen/c}"
B="/d/dbz-n64/build/$BD"
mkdir -p "$B/filesystem"
# a test script left in the filesystem would put the ROM in test mode
rm -f "$B"/filesystem/*.txt "$B/dbz.dfs"
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "export N64_INST=/opt/libdragon; cd $WSLREPO/platform/n64 && make -j12 REPO=$WSLREPO GEN=$GENDIR BUILD_DIR=/mnt/d/dbz-n64/build/$BD $* > /mnt/d/dbz-n64/build/n64_make_$BD.log 2>&1 || { grep -E 'error' /mnt/d/dbz-n64/build/n64_make_$BD.log | head; exit 1; }; cp dbz.z64 /mnt/d/dbz-n64/build/$BD/dbz.z64"
cp "$B/dbz.z64" "$OUT"
ls -l "$OUT"

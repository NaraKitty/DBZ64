#!/bin/bash
# Build the N64 ROM with a test script and run it headless in ares (from Git
# Bash on Windows).  The test script is read by platform/n64/main.c.
#   bash tools/run_n64.sh NAME TIMEOUT_S TEST_SCRIPT_FILE [make args...]
# Environment:
#   GAME     log2 (default) or buu: games/$GAME
#   BD   build directory name under /d/dbz-n64/build (default n64t)
#   WSLREPO  repository path as seen from WSL (default ~/dbzn64; use a
#            worktree like /mnt/d/dbz-n64/wt-lead to build committed code)
#   ARES_ARGS extra ares options, e.g. "--setting Boot/Prefer=PAL"
#   GENDIR   generated C directory (WSL path, default /mnt/d/dbz-n64/gen/c,
#            /mnt/d/dbz-n64/$GAME/gen/c for games other than log2)
# Output: /d/dbz-n64/runs/NAME/{ares.log,shot_fNNNNN.png} (other games:
# /d/dbz-n64/$GAME/runs/NAME)
set -e
NAME="$1"; T="$2"; SCRIPT="$3"
shift 3 || true
GAME="${GAME:-log2}"
if [ "$GAME" = log2 ]; then
    RUN="/d/dbz-n64/runs/$NAME"
    GENDIR="${GENDIR:-/mnt/d/dbz-n64/gen/c}"
else
    RUN="/d/dbz-n64/$GAME/runs/$NAME"
    GENDIR="${GENDIR:-/mnt/d/dbz-n64/$GAME/gen/c}"
fi
BD="${BD:-n64t}"
WSLREPO="${WSLREPO:-/home/nara/dbzn64}"
mkdir -p "$RUN" /d/dbz-n64/build/$BD/filesystem
cp "$SCRIPT" /d/dbz-n64/build/$BD/filesystem/test.txt
# make runs in the build directory, which receives dbz.z64 (never the repo)
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "export N64_INST=/opt/libdragon; cd /mnt/d/dbz-n64/build/$BD && touch filesystem/test.txt && make -f $WSLREPO/platform/n64/Makefile -j12 REPO=$WSLREPO GAME=$GAME GEN=$GENDIR BUILD_DIR=/mnt/d/dbz-n64/build/$BD $* > /mnt/d/dbz-n64/build/n64_make_$BD.log 2>&1 || { grep -E 'error' /mnt/d/dbz-n64/build/n64_make_$BD.log | head; exit 1; }; cp dbz.z64 dbz_test.z64"
rm -f /d/dbz-n64/build/$BD/filesystem/test.txt
cd "/d/Games/ARES N64 EMULATOR"
# Input/Defocus=Allow: ares otherwise pauses when its window loses focus,
# which stalls test runs started in the background
./ares.exe --settings-file 'D:\dbz-n64\scratch\n64platform\ares_settings.bml' --setting Input/Defocus=Allow $ARES_ARGS \
    --system "Nintendo 64" --no-file-prompt "D:\\dbz-n64\\build\\${BD}\\dbz_test.z64" > "$RUN/ares.log" 2>&1 &
ARES=$!
# ares sometimes ignores the exit request after framebuffer dumps: stop it
# once the test reports completion (or a fatal error), or at the timeout
for ((i = 0; i < T; i++)); do
    kill -0 $ARES 2>/dev/null || break
    if grep -aq "^test done\|^FATAL\|^PANIC" "$RUN/ares.log"; then
        sleep 2
        break
    fi
    sleep 1
done
kill $ARES 2>/dev/null || true
wait $ARES 2>/dev/null || true
grep -a -v "^FB " "$RUN/ares.log" | tail -40
python "D:/dbz-n64/scratch/n64platform/fbdump2png.py" "$RUN/ares.log" "$RUN/shot" | tail -5

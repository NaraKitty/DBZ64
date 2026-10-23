#!/bin/bash
# Build the N64 ROM with a test script and run it headless in ares (from Git
# Bash on Windows).  The test script is read by platform/n64/main.c.
#   bash tools/run_n64.sh NAME TIMEOUT_S TEST_SCRIPT_FILE [make args...]
# Environment:
#   BD   build directory name under /d/dbz-n64/build (default n64t)
#   WSLREPO  repository path as seen from WSL (default ~/dbzn64; use a
#            worktree like /mnt/d/dbz-n64/wt-lead to build committed code)
#   GENDIR   generated C directory (WSL path, default /mnt/d/dbz-n64/gen/c)
# Output: /d/dbz-n64/runs/NAME/{ares.log,shot_fNNNNN.png}
set -e
NAME="$1"; T="$2"; SCRIPT="$3"
shift 3 || true
RUN="/d/dbz-n64/runs/$NAME"
BD="${BD:-n64t}"
WSLREPO="${WSLREPO:-/home/nara/dbzn64}"
GENDIR="${GENDIR:-/mnt/d/dbz-n64/gen/c}"
mkdir -p "$RUN" /d/dbz-n64/build/$BD/filesystem
cp "$SCRIPT" /d/dbz-n64/build/$BD/filesystem/test.txt
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "export N64_INST=/opt/libdragon; cd $WSLREPO/platform/n64 && touch /mnt/d/dbz-n64/build/$BD/filesystem/test.txt && make -j12 REPO=$WSLREPO GEN=$GENDIR BUILD_DIR=/mnt/d/dbz-n64/build/$BD $* > /mnt/d/dbz-n64/build/n64_make_$BD.log 2>&1 || { grep -E 'error' /mnt/d/dbz-n64/build/n64_make_$BD.log | head; exit 1; }; cp dbz.z64 /mnt/d/dbz-n64/build/$BD/dbz_test.z64"
rm -f /d/dbz-n64/build/$BD/filesystem/test.txt
cd "/d/Games/ARES N64 EMULATOR"
# Input/Defocus=Allow: ares otherwise pauses when its window loses focus,
# which stalls test runs started in the background
./ares.exe --settings-file 'D:\dbz-n64\scratch\n64platform\ares_settings.bml' --setting Input/Defocus=Allow \
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

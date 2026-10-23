#!/bin/bash
# Build the N64 ROM with a test script and run it headless in ares (from Git
# Bash on Windows).  The test script is read by platform/n64/main.c.
#   bash tools/run_n64.sh NAME TIMEOUT_S TEST_SCRIPT_FILE [make args...]
# Output: /d/dbz-n64/runs/NAME/{ares.log,shot_fNNNNN.png}
set -e
NAME="$1"; T="$2"; SCRIPT="$3"
shift 3 || true
RUN="/d/dbz-n64/runs/$NAME"
mkdir -p "$RUN" /d/dbz-n64/build/n64/filesystem
cp "$SCRIPT" /d/dbz-n64/build/n64/filesystem/test.txt
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "export N64_INST=/opt/libdragon; cd /home/nara/dbzn64/platform/n64 && touch /mnt/d/dbz-n64/build/n64/filesystem/test.txt && make -j12 $* > /mnt/d/dbz-n64/build/n64_make.log 2>&1 || { grep -E 'error' /mnt/d/dbz-n64/build/n64_make.log | head; exit 1; }; cp dbz.z64 /mnt/d/dbz-n64/build/n64/dbz_test.z64"
rm -f /d/dbz-n64/build/n64/filesystem/test.txt
cd "/d/Games/ARES N64 EMULATOR"
timeout "$T" ./ares.exe --settings-file 'D:\dbz-n64\scratch\n64platform\ares_settings.bml' \
    --system "Nintendo 64" --no-file-prompt 'D:\dbz-n64\build\n64\dbz_test.z64' > "$RUN/ares.log" 2>&1 || true
grep -v "^FB " "$RUN/ares.log" | tail -40
python "D:/dbz-n64/scratch/n64platform/fbdump2png.py" "$RUN/ares.log" "$RUN/shot" | tail -5

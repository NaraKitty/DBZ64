#!/bin/bash
# Run the renderer test ROM headless in ares and diff its framebuffer dumps
# against the ppu_soft references.  Git Bash on Windows (ares is a Windows app).
#
#   bash tests/n64_render/run.sh [BUILD_DIR] [timeout_s]
#   env: ARES_DIR       (default "/d/Games/ARES N64 EMULATOR")
#        ARES_SETTINGS  settings template, copied once to BUILD_DIR/ares_settings.bml
#                       (default /d/dbz-n64/scratch/n64platform/ares_settings.bml)
# Output: BUILD_DIR/ares.log, BUILD_DIR/diff/*.png, summary on stdout.
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/d/dbz-n64/build/n64_render}"
T="${2:-900}"
ARES_DIR="${ARES_DIR:-/d/Games/ARES N64 EMULATOR}"
SETTINGS_SRC="${ARES_SETTINGS:-/d/dbz-n64/scratch/n64platform/ares_settings.bml}"
[ -f "$OUT/ares_settings.bml" ] || cp "$SETTINGS_SRC" "$OUT/ares_settings.bml"
SETTINGS_W="$(cygpath -w "$OUT/ares_settings.bml")"
ROM_W="$(cygpath -w "$OUT/rtest.z64")"
LOG="$OUT/ares.log"
start=$(date +%s)
# Audio stays as configured in the settings file (never muted or disabled:
# the test ROM is silent anyway, and test runs must keep emulator audio).
# Input/Defocus=Allow: keep running while another window has the focus (the
# settings' "Pause" froze runs whenever another program came to the front).
( cd "$ARES_DIR" && timeout "$T" ./ares.exe --settings-file "$SETTINGS_W" \
    --system "Nintendo 64" --no-file-prompt \
    --setting General/HomebrewMode=true --setting Video/Driver=None \
    --setting Input/Driver=None --setting Input/Defocus=Allow $ARES_EXTRA \
    "$ROM_W" > "$LOG" 2>&1 )
rc=$?
echo "ares exit=$rc (124 = timeout) after $(( $(date +%s) - start ))s, log $(wc -c < "$LOG") bytes"
python "$HERE/fbdiff.py" "$(cygpath -w "$LOG")" "$(cygpath -w "$OUT")"

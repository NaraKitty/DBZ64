#!/bin/bash
# Build the host test harness in WSL.
#   [GAME=log2|buu] [DEBUG=1] bash platform/host/build.sh [gen_dir] [out_dir]
# The game's directory (games/$GAME: game.h and its C sources) is compiled in.
# Default gen/out dirs: LoG2 /mnt/d/dbz-n64/{gen/c,build/host}, other games
# /mnt/d/dbz-n64/$GAME/{gen/c,build/host}.
# DEBUG=1 builds the debug harness (platform/host/debug.c) for generated code
# made with tools/recomp.py --trace.
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${GAME:-log2}"
GAMEDIR="$REPO/games/$GAME"
[ -f "$GAMEDIR/game.h" ] || { echo "unknown game '$GAME' (no $GAMEDIR/game.h)"; exit 1; }
if [ "$GAME" = log2 ]; then
    GEN="${1:-/mnt/d/dbz-n64/gen/c}"
    OUT="${2:-/mnt/d/dbz-n64/build/host}"
else
    GEN="${1:-/mnt/d/dbz-n64/$GAME/gen/c}"
    OUT="${2:-/mnt/d/dbz-n64/$GAME/build/host}"
fi
mkdir -p "$OUT/obj"
CFLAGS="-O2 -g -std=gnu11 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -fno-strict-aliasing ${EXTRA_CFLAGS}"
INC="-I$REPO/runtime -I$GAMEDIR -I$GEN"
SRCS="$REPO/runtime/mem.c $REPO/runtime/io.c $REPO/runtime/bios.c $REPO/runtime/dispatch.c \
      $REPO/runtime/runtime.c $REPO/runtime/interp.c $REPO/runtime/native.c $REPO/runtime/ppu_soft.c \
      $GAMEDIR/*.c $REPO/platform/host/main.c"
if [ -n "$DEBUG" ]; then
    CFLAGS="$CFLAGS -DRECOMP_TRACE"
    SRCS="$SRCS $REPO/platform/host/debug.c"
fi
OBJS=""
# objects of one game (or of the debug build) must not be reused for another
KIND="$GAME${DEBUG:+ debug}"
if [ "$(cat "$OUT/obj/game.txt" 2>/dev/null)" != "$KIND" ]; then
    FORCE=1
    echo "$KIND" > "$OUT/obj/game.txt"
fi
compile() {
    local src="$1" obj="$2"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ -n "$FORCE" ]; then
        gcc $CFLAGS $INC -c "$src" -o "$obj" || exit 1
    fi
}
for s in $SRCS; do
    o="$OUT/obj/$(basename "${s%.c}").o"
    compile "$s" "$o" &
    OBJS="$OBJS $o"
done
for s in "$GEN"/gen_*.c; do
    o="$OUT/obj/$(basename "${s%.c}").o"
    compile "$s" "$o" &
    OBJS="$OBJS $o"
done
wait
gcc $CFLAGS -o "$OUT/dbz" $OBJS
echo "built $OUT/dbz ($KIND)"

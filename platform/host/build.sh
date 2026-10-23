#!/bin/bash
# Build the host test harness in WSL.
#   bash platform/host/build.sh [gen_dir] [out_dir]
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
GEN="${1:-/mnt/d/dbz-n64/gen/c}"
OUT="${2:-/mnt/d/dbz-n64/build/host}"
mkdir -p "$OUT/obj"
CFLAGS="-O2 -g -std=gnu11 -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -fno-strict-aliasing ${EXTRA_CFLAGS}"
INC="-I$REPO/runtime -I$GEN"
SRCS="$REPO/runtime/mem.c $REPO/runtime/io.c $REPO/runtime/bios.c $REPO/runtime/dispatch.c \
      $REPO/runtime/runtime.c $REPO/runtime/interp.c $REPO/runtime/native.c $REPO/runtime/ppu_soft.c $REPO/runtime/widescreen.c \
      $REPO/platform/host/main.c"
OBJS=""
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
echo "built $OUT/dbz"

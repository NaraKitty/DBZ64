#!/usr/bin/env python3
"""Turn host ROM traces into the boot preload list of the N64 ROM cache.

The host harness run with --rom-profile writes rom_trace.bin (one 256-byte
bitmap of the 2048 4KB ROM pages per frame).  Every page the game touches
during play is a cartridge read on the N64 the first time; loading them at
boot (with interrupts on, behind the logos) removes those stalls.

usage: rom_hotset.py TRACE [TRACE...] [--max N]
Prints a [rom] preload = [...] entry for tools/game.toml: 16KB page indices
in order of first use (traces in the order given), at most N (default 128,
the 2MB cache).
"""
import sys


def pages16(path):
    data = open(path, 'rb').read()
    order, seen = [], set()
    for f in range(len(data) // 256):
        bm = data[f * 256:(f + 1) * 256]
        for i, byte in enumerate(bm):
            if not byte:
                continue
            for b in range(8):
                if byte >> b & 1:
                    p = (i * 8 + b) >> 2
                    if p not in seen:
                        seen.add(p)
                        order.append(p)
    return order


def main():
    args = sys.argv[1:]
    limit = 128
    if '--max' in args:
        k = args.index('--max')
        limit = int(args[k + 1])
        del args[k:k + 2]
    order, seen = [], set()
    for path in args:
        for p in pages16(path):
            if p not in seen:
                seen.add(p)
                order.append(p)
    order = order[:limit]
    print('# %d 16KB pages (%d KB), first use order, from %s' % (
        len(order), len(order) * 16, ', '.join(args)))
    print('[rom]')
    lines, cur = [], []
    for p in order:
        cur.append('%d' % p)
        if len(cur) == 16:
            lines.append(', '.join(cur))
            cur = []
    if cur:
        lines.append(', '.join(cur))
    print('preload = [\n    ' + ',\n    '.join(lines) + ',\n]')


if __name__ == '__main__':
    main()

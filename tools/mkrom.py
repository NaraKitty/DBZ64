"""Prepare the user's GBA ROM for the N64 build.

Verifies the ROM identity against games/GAME/game.h (GAME_ROM_SHA1) and
writes it with every 32-bit word byte-reversed, so a big-endian 32-bit load
on the N64 yields the GBA's little-endian word (the runtime's native-word
memory layout).  The output goes into the N64 build's DFS filesystem; it is
never committed.

    python3 tools/mkrom.py [--game log2|buu] baserom.gba out/rom.bin
"""
import hashlib
import os
import re
import sys


def game_info(game):
    """GAME_* string defines of games/<game>/game.h."""
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(repo, 'games', game, 'game.h')
    text = open(path, encoding='utf-8').read()
    return dict(re.findall(r'^#define\s+(GAME_\w+)\s+"([^"]*)"', text, re.M))


def main():
    args = sys.argv[1:]
    game = 'log2'
    if args[:1] == ['--game']:
        game, args = args[1], args[2:]
    src, dst = args[0], args[1]
    info = game_info(game)
    want = info['GAME_ROM_SHA1']
    data = open(src, 'rb').read()
    h = hashlib.sha1(data).hexdigest()
    if h != want:
        sys.exit('%s: unsupported ROM (sha1 %s, need %s: %s (%s))'
                 % (src, h, want, info['GAME_TITLE'], info['GAME_CODE']))
    out = bytearray(len(data))
    out[0::4] = data[3::4]
    out[1::4] = data[2::4]
    out[2::4] = data[1::4]
    out[3::4] = data[0::4]
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    main()

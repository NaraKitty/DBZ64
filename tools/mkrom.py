"""Prepare the user's GBA ROM for the N64 build.

Verifies the ROM identity and writes it with every 32-bit word byte-reversed,
so a big-endian 32-bit load on the N64 yields the GBA's little-endian word
(the runtime's native-word memory layout).  The output goes into the N64
build's DFS filesystem; it is never committed.

    python3 tools/mkrom.py baserom.gba out/rom.bin
"""
import hashlib
import sys

SHA1 = '18e0715dec419f3501c301511530d2edcd590f8b'


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    h = hashlib.sha1(data).hexdigest()
    if h != SHA1:
        sys.exit('%s: unsupported ROM (sha1 %s, need %s: Dragon Ball Z - '
                 'The Legacy of Goku II (USA))' % (src, h, SHA1))
    out = bytearray(len(data))
    out[0::4] = data[3::4]
    out[1::4] = data[2::4]
    out[2::4] = data[1::4]
    out[3::4] = data[0::4]
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    main()

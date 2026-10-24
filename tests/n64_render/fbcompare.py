#!/usr/bin/env python3
"""
Compare the framebuffer dumps of two renderer test runs pixel for pixel
(e.g. the current renderer against a baseline build of an earlier commit),
independently of the ppu_soft references.

    python fbcompare.py BASE_ARES_LOG NEW_ARES_LOG

Every FBDUMP block present in both logs (state renders, the DYN/WIDE/SEQ
sequences, the blend calibration dump) is compared; prints the blocks that
differ and a summary.  Exit status 1 if any block differs or is missing.
"""
import sys

import numpy as np


def dumps(path):
    out, cur, rows = {}, None, None
    with open(path, 'r', errors='replace') as f:
        for ln in f:
            if ln.startswith('FBDUMP '):
                _, tag, w, h = ln.split()[:4]
                cur, rows = tag, np.zeros((int(h), int(w)), np.uint16)
            elif ln.startswith('FB ') and cur is not None:
                try:
                    _, y, hx = ln.split(' ', 2)
                    hx = hx.strip()
                    row = np.frombuffer(bytes.fromhex(hx[:len(hx) // 4 * 4]), dtype='>u2')
                    rows[int(y), :len(row)] = row[:rows.shape[1]]
                except ValueError:
                    pass
            elif ln.startswith('FBEND') and cur is not None:
                out[cur] = rows
                cur = None
    return out


def main():
    a, b = dumps(sys.argv[1]), dumps(sys.argv[2])
    bad = 0
    for tag in sorted(set(a) | set(b)):
        if tag not in a or tag not in b:
            print(f'missing in {"base" if tag not in a else "new"}: {tag}')
            bad += 1
            continue
        n = int(np.count_nonzero(a[tag] != b[tag]))
        if n:
            print(f'DIFF {tag}: {n} pixels')
            bad += 1
    print(f'{len(set(a) & set(b))} framebuffers compared, {bad} differ or missing')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()

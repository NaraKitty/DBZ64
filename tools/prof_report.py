"""Symbolise an N64 profile from an ares log ("PROF hits addr" lines).

    python tools/prof_report.py ares.log path/to/dbz.elf(WSL path) [top]

Each sampled 32-byte code line is resolved with addr2line -i (in WSL), the
outermost function wins (generated guest functions T_/A_ show up even when
the hot line is an inlined accessor), and hits are summed per function.
"""
import re
import subprocess
import sys
from collections import Counter


def main():
    log, elf = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    rows = []
    total = 0
    for ln in open(log, errors='replace'):
        m = re.match(r'PROFILE (\d+) samples', ln)
        if m:
            total = int(m.group(1))
        m = re.match(r'PROF (\d+) ([0-9A-Fa-f]{8})', ln)
        if m:
            rows.append((int(m.group(1)), m.group(2)))
    # one addr2line call per address keeps the inline chains easy to split
    per = Counter()
    detail = []
    for hits, a in rows:
        r = subprocess.run(['wsl.exe', '-d', 'Ubuntu', '--', '/opt/libdragon/bin/mips64-elf-addr2line',
                            '-i', '-f', '-s', '-e', elf, '0x' + a], capture_output=True, text=True).stdout.split('\n')
        funcs = [r[i] for i in range(0, len(r) - 1, 2)]
        outer = funcs[-1] if funcs else '??'
        per[outer] += hits
        detail.append((hits, a, ' <- '.join(funcs)))
    print('total samples', total)
    for f, h in per.most_common(top):
        print('%6.2f%% %6d %s' % (100.0 * h / max(total, 1), h, f))
    print('\nhottest lines:')
    for hits, a, chain in detail[:25]:
        print('%6d %s %s' % (hits, a, chain))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Compare the N64 renderer's framebuffer dumps with the ppu_soft references.

    python fbdiff.py ARES_LOG BUILD_DIR [--states DIR] [--ox 40 --oy 40]

ARES_LOG   ares stdout with "FBDUMP tag W H" / "FB y hex" / "FBEND tag" blocks
           (RGBA5551) and TIME / CMP / CALIB lines from tests/n64_render/main.c
BUILD_DIR  contains ref/<tag>.raw (240x160 BGR555 LE from host_ref)
--states   directory with the game's frame_NNNNN.png (cross-check of host_ref)

Writes BUILD_DIR/diff/<tag>_{n64,ref,diff}.png and prints one line per state:
pixels differing in the GBA region, max channel delta, border check, timings.
Exit status 1 if any state differs or the border is not black.
"""
import argparse
import os
import re
import sys

import numpy as np
from PIL import Image


def parse_log(path):
    dumps, times, cmps, calib, other = {}, {}, {}, [], []
    global dyn
    dyn = []
    cur = None
    with open(path, 'r', errors='replace') as f:
        for ln in f:
            ln = ln.rstrip('\n')
            if ln.startswith('FB ') and cur is not None:
                try:
                    _, y, hx = ln.split(' ', 2)
                    y = int(y)
                    hx = hx.strip()
                    row = np.frombuffer(bytes.fromhex(hx[:len(hx) // 4 * 4]), dtype='>u2')
                except ValueError:
                    continue
                cur[1][y, :len(row)] = row[:cur[1].shape[1]]
            elif ln.startswith('FBDUMP '):
                _, tag, w, h = ln.split()[:4]
                cur = (tag, np.zeros((int(h), int(w)), np.uint16))
            elif ln.startswith('FBEND') and cur is not None:
                dumps[cur[0]] = cur[1]
                cur = None
            elif ln.startswith('TIME '):
                parts = ln.split()
                kv = dict(p.split('=', 1) for p in parts[2:])
                times.setdefault(parts[1], {})[int(kv['pass'])] = kv
            elif ln.startswith('CMP '):
                parts = ln.split()
                cmps[parts[1]] = dict(p.split('=', 1) for p in parts[2:])
            elif ln.startswith('DYN ') or ln.startswith('WIDE ') or ln.startswith('SEQ '):
                dyn.append(ln)
            elif ln.startswith('CALIB'):
                calib.append(ln)
            elif 'ERROR' in ln or 'ASSERT' in ln or 'assert' in ln or 'RDPQ' in ln or 'panic' in ln.lower():
                other.append(ln)
    return dumps, times, cmps, calib, other


def rdp_estimate_us(t):
    """Rough RDP time from the pixel/load counters (ares does not model RDP
    timing): FILL and COPY 4 px/clk, 1-cycle 1 px/clk, 2-cycle 0.5 px/clk,
    TMEM loads 8 bytes/clk, ~24 clk setup per rectangle and per load; 62.5 MHz."""
    g = lambda k: int(t.get(k, 0))
    clk = (g('pxfill') + g('pxcopy')) / 4 + g('px1') + 2 * g('px2') + g('loadbytes') / 8         + 24 * (g('rects') + g('loads'))
    return clk / 62.5


def rgba5551_to_rgb5(a):
    return np.stack([(a >> 11) & 31, (a >> 6) & 31, (a >> 1) & 31], axis=-1).astype(np.int16)


def bgr555_to_rgb5(a):
    return np.stack([a & 31, (a >> 5) & 31, (a >> 10) & 31], axis=-1).astype(np.int16)


def to_img(rgb5, scale=2):
    img = Image.fromarray(((rgb5 << 3) | (rgb5 >> 2)).astype(np.uint8), 'RGB')
    return img.resize((img.width * scale, img.height * scale), Image.NEAREST)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('build')
    ap.add_argument('--states', default='D:/dbz-n64/runs/states')
    ap.add_argument('--ox', type=int, default=40)
    ap.add_argument('--oy', type=int, default=40)
    a = ap.parse_args()
    dumps, times, cmps, calib, other = parse_log(a.log)
    outdir = os.path.join(a.build, 'diff')
    os.makedirs(outdir, exist_ok=True)
    for ln in other[:40]:
        print('LOG:', ln)
    fail = 0
    wide_ref = {}
    wide2_ref = {}
    print(f"{'state':22s} {'diffpx':>7s} {'maxd':>4s} {'border':>6s} {'p0/p1/p2':>9s} "
          f"{'cpu0':>6s} {'cpu1':>6s} {'cpu2':>6s} {'rdpEst':>6s} notes")
    for tag, fb in dumps.items():
        if '.dyn' in tag:
            got = rgba5551_to_rgb5(fb)
            to_img(got).save(os.path.join(outdir, tag + '.png'))
            wref = os.path.join(a.build, 'ref', tag.split('.')[0] + '.wide.raw')
            wref2 = os.path.join(a.build, 'ref', tag.split('.')[0] + '.wide2.raw')
            if tag.endswith('.dyn_wide2') and os.path.exists(wref2):
                # hook-less 320x240 view against ppu_soft_render_view without hooks
                ref5 = bgr555_to_rgb5(np.fromfile(wref2, dtype='<u2').reshape(240, 320))
                d = np.abs(got - ref5)
                bad = np.any(d != 0, axis=-1)
                wide2_ref[tag.split('.')[0]] = (int(bad.sum()), int(d.max()))
                if bad.any():
                    dm = np.zeros((240, 320, 3), np.int16)
                    dm[bad] = (31, 0, 31)
                    dm[~bad] = ref5[~bad] // 3
                    to_img(dm).save(os.path.join(outdir, tag + '_diff.png'))
            if tag.endswith('.dyn_wide') and os.path.exists(wref):
                ref5 = bgr555_to_rgb5(np.fromfile(wref, dtype='<u2').reshape(240, 320))
                d = np.abs(got - ref5)
                bad = np.any(d != 0, axis=-1)
                wide_ref[tag.split('.')[0]] = (int(bad.sum()), int(d.max()))
                if bad.any():
                    dm = np.zeros((240, 320, 3), np.int16)
                    dm[bad] = (31, 0, 31)
                    dm[~bad] = ref5[~bad] // 3
                    to_img(dm).save(os.path.join(outdir, tag + '_diff.png'))
            continue
        base = tag.split('.')[0]
        refp = os.path.join(a.build, 'ref', base + '.raw')
        if not os.path.exists(refp):
            continue
        ref = np.fromfile(refp, dtype='<u2').reshape(160, 240)
        ref5 = bgr555_to_rgb5(ref)
        notes = []
        m = re.match(r'state_(\d+)$', base)
        if m:
            png = os.path.join(a.states, f'frame_{m.group(1)}.png')
            if os.path.exists(png):
                p = np.asarray(Image.open(png).convert('RGB')).astype(np.int16) >> 3
                hd = int(np.any(p != ref5, axis=-1).sum())
                if hd:
                    notes.append(f'host_ref!=png:{hd}')
        got = rgba5551_to_rgb5(fb)
        reg = got[a.oy:a.oy + 160, a.ox:a.ox + 240]
        d = np.abs(reg - ref5)
        bad = np.any(d != 0, axis=-1)
        nbad = int(bad.sum())
        maxd = int(d.max())
        border = got.copy()
        border[a.oy:a.oy + 160, a.ox:a.ox + 240] = 0
        nborder = int(np.any(border != 0, axis=-1).sum())
        if nbad or nborder:
            fail = 1
        if nbad:
            hist = np.bincount(d[bad].max(axis=-1).ravel(), minlength=32)
            notes.append('deltas:' + ','.join(f'{k}:{hist[k]}' for k in range(1, 32) if hist[k]))
        c = cmps.get(tag, {}) if tag == base else {}
        pp = f"{c.get('pass0_vs_1', '?')}/{c.get('pass2_vs_1', '?')}"
        if c and (c.get('pass0_vs_1') != '0' or c.get('pass2_vs_1') != '0'):
            fail = 1
        t = times.get(tag, {}) if tag == base else {}
        cpu = [t.get(p, {}).get('cpu', '?') for p in range(3)]
        rdp = f"{rdp_estimate_us(t[1]):.0f}" if 1 in t else '?'
        bl = t.get(1, {}).get('blend')
        if bl in ('1', '2'):
            notes.append('blend32' if bl == '1' else 'blend32+tlutfinal')
        if t.get(1, {}).get('tags') == '1':
            notes.append('tags')
        print(f"{tag:22s} {nbad:7d} {maxd:4d} {nborder:6d} {pp:>9s} {cpu[0]:>6s} {cpu[1]:>6s} {cpu[2]:>6s} {rdp:>6s} {' '.join(notes)}")
        to_img(got).save(os.path.join(outdir, tag + '_n64.png'))
        to_img(ref5).save(os.path.join(outdir, tag + '_ref.png'))
        if nbad:
            dm = np.zeros((160, 240, 3), np.int16)
            dm[bad] = (31, 0, 31)
            dm[~bad] = ref5[~bad] // 3
            to_img(dm).save(os.path.join(outdir, tag + '_diff.png'))
    missing = [t for t in times if t not in dumps]
    if missing:
        print('no dump for:', ' '.join(missing))
        fail = 1
    if dyn:
        nbad = 0
        for l in dyn:
            kv = dict(p.split('=', 1) for p in l.split()[2:])
            if l.startswith('WIDE'):
                name = l.split()[1]
                if name in wide_ref:
                    l += f" vs_ppu_soft_view: diffpx={wide_ref[name][0]} maxd={wide_ref[name][1]}"
                bad = kv['centre_diff'] != '0' or wide_ref.get(name, (0, 0))[0] != 0
            else:
                bad = any(v != '0' for k, v in kv.items() if k.endswith('_diff'))
            nbad += bad
            print(('DIFF: ' if bad else '') + l)
        if nbad:
            fail = 1
    for name, (n, mx) in wide2_ref.items():
        if n:
            fail = 1
        print(('DIFF: ' if n else '') + f'WIDE2 {name} hookless_vs_ppu_soft_view: diffpx={n} maxd={mx}')
    if calib:
        bad_pairs = [l for l in calib if 'badpx=0 ' not in l and not l.startswith('CALIB_TOTAL')]
        print(f'calibration: {len(calib) - 1} (EVA,EVB) pairs, {len(bad_pairs)} with differences')
        for l in bad_pairs[:400]:
            print('  ' + l)
        for l in calib:
            if l.startswith('CALIB_TOTAL'):
                print(l)
    sys.exit(fail)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
RDP cost model: estimate the real-hardware RDP time of the frames logged by
the CMDLOG build of the renderer test ROM (tests/n64_render/main.c).

    python rdpcost.py LOG [--detail STATE] [--hyp z|c32] [--set name=value] [--json OUT]
    python rdpcost.py --compare BASE_LOG NEW_LOG      before/after per state
    python rdpcost.py --fit BASE_LOG                  refit the free constants

ares (paraLLEl-RDP) does not model RDP timing, so the time is estimated from
the command stream: rdpq's trace engine (RDPQ_VALIDATE) prints every command
the RDP receives between CMDLOG_BEGIN/END, and this script replays it (render
modes, colour/Z images, scissor, texture loads) and charges every primitive
for its spans (scanlines): pixel pipeline clocks, colour/Z bytes read and
written, Z and 32-bit penalties, fixed costs per span, primitive, load, sync.

The constants and their calibration against frame rates measured on the
Analogue 3D are described in docs/RDP_COST.md.  The measurements cannot tell
whether the slow part of the old blend path was its Z traffic or its 32-bit
colour spans (every 32-bit span there also wrote or read Z), so there are two
calibrations: 'z' (default) charges the penalty to spans that touch Z, 'c32'
to every 1/2-cycle span with a 32-bit colour image.
"""
import argparse
import itertools
import json
import re
from collections import defaultdict

RDP_HZ = 62.5e6
K = dict(
    # pixel pipeline, clocks per pixel
    px_fill16=0.25, px_fill32=0.5, px_copy16=0.25, px_1cyc=1.0, px_2cyc=2.0,
    # effective RDRAM bytes per clock for span reads/writes and texture loads
    # (the 500 MB/s peak is 8 bytes per RDP clock)
    bw=4.0,
    # extra clocks per pixel: z_px for a span that reads or writes Z,
    # c32_px for a 1/2-cycle span with a 32-bit colour image (calibrated)
    z_px=8.5, c32_px=0.0,
    # fixed clocks per span in 1/2-cycle mode and in fill/copy mode, extra
    # latency of a span that must read colour or Z before it is shaded
    span=8.0, span_fc=2.0, read_lat=30.0,
    # fixed clocks per primitive, command, sync, TMEM load (+ per LOAD_TILE
    # line), TLUT load
    prim=20.0, cmd=2.0, sync_pipe=30.0, sync_load=15.0, sync_tile=8.0, sync_full=150.0,
    load=30.0, load_row=6.0, tlut=200.0,
)
HYP = {'z': dict(z_px=8.5, c32_px=0.0), 'c32': dict(z_px=0.0, c32_px=8.5)}

# Real RDP time per frame on the Analogue 3D, derived from the overlay's
# "draw" (D) with the pacing of platform/n64/main.c at the time (a drawn
# frame waits for the previous one's RDP work, two late frames are skipped
# in between): R = D + 3 * game (6 ms) - CPU drawing time (2-3 ms) ~ D + 16.
MEASURED = {
    'state_22100': 61.0,    # dialogue outside Goku's house (14/6, BG0 empty), like the forest (45.6) / cave (43.8)
    'state_57000': 61.0,    # forest by Goku's house (14/6 all the time, BG0 empty)
    'state_17400': 62.0,    # Pepper Town rain (16/12, tags, TLUT final pass): draw 46
    'state_22800': 54.0,    # house interiors with sunbeams: draw 36-39
    'state_24000': 54.0,
}
ORDINARY = ['state_04500', 'state_03300']   # no EVB>0 blending: draw ~5 ms (CPU-bound)

CMD_WORDS = {0x24: 2, 0x25: 2}
for c in range(0x08, 0x10):
    CMD_WORDS[c] = 4 + (8 if c & 4 else 0) + (8 if c & 2 else 0) + (2 if c & 1 else 0)


def bits(w, lo, hi):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def parse_log(path):
    """{(state, view): [word]} from the CMDLOG blocks"""
    out = {}
    cur = None
    words = []
    rx = re.compile(r'^\[0x[0-9a-f]+\] ([0-9a-f]{16})')
    with open(path, 'r', errors='replace') as f:
        for ln in f:
            if ln.startswith('CMDLOG_BEGIN'):
                _, st, view = ln.split()[:3]
                cur = (st, view)
                words = []
            elif ln.startswith('CMDLOG_END'):
                if cur:
                    out[cur] = words
                cur = None
            elif cur is not None:
                m = rx.match(ln)
                if m:
                    words.append(int(m.group(1), 16))
    return out


class Sim:
    def __init__(self, k):
        self.k = k
        self.cycle = 0          # 0 1cyc, 1 2cyc, 2 copy, 3 fill
        self.img_read = self.z_cmp = self.z_upd = 0
        self.bpp = 2
        self.scissor = (0, 0, 320, 240)
        self.tex_size = 2
        self.clk = 0.0
        self.cat = defaultdict(float)       # clocks per category
        self.cnt = defaultdict(float)       # counters

    def mode_name(self):
        n = ['1cyc', '2cyc', 'copy', 'fill'][self.cycle] + '%d' % (self.bpp * 8)
        if self.cycle < 2:
            if self.img_read: n += '+rd'
            if self.z_cmp: n += '+zr'
            if self.z_upd: n += '+zw'
        return n

    def add(self, cat, clk):
        self.clk += clk
        self.cat[cat] += clk

    def rect(self, w0):
        k = self.k
        xl, yl = bits(w0, 44, 55), bits(w0, 32, 43)
        xh, yh = bits(w0, 12, 23), bits(w0, 0, 11)
        sx0, sy0, sx1, sy1 = self.scissor
        fc = self.cycle >= 2
        # fill/copy: inclusive right/bottom edge; 1/2-cycle: exclusive
        x0, y0 = (xh + 3) >> 2, (yh + 3) >> 2
        x1 = (xl >> 2) + 1 if fc else (xl + 3) >> 2
        y1 = (yl >> 2) + 1 if fc else (yl + 3) >> 2
        x0, y0 = max(x0, sx0), max(y0, sy0)
        x1, y1 = min(x1, sx1), min(y1, sy1)
        w, h = x1 - x0, y1 - y0
        mode = self.mode_name()
        self.add('prim', k['prim'])
        self.cnt['prims'] += 1
        self.cnt['prims_' + mode] += 1
        if w <= 0 or h <= 0:
            return
        self.cnt['px_' + mode] += w * h
        self.cnt['spans_' + mode] += h
        if self.cycle == 3:
            pclk = w * (k['px_fill16'] if self.bpp <= 2 else k['px_fill32'])
        elif self.cycle == 2:
            pclk = w * k['px_copy16'] * max(1, self.bpp / 2)
        else:
            pclk = w * (k['px_1cyc'] if self.cycle == 0 else k['px_2cyc'])
        zr = self.z_cmp and not fc
        zw = self.z_upd and not fc
        wr = w * self.bpp + (2 * w if zw else 0)
        rd = (w * self.bpp if (self.img_read and not fc) else 0) + (2 * w if zr else 0)
        mem = (wr + rd) / k['bw']
        pen = (k['z_px'] if (zr or zw) else 0.0) + (k['c32_px'] if (self.bpp == 4 and not fc) else 0.0)
        span_clk = max(pclk, mem) + pen * w + (k['read_lat'] if rd else 0.0) + \
            (k['span_fc'] if fc else k['span'])
        self.add(mode, span_clk * h)
        self.cnt['bytes_rw'] += (wr + rd) * h

    def step(self, w):
        k = self.k
        op = (w >> 56) & 0x3F
        self.add('cmd', k['cmd'])
        self.cnt['cmds'] += 1
        if op == 0x3F:          # SET_COLOR_IMAGE
            self.bpp = [0.5, 1, 2, 4][bits(w, 51, 52)]
        elif op == 0x2F:        # SET_OTHER_MODES
            self.cycle = bits(w, 52, 53)
            self.z_cmp = bits(w, 4, 4)
            self.z_upd = bits(w, 5, 5)
            self.img_read = bits(w, 6, 6)
            self.cnt['som'] += 1
        elif op == 0x2D:        # SET_SCISSOR
            self.scissor = (bits(w, 44, 55) >> 2, bits(w, 32, 43) >> 2,
                            bits(w, 12, 23) >> 2, bits(w, 0, 11) >> 2)
        elif op in (0x36, 0x24, 0x25):
            self.rect(w)
        elif op == 0x27:
            self.add('sync', k['sync_pipe']); self.cnt['syncs'] += 1
        elif op == 0x26:
            self.add('sync', k['sync_load']); self.cnt['syncs'] += 1
        elif op == 0x28:
            self.add('sync', k['sync_tile']); self.cnt['syncs'] += 1
        elif op == 0x29:
            self.add('sync', k['sync_full']); self.cnt['syncs'] += 1
        elif op == 0x3D:        # SET_TEXTURE_IMAGE
            self.tex_size = bits(w, 51, 52)
        elif op == 0x34:        # LOAD_TILE
            s0, t0 = bits(w, 44, 55) >> 2, bits(w, 32, 43) >> 2
            s1, t1 = bits(w, 12, 23) >> 2, bits(w, 0, 11) >> 2
            rows = t1 - t0 + 1
            b = (s1 - s0 + 1) * [0.5, 1, 2, 4][self.tex_size] * rows
            self.add('load', k['load'] + rows * k['load_row'] + b / k['bw'])
            self.cnt['loads'] += 1
            self.cnt['load_bytes'] += b
        elif op == 0x33:        # LOAD_BLOCK
            b = (bits(w, 12, 23) + 1) * [0.5, 1, 2, 4][self.tex_size]
            self.add('load', k['load'] + b / k['bw'])
            self.cnt['loads'] += 1
            self.cnt['load_bytes'] += b
        elif op == 0x30:        # LOAD_TLUT
            self.add('load', k['tlut'])
            self.cnt['tlut_loads'] += 1
        return CMD_WORDS.get(op, 1)


def simulate(words, k=K):
    """(estimated RDP ms, Sim) of a logged frame"""
    s = Sim(k)
    i = 0
    while i < len(words):
        i += s.step(words[i])
    return s.clk / RDP_HZ * 1e3, s


def consts(hyp, overrides=()):
    k = dict(K)
    k.update(HYP[hyp])
    for kv in overrides:
        name, v = kv.split('=')
        k[name] = float(v)
    return k


def fit(logs):
    """least squares (relative) of the free constants against MEASURED, per hypothesis"""
    for hyp, name in (('z', 'z_px'), ('c32', 'c32_px')):
        best = None
        for v, bw in itertools.product([x * 0.5 for x in range(0, 41)], [2.0, 3.0, 4.0, 6.0, 8.0]):
            k = consts(hyp)
            k[name] = v
            k['bw'] = bw
            err = 0.0
            for st, m in MEASURED.items():
                if (st, 'wide') in logs:
                    err += ((simulate(logs[(st, 'wide')], k)[0] - m) / m) ** 2
            for st in ORDINARY:
                if (st, 'wide') in logs:
                    err += max(0.0, (simulate(logs[(st, 'wide')], k)[0] - 5.0) / 5.0) ** 2
            if best is None or err < best[0]:
                best = (err, v, bw)
        err, v, bw = best
        print(f'hypothesis {hyp}: {name}={v} bw={bw}  rms relative error {(err / len(MEASURED)) ** 0.5:.2f}')


def table(logs, k, detail=()):
    print(f"{'state':14s} {'view':6s} {'rdp_ms':>7s} {'meas':>5s} {'prims':>6s} {'loads':>6s} "
          f"{'loadKB':>7s} {'syncs':>6s}  clocks by category (thousands)")
    res = {}
    for (st, view), words in logs.items():
        ms, s = simulate(words, k)
        cats = sorted(s.cat.items(), key=lambda kv: -kv[1])
        cs = ' '.join(f"{c}={v / 1000:.0f}" for c, v in cats if v >= 5000)
        meas = f"{MEASURED[st]:5.0f}" if st in MEASURED and view == 'wide' else '    -'
        print(f"{st:14s} {view:6s} {ms:7.2f} {meas} {s.cnt['prims']:6.0f} {s.cnt['loads']:6.0f} "
              f"{s.cnt['load_bytes'] / 1024:7.0f} {s.cnt['syncs']:6.0f}  {cs}")
        res[f'{st}/{view}'] = dict(rdp_ms=ms, cat=dict(s.cat), cnt=dict(s.cnt))
        if st in detail:
            for kk, v in sorted(s.cnt.items()):
                print(f'    {kk:28s} {v:12.0f}')
    return res


def compare(base, new, overrides):
    print(f"{'state':14s} {'view':6s} {'meas':>5s} | {'z: before':>9s} {'after':>6s} | {'c32: before':>11s} {'after':>6s}")
    for key in base:
        if key not in new:
            continue
        st, view = key
        row = []
        for hyp in ('z', 'c32'):
            k = consts(hyp, overrides)
            row.append((simulate(base[key], k)[0], simulate(new[key], k)[0]))
        meas = f"{MEASURED[st]:5.0f}" if st in MEASURED and view == 'wide' else '    -'
        print(f"{st:14s} {view:6s} {meas} | {row[0][0]:9.1f} {row[0][1]:6.1f} | {row[1][0]:11.1f} {row[1][1]:6.1f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', nargs='?')
    ap.add_argument('--detail', action='append', default=[])
    ap.add_argument('--json')
    ap.add_argument('--hyp', default='z', choices=list(HYP))
    ap.add_argument('--fit', metavar='BASE_LOG')
    ap.add_argument('--compare', nargs=2, metavar=('BASE_LOG', 'NEW_LOG'))
    ap.add_argument('--set', action='append', default=[], help='override a constant: name=value')
    a = ap.parse_args()
    if a.fit:
        fit(parse_log(a.fit))
        return
    if a.compare:
        compare(parse_log(a.compare[0]), parse_log(a.compare[1]), a.set)
        return
    res = table(parse_log(a.log), consts(a.hyp, a.set), a.detail)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(res, f, indent=1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Search RDP blend configurations that are exact when the final pass maps the
32-bit intermediate through a TLUT (arbitrary F: v -> 5-bit result, see
final_pass_tlut() in gba_render.c), as opposed to the 2x affine final pass.

Writes one line per (EVA, EVB) for which an exact configuration exists:
    eva evb variant a1 comb C
to blend_tlut_configs.txt (consumed by gen_blend_table.py).  Slow (about an
hour); the result is checked in.

    python3 gen_blend_tlut.py > blend_tlut_configs.txt

Model: see gen_blend_table.py.  Plain colours are v = 4c and must map to c;
a configuration is exact when no intermediate value is produced by two
(a, b) pairs (or a plain colour) with different results.  tau (per-channel
remap of the first-target palette) is chosen greedily per a.
"""
import sys


def T8(t): return (t << 3) | (t >> 2)


def clamp9(v):
    v &= 0x1ff
    return v if v < 256 else (255 if v < 384 else 0)


def comb(a, b, c, d): return clamp9((((a - b) * c + (d << 8) + 0x80) & 0x1ffff) >> 8)


V = [4 * c for c in range(32)]
PLAIN = {4 * c: c for c in range(32)}


def candidates():
    seen = set()
    out = []
    for C in range(256):
        for kind in (0, 1):
            ps = tuple(comb(T8(t), 0, C, 0) if kind == 0 else comb(T8(t), 0, C, T8(t)) for t in range(32))
            if ps in seen:
                continue
            seen.add(ps)
            out.append(((kind, C), ps))
    return out


def tryconf(eva, evb, var, a1, ps):
    """returns tau (list of 32) if the configuration is exact, else None"""
    mulb = 32 - a1 if var in (0, 2) else 32
    F = dict(PLAIN)
    tau = []
    for a in range(32):
        tgt = [min(31, (a * eva + b * evb) >> 4) for b in range(32)]
        ok = None
        for t in range(32):
            P = ps[t]
            if var < 2:
                vs = [((P * a1 + V[b] * mulb) >> 5) & 255 for b in range(32)]
            else:
                vs = [((V[b] * a1 + P * mulb) >> 5) & 255 for b in range(32)]
            good = True
            newF = {}
            for v, tg in zip(vs, tgt):
                f = F.get(v, newF.get(v))
                if f is None:
                    newF[v] = tg
                elif f != tg:
                    good = False
                    break
            if good:
                ok = (t, newF)
                break
        if ok is None:
            return None
        tau.append(ok[0])
        F.update(ok[1])
    return tau


def main():
    cands = candidates()
    for eva in range(17):
        for evb in range(1, 17):
            found = None
            for var in range(4):
                for a1 in range(32):
                    for (kind, C), ps in cands:
                        if tryconf(eva, evb, var, a1, ps):
                            found = (var, a1, kind, C)
                            break
                    if found: break
                if found: break
            if found:
                print(eva, evb, *found, flush=True)
            print(f"{eva} {evb}: {'exact' if found else 'none'}", file=sys.stderr, flush=True)


if __name__ == '__main__':
    main()

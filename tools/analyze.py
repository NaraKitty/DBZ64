"""Code discovery for the GBA ROM.

Finds functions by recursive descent from known entry points, BL targets,
resolved constant branches, jump tables, scanned function pointers and
addresses reported missing by the runtime (extra_entries.txt).

Output: functions.json  {"functions": [{"addr", "thumb", "blocks":[...],
                         "insns": count}, ...], "overlays": [...]}
"""
import json
import sys
import os
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
from armdecode import Rom, CodeSpace, decode_thumb  # noqa: E402

GBA_RET = 0xFFFFFF00


class Func:
    def __init__(self, addr, thumb):
        self.addr = addr
        self.thumb = thumb
        self.insns = {}         # addr -> Insn
        self.labels = set()     # addresses that are branch targets inside
        self.calls = set()      # (target, thumb)
        self.tails = set()      # tail jumps (target, thumb)
        self.jumptables = {}    # insn addr -> [targets]
        self.const_branch = {}  # insn addr -> (target, thumb, is_call)
        self.bad = []


class Analyzer:
    def __init__(self, rom_path):
        self.rom = Rom(rom_path)
        self.cs = CodeSpace(self.rom)
        self.funcs = {}
        self.entries = set()
        self.worklist = []
        self.log = []

    def add_entry(self, addr, thumb):
        addr &= ~1
        key = (addr, thumb)
        if key in self.entries:
            return
        if not self.cs.valid(addr, 2 if thumb else 4):
            return
        self.entries.add(key)
        self.worklist.append(key)

    # ---------------------------------------------------- per-function walk
    def walk(self, addr, thumb):
        f = Func(addr, thumb)
        todo = [addr]
        f.labels.add(addr)
        seen = set()
        while todo:
            a = todo.pop()
            consts = {}  # register constants within this block
            lr_link = None
            while True:
                if a in seen:
                    break
                if not self.cs.valid(a, 2 if thumb else 4):
                    f.bad.append(a)
                    break
                seen.add(a)
                ins = self.cs.decode(a, thumb)
                f.insns[a] = ins
                nxt = ins.next
                op = ins.op
                if op in ('undef', 'blprefix', 'blh'):
                    f.bad.append(a)
                    break
                # track constants for 'ldr rX,=imm ; bx rX' patterns
                self._track_consts(ins, consts)
                uncond = ins.cond == 14
                if op == 'bl':
                    if self.cs.valid(ins.target):
                        f.calls.add((ins.target, thumb))
                        self.add_entry(ins.target, thumb)
                    a = nxt
                    consts.clear()
                    continue
                if op == 'b':
                    t = ins.target
                    if (t, thumb) in self.entries and t != addr:
                        f.tails.add((t, thumb))
                    else:
                        f.labels.add(t)
                        todo.append(t)
                    if uncond:
                        break
                    f.labels.add(nxt)
                    a = nxt
                    continue
                if op == 'bx':
                    rm = ins.rm
                    if rm == 15:
                        # thumb 'bx pc' -> ARM at aligned pc
                        t = ins.pcval & ~3 if thumb else ins.pcval
                        tt = False if thumb else False
                        f.const_branch[a] = (t, tt, False)
                        f.tails.add((t, tt))
                        self.add_entry(t, tt)
                        if uncond:
                            break
                        a = nxt
                        continue
                    if rm in consts:
                        t = consts[rm]
                        tt = bool(t & 1)
                        is_call = lr_link == nxt
                        f.const_branch[a] = (t & ~1, tt, is_call)
                        if is_call:
                            f.calls.add((t & ~1, tt))
                        else:
                            f.tails.add((t & ~1, tt))
                        self.add_entry(t, tt)
                        if is_call or not uncond:
                            f.labels.add(nxt)
                            a = nxt
                            continue
                        break
                    if lr_link == nxt:
                        # mov lr, pc ; bx rX  -> indirect call
                        f.labels.add(nxt)
                        a = nxt
                        continue
                    if uncond:
                        break
                    f.labels.add(nxt)
                    a = nxt
                    continue
                if ins.writes_pc():
                    jt = self._jumptable(f, ins, consts)
                    if jt is not None:
                        f.jumptables[a] = jt
                        for t in jt:
                            f.labels.add(t)
                            todo.append(t)
                        if uncond:
                            break
                    # ldr pc, =const
                    elif op == 'mem' and ins.rn == 15 and ins.off[0] == 'imm' and ins.pre:
                        la = ins.pcval + (ins.off[1] if ins.up else -ins.off[1])
                        if self.cs.valid(la, 4):
                            t = self.cs.u32(la)
                            # ARMv4: ldr pc does not change state
                            tt = thumb
                            is_call = lr_link == nxt
                            f.const_branch[a] = (t & ~1, tt, is_call)
                            (f.calls if is_call else f.tails).add((t & ~1, tt))
                            self.add_entry(t & ~1, tt)
                            if is_call:
                                f.labels.add(nxt)
                                a = nxt
                                continue
                        if uncond:
                            break
                    elif uncond:
                        if lr_link == nxt:
                            f.labels.add(nxt)
                            a = nxt
                            continue
                        break
                    f.labels.add(nxt)
                    a = nxt
                    continue
                # 'mov lr, pc' sets lr to the address after the next insn,
                # so only the immediately following transfer sees nxt == lr_link
                if (op == 'dp' and ins.rd == 14 and ins.alu == 'mov' and
                        ins.op2[0] == 'reg' and ins.op2[1] == 15 and ins.op2[3] == 0):
                    lr_link = ins.pcval
                a = nxt
        return f

    def _track_consts(self, ins, consts):
        op = ins.op
        if op == 'mem' and ins.load and ins.rn == 15 and ins.off[0] == 'imm' and ins.msize == 4 and ins.pre:
            la = ins.pcval + (ins.off[1] if ins.up else -ins.off[1])
            if self.cs.valid(la, 4):
                consts[ins.rd] = self.cs.u32(la)
                return
        if op == 'dp' and ins.alu == 'mov' and ins.op2[0] == 'imm':
            consts[ins.rd] = ins.op2[1]
            return
        if op == 'dp' and ins.alu == 'add' and ins.rn == 15 and ins.op2[0] == 'imm':
            consts[ins.rd] = (ins.pcval + ins.op2[1]) & 0xffffffff
            return
        if op == 'dp' and ins.alu == 'add' and ins.rd in consts and ins.op2[0] == 'imm' and ins.rn == ins.rd:
            consts[ins.rd] = (consts[ins.rd] + ins.op2[1]) & 0xffffffff
            return
        # anything that writes a register kills it
        for r in written_regs(ins):
            consts.pop(r, None)

    def _jumptable(self, f, ins, consts):
        """Recognise switch tables; returns list of targets or None."""
        thumb = f.thumb
        a = ins.addr
        # gather previous few instructions linearly
        prev = []
        p = a
        for _ in range(8):
            p -= 2 if thumb else 4
            if p not in f.insns:
                break
            prev.append(f.insns[p])
        table = None
        bound = None
        if thumb and ins.op == 'dp' and ins.alu == 'mov':
            # mov pc, rX ; preceded by ldr rX,[rX] ; add rX, rY ; ldr rY,=table
            for q in prev:
                if q.op == 'mem' and q.rn == 15 and q.load and q.msize == 4:
                    la = q.pcval + q.off[1]
                    table = self.cs.u32(la)
                    break
            for q in prev:
                if q.op == 'dp' and q.alu == 'cmp' and q.op2[0] == 'imm':
                    bound = q.op2[1] + 1
                    break
            # look a little further back for the cmp
            if bound is None:
                p2 = a
                for _ in range(16):
                    p2 -= 2
                    q = f.insns.get(p2)
                    if q and q.op == 'dp' and q.alu == 'cmp' and q.op2[0] == 'imm':
                        bound = q.op2[1] + 1
                        break
        elif not thumb:
            # ARM: ldrls pc, [pc, rX, lsl #2]  (table follows) or addls pc, pc, rX lsl #2
            if ins.op == 'mem' and ins.rn == 15 and ins.off[0] == 'reg':
                table = None
                base = ins.pcval
                for q in prev:
                    if q.op == 'dp' and q.alu == 'cmp' and q.op2[0] == 'imm':
                        bound = q.op2[1] + 1
                        break
                if bound:
                    return [self.cs.u32(base + 4 * k) & ~1 for k in range(bound)
                            if self.cs.valid(base + 4 * k, 4)]
                return None
            if ins.op == 'dp' and ins.alu == 'add' and ins.rn == 15 and ins.op2[0] == 'reg':
                for q in prev:
                    if q.op == 'dp' and q.alu == 'cmp' and q.op2[0] == 'imm':
                        bound = q.op2[1] + 1
                        break
                if bound:
                    return [ins.pcval + 4 * k for k in range(bound)]
                return None
            if ins.op == 'mem' and ins.load:
                for q in prev:
                    if q.op == 'mem' and q.rn == 15 and q.load and q.msize == 4:
                        table = self.cs.u32(q.pcval + q.off[1])
                        break
                for q in prev:
                    if q.op == 'dp' and q.alu == 'cmp' and q.op2[0] == 'imm':
                        bound = q.op2[1] + 1
                        break
        if table is None or bound is None or bound > 1024:
            if table is not None and bound is None:
                self.log.append('jumptable without bound at %08x' % a)
            return None
        out = []
        for k in range(bound):
            if not self.cs.valid(table + 4 * k, 4):
                break
            t = self.cs.u32(table + 4 * k) & ~1
            if not self.cs.valid(t):
                self.log.append('jumptable %08x: bad entry %08x' % (a, t))
                return None
            out.append(t)
        return out

    # ---------------------------------------------------- driver
    def run(self):
        while self.worklist:
            addr, thumb = self.worklist.pop()
            f = self.walk(addr, thumb)
            self.funcs[(addr, thumb)] = f
        # a later-discovered entry may turn an internal branch into a tail
        # call; that is still correct code, so no re-walk is needed.

    def scan_pointers(self, lo, hi):
        """Find Thumb function pointers stored in ROM data."""
        found = 0
        known_calls = {t for (t, th) in self.entries if th}
        data = self.rom.data
        for off in range(0, len(data) - 3, 4):
            v = int.from_bytes(data[off:off + 4], 'little')
            if not (v & 1) or not (lo <= v < hi):
                continue
            t = v & ~1
            if (t, True) in self.entries:
                continue
            h = self.cs.u16(t)
            # push {..., lr} prologue
            if (h & 0xff00) == 0xb500 or t in known_calls:
                self.add_entry(t, True)
                found += 1
        return found


WRITE_CACHE = {}


def written_regs(ins):
    op = ins.op
    if op == 'dp':
        if ins.alu in ('tst', 'teq', 'cmp', 'cmn'):
            return ()
        return (ins.rd,)
    if op == 'mem':
        r = []
        if ins.load:
            r.append(ins.rd)
        if ins.wb:
            r.append(ins.rn)
        return r
    if op == 'ldm':
        r = list(ins.regs) if ins.load else []
        if ins.wb:
            r.append(ins.rn)
        return r
    if op in ('mul',):
        return (ins.rd,)
    if op == 'mull':
        return (ins.rd, ins.rdhi)
    if op in ('mrs', 'swp'):
        return (ins.rd,)
    if op == 'bl':
        return (14,)
    return ()


def main():
    rom_path = sys.argv[1]
    out = sys.argv[2]
    extra = sys.argv[3] if len(sys.argv) > 3 else None
    an = Analyzer(rom_path)
    # crt0 copies ROM 0x087fd6bc..+0xe7c to IWRAM 0x03000000
    an.cs.add_overlay(0x03000000, 0xe7c, 0x087fd6bc)
    an.add_entry(0x08000000, False)
    an.add_entry(0x08021d74, True)   # AgbMain
    an.add_entry(0x08000158, True)   # return target of main
    if extra and os.path.exists(extra):
        for line in open(extra):
            line = line.split('#')[0].strip()
            if not line:
                continue
            a, mode = line.split()[:2]
            an.add_entry(int(a, 16), mode.upper() == 'T')
    an.run()
    n = an.scan_pointers(0x08000000, 0x08000000 + len(an.rom.data))
    an.run()
    print('pointer-scan entries:', n)

    funcs = []
    total = 0
    bad = 0
    for (a, th), f in sorted(an.funcs.items()):
        total += len(f.insns)
        bad += len(f.bad)
        funcs.append({
            'addr': a, 'thumb': th,
            'labels': sorted(f.labels),
            'insns': sorted(f.insns),
            'jumptables': {str(k): v for k, v in f.jumptables.items()},
            'const_branch': {str(k): list(v) for k, v in f.const_branch.items()},
            'bad': f.bad,
        })
    uniq = set()
    for f in an.funcs.values():
        uniq.update(f.insns)
    lo = min(a for a in uniq if a >= 0x08000000)
    hi = max(a for a in uniq)
    print('functions: %d  insns (with dup): %d  unique insns: %d  bad: %d' % (
        len(funcs), total, len(uniq), bad))
    print('code range %08x - %08x' % (lo, hi))
    json.dump({'functions': funcs, 'overlays': an.cs.overlays}, open(out, 'w'))
    with open(out + '.log', 'w') as fh:
        fh.write('\n'.join(an.log))
        for f in funcs:
            if f['bad']:
                fh.write('\nbad in %08x: %s' % (f['addr'], ' '.join('%08x' % b for b in f['bad'][:8])))


if __name__ == '__main__':
    main()

"""Which ROM words are 68000 code, from the tracer's coverage and a walk from it.

The tracer (md.exe --cover FILE) lists the address of every instruction the interpreter
ran.  Discovery decodes each (m68k.py) and walks on from it: the instruction after one
that falls through, the target of a branch, a BSR, a DBcc, a JMP or JSR to a fixed
address; it stops at an instruction that ends the flow (RTS, RTE, JMP through a
register...), at an undefined opcode, and at the ROM's end.  The walk may take data for
code -- that costs only C++, never correctness: the recompiled code runs an instruction
with the interpreter's own handler, so a word wrongly called code does, if ever run,
what the interpreter would.

A Program is the ROM's code as {address: Insn}, with how each was found.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit import m68k  # noqa: E402

ROM_END = 0x400000


class Program:
    def __init__(self, data):
        self.data = data
        self.size = min(len(data), ROM_END)
        self.read16 = m68k.reader(data)
        self.code = {}          # address -> Insn
        self.how = {}           # address -> 'trace' | 'walk' | 'vector'

    def ok(self, a):
        return 0 <= a < self.size - 1 and not (a & 1)

    def add(self, roots, how):
        todo = [a for a in roots if self.ok(a) and a not in self.code]
        for a in todo:
            self.how.setdefault(a, how)
        while todo:
            a = todo.pop()
            if a in self.code or not self.ok(a):
                continue
            i = m68k.decode(self.read16, a)
            if i.next > self.size or i.next <= a:
                continue
            self.code[a] = i
            self.how.setdefault(a, 'walk')
            if i.kind == 'bad':
                continue
            nxt = []
            if not i.ends:
                nxt.append(i.next)
            if i.target is not None and i.mnem not in ('trap',) and i.form not in ('trap',):
                nxt.append(i.target)
            for t in nxt:
                if self.ok(t) and t not in self.code:
                    todo.append(t)

    def vectors(self):
        """The reset PC and the interrupt and exception vectors that point into the ROM."""
        out = []
        for v in range(1, 64):
            a = int.from_bytes(self.data[v * 4:v * 4 + 4], 'big') & 0xFFFFFF
            if self.ok(a):
                out.append(a)
        return out


def read_cover(paths):
    out = set()
    for p in paths:
        with open(p) as f:
            for ln in f:
                ln = ln.strip()
                if ln:
                    out.add(int(ln, 16))
    return out


def discover(data, covers, walk=True):
    prog = Program(data)
    traced = sorted(a for a in read_cover(covers) if a < prog.size)
    if walk:
        prog.add(traced, 'trace')
        prog.add(prog.vectors(), 'vector')
    else:
        for a in traced:
            if prog.ok(a):
                prog.code[a] = m68k.decode(prog.read16, a)
                prog.how[a] = 'trace'
    return prog


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--cover', action='append', default=[])
    ap.add_argument('--no-walk', action='store_true')
    a = ap.parse_args(argv)
    data = open(a.rom, 'rb').read()
    p = discover(data, a.cover, not a.no_walk)
    from collections import Counter
    print(f'{len(p.code)} instructions, {sum(i.length for i in p.code.values())} bytes:',
          dict(Counter(p.how[x] for x in p.code)))


if __name__ == '__main__':
    main()

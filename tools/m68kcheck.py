"""Hold mdkit's 68000 decoder to MAME's disassembler over every opcode word.

An image of 65536 slots of 16 bytes: slot n is opcode word n, then filler
words.  The filler is a MOVEQ (one word, so a disassembler that has taken
fewer extension words than the slot holds comes back in step at the next
slot) whose bits also make a valid brief extension word on a 68000 (a D
register index, no scale), a displacement, an absolute address and an
immediate.  unidasm (`-arch m68000`) disassembles the image; each slot's
first line must be what m68k.decode() makes of it: the same length and the
same text (the spacing aside).

    python tools/m68kcheck.py --unidasm PATH/unidasm.exe [--filler 7012] [--show 20]

Known differences, by design -- the decoder follows the 68000, where MAME's
disassembler (one table for the whole family) shows another CPU's
instruction:

    $Fxxx          line F on a 68000; MAME shows the 68851/68030 MMU and
                   FPU instructions (pmove, fmove...) for some of them
    $6xFF          Bcc/BRA/BSR with the 8-bit displacement -1 (to an odd
                   address: an address error); MAME shows the 68020's
                   Bcc.L as illegal
    $4AC8-$4ACF    TAS An, illegal on a 68000; MAME shows CPU32's HALT
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit import m68k  # noqa: E402

SLOT = 16


def known(op, mine, theirs):
    """A difference listed above: the 68000 and MAME's family-wide disassembler disagree."""
    if op >> 12 == 0xF:
        return mine[1].endswith('opcode 1111')
    if op & 0xF0FF == 0x60FF:
        return theirs[1].endswith('ILLEGAL')
    if 0x4AC8 <= op <= 0x4ACF:
        return mine[1].endswith('ILLEGAL')
    return False


def image(filler):
    out = bytearray()
    for op in range(0x10000):
        out += op.to_bytes(2, 'big') + filler.to_bytes(2, 'big') * (SLOT // 2 - 1)
    return bytes(out)


def norm(t):
    t = t.strip()
    parts = t.split(None, 1)
    return parts[0] + (' ' + re.sub(r'\s*,\s*', ', ', parts[1].strip()) if len(parts) > 1 else '')


def run(unidasm, filler, show):
    img = image(filler)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'ops.bin')
        with open(path, 'wb') as f:
            f.write(img)
        out = subprocess.run([unidasm, path, '-arch', 'm68000', '-basepc', '0'], capture_output=True,
                             text=True, check=True).stdout
    lines = {}
    for ln in out.splitlines():
        m = re.match(r'\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{4}\s)+)\s*(.*)$', ln)
        if m:
            a = int(m.group(1), 16)
            lines[a] = (len(m.group(2).split()) * 2, m.group(3))
    read16 = m68k.reader(img)
    bad = defaultdict(list)
    kn = Counter()
    n = 0
    for op in range(0x10000):
        a = op * SLOT
        ref = lines.get(a)
        i = m68k.decode(read16, a)
        mine = (i.length, norm(i.text()))
        theirs = (ref[0], norm(ref[1])) if ref else (None, None)
        n += 1
        if mine != theirs:
            if known(op, mine, theirs):
                kn[i.form] += 1
            else:
                bad[i.form].append((op, mine, theirs))
    total = sum(len(v) for v in bad.values())
    print(f'filler ${filler:04X}: {n} opcode words, {n - total - sum(kn.values())} identical, '
          f'{sum(kn.values())} known differences ({", ".join(f"{k} {v}" for k, v in kn.most_common())}), '
          f'{total} differ')
    for form, v in sorted(bad.items(), key=lambda kv: -len(kv[1])):
        print(f'  {form:14} {len(v):6}')
        for op, mine, theirs in v[:show]:
            print(f'      {op:04X}  mine {mine[0]} {mine[1]!r:44}  MAME {theirs[0]} {theirs[1]!r}')
    return total


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--unidasm', required=True)
    ap.add_argument('--filler', action='append', type=lambda s: int(s, 16))
    ap.add_argument('--show', type=int, default=4)
    a = ap.parse_args(argv)
    fails = sum(run(a.unidasm, f, a.show) for f in (a.filler or [0x7012, 0x78F0]))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

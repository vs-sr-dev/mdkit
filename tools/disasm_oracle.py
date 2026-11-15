"""Hold discovery to a disassembly project's data: what it includes as binary is not code.

The Sonic Retro disassemblies (s1disasm, s2disasm, skdisasm) build a ROM identical to the
dump from their sources, and keep the art, mappings, layouts, palettes and sound data as
binary files they include.  Each such file found in the ROM (byte for byte, at a word
boundary) marks a range of data.  Nothing of the disassembly is run, only read.

The report: the ROM's share the data files cover, and the discovered instructions that
start in it -- each one a word discovery took for code (from the trace: a real mistake
of the oracle or of the runtime; from the walk: a walk into data, harmless but wasted).

    python tools/disasm_oracle.py ROM DISASM_DIR --cover FILE [--cover FILE...]
"""
import argparse
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit.recomp.discover import discover  # noqa: E402

SKIP_DIRS = {'.git', 'build_tools', 'Utility Project Files'}
SKIP_EXT = {'.asm', '.md', '.txt', '.lua', '.bat', '.sh', '.ini', '.exe', '.dll', '.py', '.json', '.yml', '.gitattributes'}


def data_ranges(rom, root, min_size=24):
    found, missing = [], []
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for f in files:
            p = os.path.join(d, f)
            if os.path.splitext(f)[1].lower() in SKIP_EXT or f.startswith('.'):
                continue
            b = open(p, 'rb').read()
            if len(b) < min_size:
                continue
            at = rom.find(b)
            hits = []
            while at >= 0:
                if not at & 1:
                    hits.append(at)
                at = rom.find(b, at + 1)
            if hits:
                for h in hits:
                    found.append((h, h + len(b), os.path.relpath(p, root)))
            else:
                missing.append(os.path.relpath(p, root))
    return sorted(found), missing


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('disasm')
    ap.add_argument('--cover', action='append', default=[])
    a = ap.parse_args(argv)
    rom = open(a.rom, 'rb').read()
    ranges, missing = data_ranges(rom, a.disasm)
    is_data = bytearray(len(rom))
    for lo, hi, _ in ranges:
        is_data[lo:hi] = b'\1' * (hi - lo)
    print(f'{len(ranges)} data files found in the ROM ({len(missing)} not: other revisions, or built), '
          f'{sum(is_data)} bytes of {len(rom)} ({100 * sum(is_data) / len(rom):.1f}%)')
    prog = discover(rom, a.cover)
    bad = [x for x in prog.code if is_data[x]]
    print(f'{len(prog.code)} instructions discovered; {len(bad)} start in data:',
          dict(Counter(prog.how[x] for x in bad)))
    where = Counter()
    for x in bad:
        for lo, hi, name in ranges:
            if lo <= x < hi:
                where[(prog.how[x], name)] += 1
                break
    for (how, name), n in where.most_common(15):
        print(f'  {n:5} {how:6} {name}')


if __name__ == '__main__':
    sys.exit(main())

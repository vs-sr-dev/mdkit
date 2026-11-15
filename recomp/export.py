"""What discovery and the traces know of a cartridge, as JSON: the input of the Mega Drive to
Atari Jaguar side (the same 68000; another memory map, no VDP, no YM2612).

    python -m mdkit.recomp.export ROM --cover FILE [--out FILE.json]

The tracer (md.exe --cover FILE) writes FILE (the instructions run) and FILE.sites (each
instruction that reached the hardware, and which: two bits a class, read and write, in the
order of md.h's IoClass).  The JSON:

- "cart": the list's name, title, machine, slot, size, CRC32;
- "code": the instructions' count and bytes, how each was found (trace, walk, vector), and
  the runs of consecutive instructions as [first, end) address pairs;
- "sites": every instruction the trace saw reach the hardware -- its address, its text, the
  addressing mode of each operand (abs.w, abs.l, (An), d16(An), indexed, PC-relative...)
  and the classes it read and wrote.  On the Jaguar each needs its address rewritten: an
  absolute operand in place, a register's value where it is loaded;
- "absolute": the instructions (all discovered code) with an absolute operand, by the area
  it names: work RAM ($FF0000, and its mirrors from $E00000), the VDP, the Z80 side, I/O...
  -- the Jaguar's RAM sits at $000000 and its ROM at $800000;
- "indirect": JMP and JSR through a register, and the jump tables reached by
  (d8,PC,Xn): the places a translator must resolve or keep a lookup for;
- "special": TAS, MOVEP, STOP, RESET, TRAP, illegal and line-A/F words among the code.
"""
import argparse
import json
import os
import sys
import zlib
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit.cart import Cart  # noqa: E402
from mdkit.recomp.discover import discover  # noqa: E402

CLASSES = ['cart', 'z80ram', 'ym2612', 'version', 'pad', 'z80bus', 'mapper', 'tmss',
           'vdp_data', 'vdp_ctrl', 'hv_counter', 'psg', 'other']


def area(a):
    """The area of the Mega Drive's map an address names."""
    a &= 0xFFFFFF
    if a < 0x400000:
        return 'rom'
    if a >= 0xE00000:
        return 'work_ram'
    if 0xA00000 <= a < 0xA10000:
        return 'z80'
    if 0xA10000 <= a < 0xA20000:
        return 'io'
    if 0xA13000 <= a < 0xA14000:
        return 'mapper'
    if 0xC00000 <= a < 0xE00000:
        return 'vdp'
    return 'other'


def read_sites(path):
    out = {}
    if os.path.exists(path):
        for ln in open(path):
            p = ln.split()
            if len(p) == 2:
                out[int(p[0], 16)] = int(p[1], 16)
    return out


def export(rom_path, covers):
    c = Cart.load(rom_path)
    data = c.data
    prog = discover(data, covers)
    sites = {}
    for cv in covers:
        for a, m in read_sites(cv + '.sites').items():
            sites[a] = sites.get(a, 0) | m
    runs = []
    for a in sorted(prog.code):
        i = prog.code[a]
        if runs and runs[-1][1] == a:
            runs[-1][1] = i.next
        else:
            runs.append([a, i.next])
    out_sites = []
    for a in sorted(sites):
        m = sites[a]
        i = prog.code.get(a)
        out_sites.append(dict(
            pc=a, text=i.text() if i else None,
            modes=[o.kind for o in i.ops if hasattr(o, 'kind')] if i else [],
            reads=[CLASSES[k] for k in range(len(CLASSES)) if m >> (2 * k) & 1],
            writes=[CLASSES[k] for k in range(len(CLASSES)) if m >> (2 * k + 1) & 1]))
    absolute = defaultdict(Counter)
    indirect, special = [], []
    for a in sorted(prog.code):
        i = prog.code[a]
        for o in i.ops:
            if hasattr(o, 'kind') and o.kind in ('aw', 'al'):
                absolute[area(o.addr)][o.kind] += 1
            if hasattr(o, 'kind') and o.kind == 'pcix' and i.mnem in ('jmp', 'jsr', 'lea', 'move', 'movea'):
                indirect.append(dict(pc=a, text=i.text(), kind='pc_indexed'))
        if i.kind in ('jumpind', 'callind'):
            indirect.append(dict(pc=a, text=i.text(), kind=i.kind))
        if i.mnem in ('tas', 'movep', 'stop', 'reset', 'trap') or i.kind == 'bad':
            special.append(dict(pc=a, text=i.text()))
    return dict(
        cart=dict(key=c.key, title=c.title, machine=c.machine, slot=c.slot, size=c.size,
                  crc32=f'{zlib.crc32(data):08X}'),
        code=dict(instructions=len(prog.code), bytes=sum(i.length for i in prog.code.values()),
                  how=dict(Counter(prog.how[a] for a in prog.code)), runs=runs),
        sites=out_sites,
        absolute={k: dict(v) for k, v in sorted(absolute.items())},
        indirect=indirect,
        special=special)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--cover', action='append', default=[])
    ap.add_argument('--out')
    a = ap.parse_args(argv)
    j = export(a.rom, a.cover)
    text = json.dumps(j, indent=1)
    if a.out:
        with open(a.out, 'w', encoding='utf8') as f:
            f.write(text)
        print(f"{a.out}: {j['code']['instructions']} instructions, {len(j['sites'])} access sites, "
              f"{len(j['indirect'])} indirect, absolute {j['absolute']}")
    else:
        print(text)


if __name__ == '__main__':
    sys.exit(main())

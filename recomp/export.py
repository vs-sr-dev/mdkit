"""What discovery and the traces know of a cartridge, as JSON: the input of the Mega Drive to
Atari Jaguar side (the same 68000; another memory map, no VDP, no YM2612).

    python -m mdkit.recomp.export ROM --cover FILE [--cover FILE...] [--out FILE.json]

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
- "special": TAS, MOVEP, STOP, RESET, TRAP, illegal and line-A/F words among the code;
- "ram_pointers": from FILE.taint (build/md_taint.exe, runtime/md_taint.h): every word or long
  read from the ROM's data whose value the run used as an address in work RAM -- the base or
  index of an access there ("ram_base"), compared with such an address ("ram_cmp"), or put
  in an address register while it names the work RAM ("to_an") -- with the ROM address read,
  the size, the PC that read it, the value, how many accesses it formed and the first PC
  that formed one ("use_pc");
- "immediates": the same for the instructions' own words: the immediate, absolute address,
  displacement or MOVEQ value ("operand": imm, abs, disp, quick) of the instruction at "pc";
- "an_loads": the fixups' sites -- every instruction that loaded an address register (MOVEA,
  MOVEM, EXG, LEA, from the ROM, an immediate, a data register or the RAM; a copy between
  address registers keeps the load it copies, an offset or an increment the register's own)
  whose value then formed an address in work RAM: its PC and text, the register, how many
  accesses, the first that formed one, and the origin of the value when one is known (the ROM
  word or the immediate, as in ram_pointers and immediates; none: made from no ROM word);
- "unattributed": PCs that formed a work RAM address from a register no ROM word reached, and
  the PC that last wrote that register ("written_at"; $FFFFFF: an exception's entry);
- "ram_code": the instructions the runs ran from the work RAM (code copied there; jsr $c500.w
  runs $FFC500): their count and their PCs in groups, [first, last] with no gap over 16 bytes;
- "z80_code": the Z80's code the runs saw it run from its RAM (FILE.z80): the bytes, their
  CRC32 (the driver's fingerprint, code only: its variables and the music data are not in it)
  and the runs of consecutive bytes as [first, hex bytes];
- "runs": the tracer's runs the coverage is the union of (tools/cover.py), each with its
  trace in runs/NAME.cov beside the merged one.
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


FLAGS = ((1, 'to_an'), (2, 'ram_base'), (4, 'ram_cmp'), (8, 'other_base'))


def in_ram(v):
    """A work RAM address, its top byte $00 or $FF (md_taint.cpp's rule)."""
    return (v & 0xFFFFFF) >= 0xE00000 and v >> 24 in (0, 0xFF)


def read_taint(paths):
    """The origins of one or more .taint files, merged: (addr, size, pc) -> [value, flags, uses, use_pc]."""
    origins, unattr, loads = {}, {}, {}
    for p in paths:
        if not os.path.exists(p):
            continue
        for ln in open(p):
            w = ln.split()
            if not w or w[0] == '#':
                continue
            if w[0] == 'O':
                o = origins.setdefault((int(w[1], 16), int(w[2]), int(w[3], 16)), [int(w[4], 16), 0, 0, 0])
                o[1] |= int(w[5], 16)
                o[2] += int(w[6])
                if not o[3] and int(w[6]):
                    o[3] = int(w[7], 16)
            elif w[0] == 'U':
                u = unattr.setdefault(int(w[1], 16), [0, int(w[3], 16) if len(w) > 3 else None])
                u[0] += int(w[2])
            elif w[0] == 'A':
                key = (int(w[1], 16), int(w[2]))
                ld = loads.setdefault(key, [0, int(w[4], 16), None])
                ld[0] += int(w[3])
                if ld[2] is None and int(w[6]):
                    ld[2] = (int(w[5], 16), int(w[6]), int(w[7], 16))
    return origins, unattr, loads


def operand_at(i, addr, size):
    """Which of instruction i's operands holds the word at addr: imm, abs, disp (or None)."""
    if addr == i.addr and size == 1 and i.mnem == 'moveq':
        return 'quick'
    for o in i.ops:
        at = getattr(o, 'ext_at', None)
        if at is None:
            continue
        k = o.kind
        n = 4 if (k == 'al' or (k == 'imm' and o.size == 'l')) else 2
        if at <= addr < at + n:
            return 'imm' if k == 'imm' else 'abs' if k in ('aw', 'al') else 'disp'
    return None


def pointers(data, prog, covers):
    """ram_pointers, immediates and unattributed from the covers' .taint files."""
    from mdkit import m68k
    origins, unattr, loads = read_taint([cv + '.taint' for cv in covers])
    read16 = m68k.reader(data)
    ram, imm = [], []
    for (addr, size, pc), (value, flags, uses, use_pc) in sorted(origins.items()):
        # a pointer: its value, as the 68000 extends a word or MOVEQ's byte, is a work RAM address
        # (an origin that only went into one -- an offset, a 0 -- is not)
        bits = 8 * size
        wide = value - (1 << bits) if size < 4 and value >> (bits - 1) & 1 else value
        if not (flags & 7 and in_ram(wide & 0xFFFFFFFF)):
            continue
        names = [n for b, n in FLAGS if flags & b]
        i = prog.code.get(pc) or m68k.decode(read16, pc)
        rec = dict(rom=addr, size=size, pc=pc, value=value, flags=names, uses=uses, use_pc=use_pc or None)
        operand = operand_at(i, addr, size) if pc <= addr < pc + i.length else None
        if operand:
            rec['operand'] = operand
            rec['text'] = i.text()
            imm.append(rec)
        else:
            ram.append(rec)
    un = []
    for pc, (n, at) in sorted(unattr.items()):
        i, w = prog.code.get(pc), prog.code.get(at)
        un.append(dict(pc=pc, text=i.text() if i else None, count=n, written_at=at,
                       written_text=w.text() if w else None))
    an = []
    for (pc, reg), (n, use_pc, org) in sorted(loads.items()):
        i = prog.code.get(pc)
        an.append(dict(pc=pc, text=i.text() if i else None, reg=f'A{reg}', uses=n, use_pc=use_pc,
                       origin=dict(rom=org[0], size=org[1], pc=org[2]) if org else None))
    return ram, imm, un, an


def z80_code(covers):
    """The Z80 RAM bytes run as code (FILE.z80), merged: count, CRC32, runs of consecutive bytes."""
    code = {}
    for cv in covers:
        if os.path.exists(cv + '.z80'):
            for ln in open(cv + '.z80'):
                a, b = ln.split()
                code.setdefault(int(a, 16), int(b, 16))
    runs = []
    for a in sorted(code):
        if runs and runs[-1][0] + len(runs[-1][1]) == a:
            runs[-1][1].append(code[a])
        else:
            runs.append([a, [code[a]]])
    blob = b''.join(a.to_bytes(2, 'big') + bytes(bs) for a, bs in runs)
    return dict(bytes=len(code), crc32=f'{zlib.crc32(blob):08X}' if code else None,
                runs=[[a, bytes(bs).hex()] for a, bs in runs])


def runs_of(covers):
    """The runs (tools/cover.py's runs.json) beside a merged trace."""
    out = []
    for cv in covers:
        p = os.path.join(os.path.dirname(os.path.abspath(cv)), 'runs.json')
        if os.path.exists(p):
            for r in json.load(open(p, encoding='utf8'))['runs']:
                if r.get('ok'):
                    out.append(dict(name=r['name'], args=r['args'], frames=r.get('frames'),
                                    instructions_run=r.get('instructions_run'), cover=f"runs/{r['name']}.cov"))
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
    ram_pointers, immediates, unattributed, an_loads = pointers(data, prog, covers)
    run_pcs = set()
    for cv in covers:
        if os.path.exists(cv):
            run_pcs |= {int(x, 16) for x in open(cv).read().split()}
    ram_groups = []
    for a in sorted(p for p in run_pcs if p >= 0xE00000):
        if ram_groups and a - ram_groups[-1][1] <= 16:
            ram_groups[-1][1] = a
        else:
            ram_groups.append([a, a])
    return dict(
        cart=dict(key=c.key, title=c.title, machine=c.machine, slot=c.slot, size=c.size,
                  crc32=f'{zlib.crc32(data):08X}'),
        runs=runs_of(covers),
        instructions_run=len(run_pcs),
        code=dict(instructions=len(prog.code), bytes=sum(i.length for i in prog.code.values()),
                  how=dict(Counter(prog.how[a] for a in prog.code)), runs=runs),
        sites=out_sites,
        absolute={k: dict(v) for k, v in sorted(absolute.items())},
        indirect=indirect,
        special=special,
        ram_pointers=ram_pointers,
        immediates=immediates,
        an_loads=an_loads,
        unattributed=unattributed,
        ram_code=dict(instructions=sum(1 for p in run_pcs if p >= 0xE00000), pcs=ram_groups),
        z80_code=z80_code(covers))


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
        print(f"{a.out}: {j['code']['instructions']} instructions ({j['instructions_run']} run), "
              f"{len(j['sites'])} access sites, {len(j['indirect'])} indirect, absolute {j['absolute']}, "
              f"{len(j['ram_pointers'])} RAM pointers in data, {len(j['immediates'])} in instructions, "
              f"{len(j['unattributed'])} unattributed")
    else:
        print(text)


if __name__ == '__main__':
    sys.exit(main())

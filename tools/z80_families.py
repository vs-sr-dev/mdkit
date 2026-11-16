"""The Z80 sound drivers of the library, by their code alone.

The tracer (md.exe --cover FILE) writes FILE.z80: each byte of the Z80's RAM it ran as code,
with its value then -- the driver's code, without its variables, its tables or the music it
was handed.  Two carts whose code is byte for byte the same run the same build of a driver
(the "exact" groups, by CRC32); builds of one driver differ by a few routines, a relocation,
a table in between, so the families gather carts whose code shares most of its 8-byte
sequences (single linkage, Jaccard >= THRESHOLD over the two carts' sequences, address-free).

    python tools/z80_families.py [--recomp build/recomp] [--out build/survey] [--threshold 0.5] [--write-exports]

build/survey/z80_families.csv: a cart a line (key, title, the code's bytes and CRC32, the exact
group, the family, its size and the carts that name it); z80_families.txt: the families, the
largest first.  --write-exports adds "family" (and its size and namesakes) to each export.json's
z80_code.
"""
import argparse
import csv
import json
import os
import sys
import zlib
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
K = 8


def load(path):
    code = {}
    for ln in open(path):
        a, b = ln.split()
        code[int(a, 16)] = int(b, 16)
    return code


def shingles(code):
    """The K-byte sequences of the code's runs of consecutive bytes (no addresses)."""
    out, run, last = set(), [], None
    for a in sorted(code):
        if last is not None and a != last + 1:
            out |= {bytes(run[i:i + K]) for i in range(len(run) - K + 1)}
            run = []
        run.append(code[a])
        last = a
    out |= {bytes(run[i:i + K]) for i in range(len(run) - K + 1)}
    return out


def crc(code):
    """export.json's z80_code CRC32: the runs of consecutive bytes, each its address (2 bytes) and bytes."""
    if not code:
        return ''
    blob, last = bytearray(), None
    for a in sorted(code):
        if last is None or a != last + 1:
            blob += a.to_bytes(2, 'big')
        blob.append(code[a])
        last = a
    return f'{zlib.crc32(bytes(blob)):08X}'


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--recomp', default=os.path.join(HERE, 'build', 'recomp'))
    ap.add_argument('--out', default=os.path.join(HERE, 'build', 'survey'))
    ap.add_argument('--threshold', type=float, default=0.5)
    ap.add_argument('--min-bytes', type=int, default=64, help='less code than this is no driver (a DAC loop, a halt)')
    ap.add_argument('--write-exports', action='store_true')
    a = ap.parse_args(argv)
    carts = {}
    for key in sorted(os.listdir(a.recomp)):
        p = os.path.join(a.recomp, key, 'trace.cov.z80')
        ex = os.path.join(a.recomp, key, 'export.json')
        if not os.path.exists(p):
            continue
        try:
            title = json.load(open(ex, encoding='utf8'))['cart']['title']
            code = load(p)
        except (OSError, ValueError, KeyError):     # not written yet
            continue
        carts[key] = dict(title=title, code=code, crc=crc(code), sh=shingles(code) if len(code) >= a.min_bytes else set())
    keys = sorted(carts)
    # exact groups
    by_crc = defaultdict(list)
    for k in keys:
        if carts[k]['code']:
            by_crc[carts[k]['crc']].append(k)
    # families: an inverted index of the sequences, then single linkage over the pairs that share enough
    parent = {k: k for k in keys}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    index = defaultdict(list)
    for k in keys:
        for s in carts[k]['sh']:
            index[s].append(k)
    for k in keys:
        sk = carts[k]['sh']
        if not sk:
            continue
        common = Counter()
        for s in sk:
            for o in index[s]:
                if o > k:
                    common[o] += 1
        for o, n in common.items():
            if n / (len(sk) + len(carts[o]['sh']) - n) >= a.threshold:
                parent[find(o)] = find(k)
    fam = defaultdict(list)
    for k in keys:
        if carts[k]['sh']:
            fam[find(k)].append(k)
    ordered = sorted(fam.values(), key=lambda v: (-len(v), v[0]))
    fid = {}
    for n, members in enumerate(ordered, 1):
        for k in members:
            fid[k] = n

    def namesakes(members, n=3):
        # the carts that name a family: its largest exact group's, then the others
        groups = Counter(carts[k]['crc'] for k in members)
        top = sorted(members, key=lambda k: (-groups[carts[k]['crc']], k))
        return [carts[k]['title'] for k in top[:n]]
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for k in keys:
        c = carts[k]
        f = fid.get(k)
        members = ordered[f - 1] if f else []
        rows.append(dict(key=k, title=c['title'], z80_bytes=len(c['code']), z80_crc32=c['crc'],
                         exact_group_size=len(by_crc.get(c['crc'], [])) if c['code'] else 0,
                         family=f or '', family_size=len(members),
                         family_names='; '.join(namesakes(members)) if members else
                         ('no Z80 code' if not c['code'] else 'too little code')))
    with open(os.path.join(a.out, 'z80_families.csv'), 'w', newline='', encoding='utf8') as fcsv:
        w = csv.DictWriter(fcsv, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    lines = [f'{len(keys)} carts: {sum(1 for k in keys if not carts[k]["code"])} run no Z80 code, '
             f'{sum(1 for k in keys if carts[k]["code"] and not carts[k]["sh"])} less than {a.min_bytes} bytes; '
             f'{len(by_crc)} distinct builds, {len(ordered)} families (Jaccard >= {a.threshold} over {K}-byte sequences)', '']
    for n, members in enumerate(ordered, 1):
        builds = len({carts[k]['crc'] for k in members})
        size = sorted(len(carts[k]['code']) for k in members)[len(members) // 2]
        lines.append(f'family {n}: {len(members)} carts, {builds} builds, median {size} bytes of code -- '
                     + '; '.join(namesakes(members, 6)))
    with open(os.path.join(a.out, 'z80_families.txt'), 'w', encoding='utf8') as ft:
        ft.write('\n'.join(lines) + '\n')
    print('\n'.join(lines[:25]))
    if a.write_exports:
        for r in rows:
            ex = os.path.join(a.recomp, r['key'], 'export.json')
            if not os.path.exists(ex):
                continue
            j = json.load(open(ex, encoding='utf8'))
            z = j.setdefault('z80_code', {})
            z['family'] = r['family'] or None
            z['family_size'] = r['family_size']
            z['family_names'] = r['family_names']
            with open(ex + '.tmp', 'w', encoding='utf8') as f:     # whole or not at all
                json.dump(j, f)
            os.replace(ex + '.tmp', ex)


if __name__ == '__main__':
    sys.exit(main())

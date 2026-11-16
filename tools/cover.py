"""More than one run of the tracer, one coverage: the runs, their union, the export.

Each run is build/md_taint.exe with --cover (md.exe's tracer, with the pointers' provenance
beside it): a tape (md.exe --tape), the random hand with a seed, or no input at all (the
attract mode: the title and the demos).  The runs' files stay in OUT/runs/; their union is
OUT/trace.cov (the instructions), trace.cov.sites (the access sites, their classes or-ed),
trace.cov.taint (the origins, their flags or-ed and their uses summed; the probes merged) and trace.cov.z80
(the Z80's code: each RAM byte it ran, the first run's value), and
OUT/export.json is written from it (recomp/export.py, with the runs listed).

    python tools/cover.py ROM --out DIR [--tapes DIR] [--library] [--jobs 8]
    python tools/cover.py "Mega Drive/core" --library --jobs 16      # every cart: DIR = build/recomp/KEY

--library: the runs any cart gets -- the random hand with seeds 1, 2 and 3 (6000 frames each)
and the attract mode (14000 frames, no input), so that every demo and the menus the hand
reaches are in.  --tapes: a directory of tapes for this cart (tools/tapes_sonic1.py makes
Sonic 1's).
"""
import argparse
import concurrent.futures
import contextlib
import glob
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit.cart import Cart, run_flags  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(HERE, 'build', 'md_taint.exe')
LIBRARY = [('monkey1', ['--frames', '6000', '--monkey', '1']),
           ('monkey2', ['--frames', '6000', '--monkey', '2']),
           ('monkey3', ['--frames', '6000', '--monkey', '3']),
           ('attract', ['--frames', '14000'])]


@contextlib.contextmanager
def atomic(path):
    """A file written whole or not at all: another reader never sees it half written."""
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf8') as f:
        yield f
    os.replace(tmp, path)


def run(exe, rom, flags, name, args, runs_dir):
    cov = os.path.join(runs_dir, name + '.cov')
    t0 = time.time()
    r = subprocess.run([exe, os.path.abspath(rom), '--cover', cov] + args + flags,
                       capture_output=True, text=True, timeout=7200)
    info = dict(name=name, args=' '.join(a if ' ' not in a else f'"{a}"' for a in args),
                seconds=round(time.time() - t0, 1), ok=r.returncode == 0 and os.path.exists(cov))
    m = re.search(r'(\d+) frames in', r.stdout)
    info['frames'] = int(m.group(1)) if m else None
    m = re.search(r'tape: \d+ frames, (\d+) timeouts', r.stdout)
    if m:
        info['tape_timeouts'] = int(m.group(1))
    if not info['ok']:
        info['error'] = (r.stderr or r.stdout)[-300:]
    return info


def merge(runs_dir, names, out_cov):
    """The union of the runs' files as out_cov, .sites and .taint; each run's own count."""
    pcs, sites, origins, unattr, z80, loads = set(), {}, {}, {}, {}, {}
    header, sub, stack, ram_pcs, exits = {}, {}, {}, {}, {}
    per_run = {}
    for n in names:
        p = os.path.join(runs_dir, n + '.cov')
        if not os.path.exists(p):
            continue
        mine = {int(x, 16) for x in open(p).read().split()}
        per_run[n] = len(mine)
        pcs |= mine
        if os.path.exists(p + '.sites'):
            for ln in open(p + '.sites'):
                a, m = ln.split()
                sites[a] = sites.get(a, 0) | int(m, 16)
        if os.path.exists(p + '.taint'):
            for ln in open(p + '.taint'):
                w = ln.split()
                if not w or w[0] == '#':
                    continue
                if w[0] == 'O':
                    key = (w[1], w[2], w[3])
                    o = origins.setdefault(key, [w[4], 0, 0, '000000'])
                    o[1] |= int(w[5], 16)
                    o[2] += int(w[6])
                    if o[3] == '000000' and int(w[6]):
                        o[3] = w[7]
                elif w[0] == 'U':
                    u = unattr.setdefault(w[1], [0, w[3] if len(w) > 3 else '000000'])
                    u[0] += int(w[2])
                elif w[0] == 'A':
                    ld = loads.setdefault((w[1], w[2]), [0, w[4], w[5], w[6], w[7]])
                    ld[0] += int(w[3])
                    if ld[1] == '000000':
                        ld[1] = w[4]
                    if ld[2] == '000000' and w[5] != '000000':
                        ld[2:] = w[5:8]
                elif w[0] == 'H':                   # the header's first long read: PC VIA ADDR SIZE COUNT
                    k = tuple(w[1:5])
                    header[k] = header.get(k, 0) + int(w[5])
                elif w[0] == 'S':                   # under A7: PC VIA MIN MAX COUNT WRITTEN_AT
                    lo, hi, n = int(w[3]), int(w[4]), int(w[5])
                    o = sub.get((w[1], w[2]))
                    if o is None:
                        sub[(w[1], w[2])] = [lo, hi, n, w[6]]
                    else:
                        o[0] = min(o[0], lo)
                        if hi > o[1]:
                            o[1], o[3] = hi, w[6]
                        o[2] += n
                elif w[0] == 'K':                   # A7 loaded: PC COUNT LO HI JUMP VALUES
                    o = stack.setdefault(w[1], [0, 0xFFFFFFFF, 0, 0, set()])
                    o[0] += int(w[2])
                    o[1] = min(o[1], int(w[3], 16))
                    o[2] = max(o[2], int(w[4], 16))
                    o[3] = max(o[3], int(w[5], 16))
                    o[4] |= set(w[6].split(',')) if len(w) > 6 else set()
                elif w[0] == 'R':                   # run from the work RAM: PC COUNT COPY_PC ENCODINGS SOURCES
                    o = ram_pcs.setdefault(w[1], [0, '000000', set(), set()])
                    o[0] += int(w[2])
                    if o[1] == '000000':
                        o[1] = w[3]
                    o[2] |= set(w[4].split(','))
                    o[3] |= set(w[5].split(','))
                elif w[0] == 'X':                   # an address register's RAM address out: PC AN TO COUNT
                    k = tuple(w[1:4])
                    exits[k] = exits.get(k, 0) + int(w[4])
        if os.path.exists(p + '.z80'):
            for ln in open(p + '.z80'):
                a, b = ln.split()
                z80.setdefault(a, b)
    with atomic(out_cov) as f:
        f.writelines(f'{a:06X}\n' for a in sorted(pcs))
    with atomic(out_cov + '.sites') as f:
        f.writelines(f'{a} {m:08X}\n' for a, m in sorted(sites.items()))
    with atomic(out_cov + '.taint') as f:
        f.write('# mdkit taint 1 (merged): O ADDR SIZE PC VALUE FLAGS USES USE_PC; U PC COUNT WRITTEN_AT; '
                'A LOAD_PC REG USES USE_PC ORIGIN_ADDR ORIGIN_SIZE ORIGIN_PC; H PC VIA ADDR SIZE COUNT; '
                'S PC VIA MIN MAX COUNT WRITTEN_AT; K PC COUNT LO HI JUMP VALUES; '
                'R PC COUNT COPY_PC ENCODINGS SOURCES; X PC AN TO COUNT\n')
        for (a, s, pc), (v, fl, u, upc) in sorted(origins.items()):
            f.write(f'O {a} {s} {pc} {v} {fl:X} {u} {upc}\n')
        for pc, (n, at) in sorted(unattr.items()):
            f.write(f'U {pc} {n} {at}\n')
        for (pc, reg), (n, upc, oa, osz, opc) in sorted(loads.items()):
            f.write(f'A {pc} {reg} {n} {upc} {oa} {osz} {opc}\n')
        for k, n in sorted(header.items()):
            f.write(f'H {" ".join(k)} {n}\n')
        for (pc, via), (lo, hi, n, at) in sorted(sub.items()):
            f.write(f'S {pc} {via} {lo} {hi} {n} {at}\n')
        for pc, (n, lo, hi, jump, vals) in sorted(stack.items()):
            vals = sorted(vals, key=lambda v: int(v, 16))[:16]
            f.write(f'K {pc} {n} {lo:08X} {hi:08X} {jump:X} {",".join(vals)}\n')
        for pc, (n, copy, enc, srcs) in sorted(ram_pcs.items()):
            f.write(f'R {pc} {n} {copy} {",".join(sorted(enc))} {",".join(sorted(srcs))}\n')
        for k, n in sorted(exits.items()):
            f.write(f'X {" ".join(k)} {n}\n')
    with atomic(out_cov + '.z80') as f:
        f.writelines(f'{a} {b}\n' for a, b in sorted(z80.items()))
    return len(pcs), per_run


def one(rom, out, tapes, library, jobs, exe, quiet=False):
    c = Cart.load(rom)
    flags = run_flags(c)
    runs_dir = os.path.join(out, 'runs')
    os.makedirs(runs_dir, exist_ok=True)
    plan = list(LIBRARY) if library else []
    for t in sorted(glob.glob(os.path.join(tapes, '*.tape'))) if tapes else []:
        plan.append((os.path.splitext(os.path.basename(t))[0], ['--tape', os.path.abspath(t)]))
    with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as ex:
        infos = list(ex.map(lambda p: run(exe, c.path, flags, p[0], p[1], runs_dir), plan))
    # the runs made before stay in the union (a cart's tapes, then the library's runs): a run of
    # the same name is replaced
    listed = os.path.join(out, 'runs.json')
    earlier = json.load(open(listed, encoding='utf8'))['runs'] if os.path.exists(listed) else []
    mine = {i['name'] for i in infos}
    every = [r for r in earlier if r['name'] not in mine and r.get('ok')
             and os.path.exists(os.path.join(runs_dir, r['name'] + '.cov'))] + infos
    cov = os.path.join(out, 'trace.cov')
    total, per_run = merge(runs_dir, [i['name'] for i in every if i['ok']], cov)
    for i in every:
        i['instructions_run'] = per_run.get(i['name'])
    with atomic(listed) as f:
        json.dump(dict(cart=c.key, instructions_run=total, runs=every), f, indent=1)
    from mdkit.recomp.export import export as export_json
    with atomic(os.path.join(out, 'export.json')) as f:
        json.dump(export_json(c.path, [cov]), f)
    if not quiet:
        for i in infos:
            print(f"  {i['name']:16} {i.get('frames') or '':>6} frames {i['seconds']:>6} s  "
                  f"{i.get('instructions_run') or 0:>6} instructions"
                  + (f"  {i['tape_timeouts']} timeouts" if i.get('tape_timeouts') else '')
                  + ('' if i['ok'] else '  FAILED ' + i.get('error', '')))
    return c.key, total, sum(1 for i in infos if not i['ok'])


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('rom', help='a ROM, or a directory of them (each into build/recomp/KEY)')
    ap.add_argument('--out')
    ap.add_argument('--tapes')
    ap.add_argument('--library', action='store_true')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--only', default='')
    a = ap.parse_args(argv)
    if os.path.isdir(a.rom):
        roms = sorted(glob.glob(os.path.join(a.rom, '*.bin')))
        carts = [(r, Cart.load(r).key) for r in roms]
        if a.only:
            only = set(a.only.split(','))
            carts = [(r, k) for r, k in carts if k in only]
        recomp = os.path.join(HERE, 'build', 'recomp')

        def go(rk):
            r, k = rk
            try:
                return one(r, os.path.join(recomp, k), a.tapes, a.library, 1, a.exe, quiet=True)
            except Exception as e:      # one cart must not stop the library
                return k, 0, f'error {e}'
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            for k, total, failed in ex.map(go, carts):
                print(f'{k:28} {total:>7} {failed}', flush=True)
        return 0
    key, total, failed = one(a.rom, a.out, a.tapes, a.library, a.jobs, a.exe)
    print(f'{key}: {total} instructions run in all, {failed} runs failed')
    return 0


if __name__ == '__main__':
    sys.exit(main())

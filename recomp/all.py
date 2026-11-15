"""Every cartridge: trace, recompile, build, lockstep.

For each dump: the interpreter's run with the monkey's hand (seed 1) gives the coverage;
discovery and the C++ from it (and export.json, recomp/export.py); an executable; and that executable run in lockstep with
the interpreter for FRAMES frames with another hand (seed 2), so that the recompiled code
meets the interpreter at places the trace did not lead it to.  One line a cart in
build/recomp/all.csv: the instructions recompiled, the build's time, the lockstep's
verdict and the share of instructions run natively.

    python -m mdkit.recomp.all "Mega Drive/core" [--only KEY,...|@FILE] [--jobs 4] [--frames 6000]

A cart whose slot banks the ROM is recompiled where it is fixed: SSF2's first 512 KB, none of Realtec's.
"""
import argparse
import concurrent.futures
import csv
import glob
import os
import re
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit.cart import Cart, run_flags  # noqa: E402
from mdkit.recomp import discover as disc  # noqa: E402
from mdkit.recomp.emit import emit  # noqa: E402
from mdkit.recomp.__main__ import build  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def one(c, a):
    d = os.path.join(a.out, c.key)
    os.makedirs(d, exist_ok=True)
    row = dict(key=c.key, title=c.title, slot=c.slot)
    flags = run_flags(c)
    cov = os.path.join(d, 'trace.cov')
    t0 = time.time()
    r = subprocess.run([a.exe, os.path.abspath(c.path), '--frames', str(a.frames), '--monkey', '1', '--cover', cov] + flags,
                       capture_output=True, text=True, timeout=1800)
    row['trace_s'] = round(time.time() - t0, 1)
    if r.returncode or not os.path.exists(cov):
        row['verdict'] = 'trace failed'
        return row
    data = open(c.path, 'rb').read()
    # banked ROM: SSF2's fixed first 512 KB only; Realtec's (the whole ROM through its mapper) none
    limit = 0x80000 if c.slot == 'rom_ssf2' else 0 if c.slot == 'rom_realtec' else disc.ROM_END
    old = disc.ROM_END
    disc.ROM_END = limit
    try:
        prog = disc.discover(data, [cov])
    finally:
        disc.ROM_END = old
    row['insns'] = len(prog.code)
    try:                        # the Jaguar side's input, beside the C++
        import json
        from mdkit.recomp.export import export as export_json
        with open(os.path.join(d, 'export.json'), 'w', encoding='utf8') as f:
            json.dump(export_json(c.path, [cov]), f)
    except Exception as e:      # the export must not cost the lockstep
        row['error'] = f'export: {e}'
    for p in glob.glob(os.path.join(d, 'r*.cpp')) + glob.glob(os.path.join(d, '*.o')):
        os.remove(p)
    paths = emit(prog, d, zlib.crc32(data))
    t0 = time.time()
    try:
        exe = build(paths, d, a.opt)
    except RuntimeError as e:
        row['verdict'] = 'build failed'
        row['error'] = str(e)[-300:]
        return row
    row['build_s'] = round(time.time() - t0, 1)
    t0 = time.time()
    r = subprocess.run([exe, os.path.abspath(c.path), '--frames', str(a.frames), '--monkey', '2', '--lockstep'] + flags,
                       capture_output=True, text=True, timeout=3600)
    row['lockstep_s'] = round(time.time() - t0, 1)
    out = r.stdout
    m = re.search(r'native (\d+), interpreted (\d+)', out)
    if m:
        n, i = int(m.group(1)), int(m.group(2))
        row['native'], row['interp'] = n, i
        row['native_pct'] = round(100 * n / max(1, n + i), 3)
    if 'identical' in out:
        row['verdict'] = 'identical'
    else:
        row['verdict'] = (re.search(r'lockstep: frame .*', out) or [out.strip()[-120:] or f'rc {r.returncode}'])[0]
    # the objects are large and rebuilt from the C++: keep the executable and the sources
    for p in glob.glob(os.path.join(d, '*.o')):
        os.remove(p)
    return row


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--only', default='')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--frames', type=int, default=6000)
    ap.add_argument('--out', default=os.path.join(HERE, 'build', 'recomp'))
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'md.exe'))
    ap.add_argument('--opt', default='-O0', help="g++'s optimisation for the generated code (the build's time rules)")
    a = ap.parse_args(argv)
    a.exe = os.path.abspath(a.exe)
    carts = [Cart.load(p) for p in sorted(glob.glob(os.path.join(a.dir, '*.bin')))]
    if a.only:
        only = set(open(a.only[1:]).read().split() if a.only.startswith('@') else a.only.split(','))
        carts = [c for c in carts if c.key in only]
    rows = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(lambda c: one(c, a), carts):
            print(f"{r['key']:28} {r.get('insns', ''):>7} {r.get('native_pct', ''):>8} {r['verdict']}", flush=True)
            rows.append(r)
    rows.sort(key=lambda r: r['key'])
    fields = ['key', 'title', 'slot', 'insns', 'trace_s', 'build_s', 'lockstep_s', 'native', 'interp', 'native_pct',
              'verdict', 'error']
    os.makedirs(a.out, exist_ok=True)
    name = 'all.csv' if not a.only else 'some.csv'
    with open(os.path.join(a.out, name), 'w', newline='', encoding='utf8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    ok = sum(1 for r in rows if r['verdict'] == 'identical')
    print(f'{ok} of {len(rows)} identical in lockstep')


if __name__ == '__main__':
    sys.exit(main())

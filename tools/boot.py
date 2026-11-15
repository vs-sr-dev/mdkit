"""Run every cartridge of a collected set in the interpreter and keep a few frames of each.

For each dump: md.exe with the machine and SRAM cart.py gives it, the monkey's pad,
FRAMES frames, snapshots at SNAPS; then one mosaic per snapshot frame (a thumbnail a
game, its key under it) and a line per game in boot.csv (time, the state's hash, the
VINT and HINT counts, whether the run crashed).

    python tools/boot.py "Mega Drive/core" --out build/boot [--frames 6000] [--jobs 14]
"""
import argparse
import concurrent.futures
import csv
import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit.cart import Cart  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(exe, path, outdir, frames, snaps):
    c = Cart.load(path)
    d = os.path.join(outdir, 'runs', c.key)
    os.makedirs(d, exist_ok=True)
    cmd = [exe, path, '--machine', c.machine, '--frames', str(frames), '--snap', ','.join(map(str, snaps)),
           '--out', d, '--monkey', '1', '--state']
    if c.sram:
        cmd += ['--sram', str(c.sram)]
    if '6' in c.io:
        cmd.append('--pad6')
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        out, rc = p.stdout, p.returncode
    except subprocess.TimeoutExpired:
        out, rc = '', 'timeout'
    first = out.splitlines()[0] if out else ''
    ints = next((ln for ln in out.splitlines() if 'VINT' in ln), '')
    return dict(key=c.key, title=c.title, machine=c.machine, slot=c.slot, rc=rc, run=first, ints=ints.strip())


def mosaic(outdir, keys, frame, cols=24, tw=160, th=112):
    from PIL import Image, ImageDraw
    rows = (len(keys) + cols - 1) // cols
    m = Image.new('RGB', (cols * tw, rows * (th + 12)), (32, 32, 32))
    dr = ImageDraw.Draw(m)
    for i, k in enumerate(keys):
        x, y = (i % cols) * tw, (i // cols) * (th + 12)
        p = os.path.join(outdir, 'runs', k, f'f{frame:05d}.ppm')
        if os.path.exists(p):
            m.paste(Image.open(p).resize((tw, th)), (x, y))
        dr.text((x + 2, y + th), k.replace('megadriv_', '')[:24], fill=(200, 200, 200))
    m.save(os.path.join(outdir, f'mosaic_{frame:05d}.png'))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--out', default='build/boot')
    ap.add_argument('--frames', type=int, default=6000)
    ap.add_argument('--snaps', default='300,1500,3000,5999')
    ap.add_argument('--jobs', type=int, default=14)
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'md.exe'))
    a = ap.parse_args(argv)
    snaps = [int(s) for s in a.snaps.split(',') if int(s) < a.frames]
    paths = sorted(glob.glob(os.path.join(a.dir, '*.bin')))
    os.makedirs(a.out, exist_ok=True)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(lambda p: run(a.exe, p, a.out, a.frames, snaps), paths):
            rows.append(r)
            if r['rc'] != 0:
                print(f"{r['key']}: rc {r['rc']}", flush=True)
    rows.sort(key=lambda r: r['key'])
    with open(os.path.join(a.out, 'boot.csv'), 'w', newline='', encoding='utf8') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    keys = [r['key'] for r in rows]
    for fr in snaps:
        mosaic(a.out, keys, fr)
    bad = [r for r in rows if r['rc'] != 0]
    print(f'{len(rows)} carts, {len(bad)} did not finish')


if __name__ == '__main__':
    sys.exit(main())

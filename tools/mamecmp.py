"""Hold the interpreter to MAME, a frame at a time: the same cart, the same pad hand.

For each dump: md.exe (the machine, slot and SRAM cart.py gives it) and a MAME built with
the Mega Drive drivers (SUBTARGET=mdref SOURCES=src/mame/sega/mdconsole.cpp), both with
the monkey's seeded hand (tools/mame_snap.lua plays it in MAME) or a --press script, and
snapshots at the same frames.  MAME loads the software from its list, so that its cart
type is the list's: the dump is hard-linked under build/mame/roms as the list names it
(single-chip software; the rest is given to MAME as a file).

Each snapshot pair gets a score: the share of pixels whose colours differ by more than a
step.  mamecmp.csv has one line per game; diff_FRAME.png puts the worst pairs side by side
(md.exe left, MAME right).

    python tools/mamecmp.py "Mega Drive/core" --mame build/mame/mdref.exe [--only KEY,...|@FILE]
           [--frames 300,1500] [--seed 1 | --press "600+6:start"] [--jobs 14] [--out build/cmp]
"""
import argparse
import concurrent.futures
import csv
import glob
import html
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit.cart import Cart, run_flags  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MACHINES = {'NTSC-U': 'genesis', 'NTSC-J': 'megadrij', 'PAL': 'megadriv'}


def single_chip_names():
    """short -> the ROM's file name, for list entries made of one whole chip."""
    xml = open(os.path.join(HERE, 'hash', 'megadriv.xml'), encoding='utf8').read()
    out = {}
    for m in re.finditer(r'<software name="([^"]+)"(.*?)</software>', xml, re.S):
        area = re.search(r'<dataarea name="rom"[^>]*>(.*?)</dataarea>', m.group(2), re.S)
        if not area:
            continue
        roms = re.findall(r'<rom\s([^>]*)/>', area.group(1))
        off = re.search(r'offset="([^"]+)"', roms[0]) if len(roms) == 1 else None
        if len(roms) == 1 and 'loadflag' not in roms[0] and (not off or int(off.group(1), 0) == 0):
            out[m.group(1)] = html.unescape(re.search(r'name="([^"]+)"', roms[0]).group(1))
    return out


def mame_args(c, names, romroot):
    """MAME's machine and software for cart c: from the list where it can, else the file."""
    args = [MACHINES.get(c.machine, 'genesis')]
    name = names.get(c.short) if c.short else None
    if name:
        d = os.path.join(romroot, 'megadriv', c.short)
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, name)
        if not os.path.exists(dst):
            try:
                os.link(os.path.abspath(c.path), dst)
            except OSError:
                import shutil
                shutil.copyfile(c.path, dst)
        args.append(c.short)
    else:
        args += ['-cart', os.path.abspath(c.path)]
    if '6' in c.io:
        args += ['-ctrl1', 'md6button']
    return args


def score(a_path, b_path):
    """The share of pixels that differ by more than a step, md.exe's frame against MAME's."""
    from PIL import Image, ImageChops
    if not (os.path.exists(a_path) and os.path.exists(b_path)):
        return 1.0
    a = Image.open(a_path).convert('RGB')
    b = Image.open(b_path).convert('RGB')
    if b.size != a.size:
        b = b.resize(a.size, Image.NEAREST)
    d = ImageChops.difference(a, b).convert('L').point(lambda v: 255 if v > 24 else 0)
    return d.histogram()[255] / (a.size[0] * a.size[1])


def run(c, names, a):
    d = os.path.join(a.out, 'runs', c.key)
    os.makedirs(d, exist_ok=True)
    for f in glob.glob(os.path.join(d, 'f*.p*')):
        os.remove(f)
    frames = ','.join(map(str, a.frame_list))
    hand = ['--monkey', str(a.seed)] if not a.press else ['--press', a.press]
    env = dict(os.environ, MDSNAP=frames)
    if a.press:
        env['MDPRESS'] = a.press
    else:
        env['MDSEED'] = str(a.seed)
    md = [a.exe, os.path.abspath(c.path), '--frames', str(max(a.frame_list) + 1), '--snap', frames,
          '--out', d] + hand + run_flags(c)
    mame = [os.path.abspath(a.mame)] + mame_args(c, names, os.path.abspath(os.path.join(a.out, 'roms'))) + [
        '-rompath', os.path.abspath(os.path.join(a.out, 'roms')), '-hashpath', os.path.join(HERE, 'hash'),
        '-video', 'none', '-sound', 'none', '-nothrottle', '-skip_gameinfo', '-nonvram_save',
        '-autoboot_script', os.path.join(HERE, 'tools', 'mame_snap.lua'),
        '-snapshot_directory', os.path.abspath(d), '-cfg_directory', os.path.abspath(os.path.join(a.out, 'cfg')),
        '-nvram_directory', os.path.abspath(os.path.join(a.out, 'nvram', c.key))]
    try:
        subprocess.run(md, capture_output=True, timeout=900)
        rc = subprocess.run(mame, capture_output=True, timeout=1800, cwd=d, env=env).returncode
    except subprocess.TimeoutExpired:
        rc = 'timeout'
    # MAME writes its snapshots under a folder named after the system
    for p in glob.glob(os.path.join(d, '**', 'f*.png'), recursive=True):
        q = os.path.join(d, 'mame_' + os.path.basename(p))
        os.replace(p, q)
    row = dict(key=c.key, title=c.title, machine=c.machine, slot=c.slot, mame_rc=rc)
    for f in a.frame_list:
        row[f'f{f}'] = round(score(os.path.join(d, f'f{f:05d}.ppm'), os.path.join(d, f'mame_f{f:05d}.png')), 4)
    return row


def pairs(out, rows, frame, n=48, cols=6, tw=160, th=112):
    from PIL import Image, ImageDraw
    worst = sorted(rows, key=lambda r: -r[f'f{frame}'])[:n]
    worst = [r for r in worst if r[f'f{frame}'] > 0.02]
    if not worst:
        return
    rws = (len(worst) + cols - 1) // cols
    m = Image.new('RGB', (cols * (2 * tw + 8), rws * (th + 12)), (32, 32, 32))
    dr = ImageDraw.Draw(m)
    for i, r in enumerate(worst):
        x, y = (i % cols) * (2 * tw + 8), (i // cols) * (th + 12)
        for j, name in enumerate((f'f{frame:05d}.ppm', f'mame_f{frame:05d}.png')):
            p = os.path.join(out, 'runs', r['key'], name)
            if os.path.exists(p):
                m.paste(Image.open(p).convert('RGB').resize((tw, th)), (x + j * tw, y))
        dr.text((x + 2, y + th), f"{r['key'].replace('megadriv_', '')[:28]} {r[f'f{frame}']:.2f}", fill=(220, 220, 220))
    m.save(os.path.join(out, f'diff_{frame:05d}.png'))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--mame', default=os.path.join(HERE, 'build', 'mame', 'mdref.exe'))
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'md.exe'))
    ap.add_argument('--out', default=os.path.join(HERE, 'build', 'cmp'))
    ap.add_argument('--frames', default='300,1500')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--press', default='')
    ap.add_argument('--only', default='')
    ap.add_argument('--jobs', type=int, default=14)
    a = ap.parse_args(argv)
    a.exe = os.path.abspath(a.exe)
    a.frame_list = [int(f) for f in a.frames.split(',')]
    os.makedirs(a.out, exist_ok=True)
    carts = [Cart.load(p) for p in sorted(glob.glob(os.path.join(a.dir, '*.bin')))]
    if a.only:
        only = set(open(a.only[1:]).read().split() if a.only.startswith('@') else a.only.split(','))
        carts = [c for c in carts if c.key in only]
    names = single_chip_names()
    rows = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(lambda c: run(c, names, a), carts):
            rows.append(r)
    rows.sort(key=lambda r: r['key'])
    with open(os.path.join(a.out, 'mamecmp.csv'), 'w', newline='', encoding='utf8') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    for fr in a.frame_list:
        pairs(a.out, rows, fr)
        bad = sum(1 for r in rows if r[f'f{fr}'] > 0.02)
        print(f'frame {fr}: {bad} of {len(rows)} differ from MAME on more than 2% of the picture')


if __name__ == '__main__':
    sys.exit(main())

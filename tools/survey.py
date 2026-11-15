"""The library's survey: what each cartridge does with the machine, for the Jaguar side.

Each dump runs FRAMES frames in the interpreter (md.exe --survey, the monkey's hand) and
gives one line of counts: the VDP's modes frame by frame (H32/H40, V30, interlace,
shadow/highlight, window, the scroll modes), sprites per line, DMA words, HINTs, writes to
CRAM, VSRAM and the registers while the display draws (raster effects), reads of the
status and the HV counter (busy waits), and the sound: who writes the YM2612 (68000 or
Z80), DAC samples, key-ons, PSG writes, YM2612 register $27's modes, and a hash of the
Z80's RAM when the 68000 last let it out of reset -- the sound driver, grouped into
families by identical uploads.  Where recomp.all left an export.json beside, its access
sites and absolute operands join the row.

    python tools/survey.py "Mega Drive/core" --exe build/md_survey.exe [--jobs 6] [--frames 6000]
           [--out build/survey]            -> survey.csv, summary.txt
"""
import argparse
import concurrent.futures
import csv
import glob
import json
import os
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from mdkit.cart import Cart, run_flags  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def one(c, a):
    row = dict(key=c.key, title=c.title, year=c.sw.year if c.sw else '', publisher=c.sw.publisher if c.sw else '',
               machine=c.machine, slot=c.slot, size=c.size, io=''.join(sorted(set(c.io) & set('J64M'))))
    try:
        r = subprocess.run([a.exe, os.path.abspath(c.path), '--frames', str(a.frames), '--monkey', '1', '--survey']
                           + run_flags(c), capture_output=True, text=True, timeout=1800)
        line = next((ln for ln in r.stdout.splitlines() if ln.startswith('survey ')), '')
        for kv in line.split()[1:]:
            k, v = kv.split('=')
            row[k] = v
    except subprocess.TimeoutExpired:
        row['error'] = 'timeout'
    return join_export(row, a)


def join_export(row, a):
    ex = os.path.join(a.recomp, row['key'], 'export.json')
    if os.path.exists(ex):
        j = json.load(open(ex, encoding='utf8'))
        row['insns'] = j['code']['instructions']
        row['sites'] = len(j['sites'])
        modes = Counter(m for s in j['sites'] for m in s['modes'] if m not in ('dn', 'imm'))
        for m in ('ai', 'pi', 'pd', 'di', 'ix', 'aw', 'al'):
            row[f'site_{m}'] = modes.get(m, 0)
        for area, d in j['absolute'].items():
            row[f'abs_{area}'] = sum(d.values())
        row['abs_ram_w'] = j['absolute'].get('work_ram', {}).get('aw', 0)
        row['indirect'] = len(j['indirect'])
        row['special'] = ' '.join(sorted({s['text'].split()[0] for s in j['special']}))
    return row


def summary(rows):
    n = len(rows)
    num = lambda r, k: int(r.get(k) or 0)
    has = lambda k: sum(1 for r in rows if num(r, k) > 0)
    L = [f'{n} carts']
    big = lambda k: sum(1 for r in rows if num(r, k) > 60)      # a mode held over a second, not a reset's frame
    L.append(f'VDP: H32 at some point {big("h32")}, only H32 {sum(1 for r in rows if num(r, "h32") > 60 and num(r, "h40") <= 60)}; '
             f'V30 {has("v30")}; interlace {has("interlace")}; shadow/highlight {has("shadow")}; window {has("window")}')
    L.append(f'scroll: per line {has("hs_line")}, per cell {has("hs_cell")}, column vscroll {has("vs_column")}')
    L.append(f'raster: HINT {has("hint_frames")}; CRAM written during the display {has("cram_active")}, '
             f'VSRAM {has("vsram_active")}, registers {has("reg_active")}')
    if any('raster_frames_cram' in r for r in rows):
        L.append(f'raster, mid-picture (lines 8 to the last but 8) in more than 60 frames: CRAM {big("raster_frames_cram")}, '
                 f'VSRAM {big("raster_frames_vsram")}, registers {big("raster_frames_reg")}')
    L.append(f'sprites: over the line limit {has("spr_ovf_lines")}; HV counter read {has("hv_reads")}')
    dm = sorted(num(r, 'dma_frame_max') for r in rows)
    if dm:
        L.append(f'DMA words in a frame, max: median {dm[n // 2]}, 90th {dm[n * 9 // 10]}, top {dm[-1]}')
    L.append(f'sound: YM2612 written by the 68000 only {sum(1 for r in rows if num(r, "ym_writes") and num(r, "ym_writes") == num(r, "ym_writes_68k"))}, '
             f'by the Z80 only {sum(1 for r in rows if num(r, "ym_writes") and not num(r, "ym_writes_68k"))}, '
             f'both {sum(1 for r in rows if num(r, "ym_writes_68k") and num(r, "ym_writes") > num(r, "ym_writes_68k"))}; '
             f'DAC {has("dac_writes")}; PSG {has("psg_writes")}; no YM2612 at all {sum(1 for r in rows if not num(r, "ym_writes"))}')
    m27 = Counter()
    for r in rows:
        v = int(r.get('ym27') or '0', 16)
        if v & 0x05: m27['timer A'] += 1
        if v & 0x0A: m27['timer B'] += 1
        if (v & 0xC0) == 0x80: m27['CSM'] += 1
        if v & 0x40: m27['channel 3 special'] += 1
    L.append(f'YM2612 $27: {dict(m27)}')
    drivers = Counter(r.get('z80_hash') for r in rows if num(r, 'z80_uploads'))
    L.append(f'Z80 driver uploads: {sum(drivers.values())} carts, {len(drivers)} distinct images; '
             f'the most shared: {drivers.most_common(5)}')
    if any('abs_ram_w' in r for r in rows):
        L.append(f'code: abs.w work RAM operands, median {sorted(num(r, "abs_ram_w") for r in rows)[n // 2]}; '
                 f'access sites by mode (carts using it): '
                 + ', '.join(f'{m} {has("site_" + m)}' for m in ('ai', 'pi', 'pd', 'di', 'ix', 'aw', 'al')))
        sp = Counter(w for r in rows for w in (r.get('special') or '').split())
        L.append(f'special instructions (carts): {dict(sp)}')
    return '\n'.join(L)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--exe', default=os.path.join(HERE, 'build', 'md_survey.exe'))
    ap.add_argument('--recomp', default=os.path.join(HERE, 'build', 'recomp'))
    ap.add_argument('--out', default=os.path.join(HERE, 'build', 'survey'))
    ap.add_argument('--frames', type=int, default=6000)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--only', default='')
    ap.add_argument('--resummarize', action='store_true', help='only rebuild summary.txt from survey.csv and the exports')
    a = ap.parse_args(argv)
    a.exe = os.path.abspath(a.exe)
    os.makedirs(a.out, exist_ok=True)
    carts = [Cart.load(p) for p in sorted(glob.glob(os.path.join(a.dir, '*.bin')))]
    if a.only:
        only = set(a.only.split(','))
        carts = [c for c in carts if c.key in only]
    if a.resummarize:
        rows = [join_export(dict(r), a) for r in csv.DictReader(open(os.path.join(a.out, 'survey.csv'), encoding='utf8'))]
    else:
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            rows = sorted(ex.map(lambda c: one(c, a), carts), key=lambda r: r['key'])
    fields = []
    for r in rows:
        fields += [k for k in r if k not in fields]
    with open(os.path.join(a.out, 'survey.csv'), 'w', newline='', encoding='utf8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    s = summary(rows)
    open(os.path.join(a.out, 'summary.txt'), 'w', encoding='utf8').write(s + '\n')
    print(s)


if __name__ == '__main__':
    sys.exit(main())

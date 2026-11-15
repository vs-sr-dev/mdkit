"""A Mega Drive cartridge: its header, what MAME's list says about it, and
the set's dumps brought to the image the list describes.

The 68000 sees the cartridge from $000000 up (4 MB of address space for it;
SRAM, when a cart has it, usually answers at $200000 once $A130F1 maps it
in).  The header sits at $100-$1FF:

    $100  console name (16)  'SEGA MEGA DRIVE ' / 'SEGA GENESIS    '
    $110  copyright (16)     '(C)SEGA 1991.APR'
    $120  domestic title (48)          $150  overseas title (48)
    $180  serial (14)        'GM 00001009-00'
    $18E  checksum: the 16-bit sum of the words from $200 to the ROM's end
    $190  I/O support (16)   J pad, 6 six-button pad, 4 multitap, M mouse,
                             K keyboard, L Activator, G/V/A guns, D drive...
    $1A0  ROM start, end (2 x 32 bits)     $1A8  RAM start, end
    $1B0  'RA', type, $20, SRAM start, end (when the cart keeps saves)
    $1BC  modem (12)         $1C8  notes (40)
    $1F0  regions (3): 'JUE' letters, or one hex digit of bits
                             (1 Japan NTSC, 2 Japan PAL, 4 overseas NTSC, 8 overseas PAL)

What a dump is comes from MAME's software list (hash/megadriv.xml, CC0) by
CRC32.  A set's dumps are not always the list's image: a GoodGen dump is
often the ROM's used part, where MAME lists the whole chip (Flashback, 1.5
of 2 MB: the rest reads $FF), or an overdump; some are in the SMD format
(a 512-byte header, then 16 KB blocks with the odd bytes first and the
even ones after); and the list describes some carts chip by chip -- two
EPROMs for the even and odd bytes, a chip continued at another offset
(Sword of Vermilion), a chip whose words are byte-swapped.  identify()
finds the listed software behind a dump and returns its image: the 68000's
view of the ROM, the list's size.  Collected dumps are written as that
image, so they are found again by one CRC.

Every listed software falls in a tier, by its description, year and
publisher:

    core      the period's commercial releases (to 1998): the library
    proto     prototypes, demos, betas, Sega Channel and Sega Game
              Toshokan downloads, launch and kiosk carts: counted apart
    unl       the period's unlicensed and pirate carts (Taiwan, China,
              Russia, Argentina...): counted apart
    out       homebrew and later releases (after 1998), hacks, ripped
              and translated images: not counted

The machine a game runs on is its region's: MAME's `compatibility`
(NTSC-J, NTSC-U, PAL), else the description's regions, else the header's.
NTSC-U and NTSC-J are the 60 Hz machines (overseas and domestic), PAL the
50 Hz one; games read which they are on at $A10001 and some refuse the
wrong one.

    python -m mdkit.cart ROM.bin                            # what it is
    python -m mdkit.cart "Mega Drive/*/*" --table
    python -m mdkit.cart "Mega Drive/*/*" --summary         # the library: tiers, regions, sizes, carts
    python -m mdkit.cart --collect SETDIR "Mega Drive"      # the listed dumps of a set, one each
"""

import argparse
import array
import concurrent.futures
import glob
import os
import re
import shutil
import sys
import zipfile
import zlib
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
LIST = 'megadriv'
EXTS = ('.bin', '.md', '.gen', '.smd', '.zip')
TIERS = ('core', 'proto', 'unl', 'out')

# the slots (MAME's cart types) the runtime implements; the others: "Not covered (yet)"
SUPPORTED_SLOTS = ('rom', 'rom_sram', 'rom_fram')

IO = {'J': 'pad', '6': '6-button', '4': 'multitap', '0': 'master-system pad', 'M': 'mouse', 'K': 'keyboard',
      'L': 'activator', 'G': 'menacer', 'V': 'paddle', 'A': 'analog', 'B': 'trackball', 'C': 'cd-rom',
      'D': 'download', 'F': 'floppy', 'P': 'printer', 'R': 'serial', 'T': 'tablet', 'O': 'justifier'}

# a bare date in a description tells revisions of a release apart ('199307'): not a prototype
_PROTO = re.compile(r'\b(prototype|beta|demo|sample|sega channel|sega game toshokan|kiosk|trial version)\b', re.I)
_UNL = re.compile(r'^(pirate|taiwan|china|chinese|hong kong|russia|unprotected\??|protected|argentina|'
                  r'unl|unlicensed|bootleg)$', re.I)
_OUT = re.compile(r'^(hacked\??|bad\?|pd|digital release|retro-bit|.*translation.*|ripped from .*|'
                  r'v\d+\.\d+\.\d+)$', re.I)

_sw = None


def _unxml(s):
    return s.replace('&amp;', '&').replace('&apos;', "'").replace('&quot;', '"').replace('&lt;', '<').replace('&gt;', '>')


def _int(s):
    return int(s, 0)


class Software:
    """One entry of MAME's list."""

    def __init__(self, short, attrs, body):
        self.short = short
        p = re.search(r'cloneof="([^"]+)"', attrs)
        s = re.search(r'supported="([^"]+)"', attrs)
        self.parent = p.group(1) if p else None
        self.supported = s.group(1) if s else 'yes'
        self.title = _unxml(re.search(r'<description>(.*?)</description>', body).group(1))
        y = re.search(r'<year>(.*?)</year>', body)
        self.year = y.group(1) if y else ''
        pb = re.search(r'<publisher>(.*?)</publisher>', body)
        self.publisher = _unxml(pb.group(1)) if pb else ''
        self.info = dict((k, _unxml(v)) for k, v in re.findall(r'<info name="([^"]+)" value="([^"]*)"', body))
        compat = re.search(r'<sharedfeat name="compatibility" value="([^"]*)"', body)
        self.compat = compat.group(1).split(',') if compat else []
        self.addon = (re.search(r'<sharedfeat name="addon" value="([^"]*)"', body) or [None, None])[1]
        part = re.search(r'<part name="[^"]*" interface="[^"]*">(.*?)</part>', body, re.S)
        pbody = part.group(1) if part else body
        feat = dict(re.findall(r'<feature name="([^"]+)" value="([^"]*)"', pbody))
        self.slot = feat.get('slot', 'rom')
        sram = re.search(r'<dataarea name="sram" size="(0x[0-9a-fA-F]+|\d+)"', pbody)
        self.sram = _int(sram.group(1)) if sram else 0
        # the ROM: its size, and its chips (each a CRC over one or more segments)
        area = re.search(r'<dataarea name="rom"[^>]*size="(0x[0-9a-fA-F]+|\d+)"[^>]*>(.*?)</dataarea>', pbody, re.S)
        self.size = _int(area.group(1)) if area else 0
        self.chips = []
        off = 0
        for r in re.finditer(r'<rom\s([^>]*)/>', area.group(2) if area else ''):
            a = dict(re.findall(r'(\w+)="([^"]*)"', r.group(1)))
            o = _int(a['offset']) if 'offset' in a else off
            n = _int(a['size']) if 'size' in a else 0
            flag = a.get('loadflag')
            if flag == 'continue' and self.chips:
                self.chips[-1]['segs'].append((o, n))
            elif 'crc' in a:
                self.chips.append(dict(crc=int(a['crc'], 16), size=n, flag=flag, segs=[(o, n)]))
            off = o + n
        self.tier, self.why = self._tier()

    @property
    def tags(self):
        return [t.strip() for g in re.findall(r'\(([^)]*)\)', self.title) for t in g.split(',')]

    @property
    def simple(self):
        """One chip loaded as it is at 0: its CRC is the image's."""
        return len(self.chips) == 1 and self.chips[0]['flag'] is None and self.chips[0]['segs'] == [(0, self.chips[0]['size'])]

    def _tier(self):
        tags = self.tags
        yr = re.match(r'(\d{3})', self.year)
        year = int(self.year[:4].replace('?', '0')) if yr else 0
        for t in tags:
            if _OUT.match(t):
                return 'out', t
        if year > 1998:
            return 'out', f'year {self.year}'
        if self.publisher.lower() in ('<homebrew>', 'homebrew'):
            return 'out', 'homebrew'
        if self.slot in ('rom_ggenie', 'ar') or re.match(r'(Pro )?Action Replay', self.title):
            return 'out', 'cheat device'
        for t in tags:
            if _PROTO.search(t):
                return 'proto', t
        if re.match(r'Sega Channel\b', self.title):
            return 'proto', 'Sega Channel'
        if re.search(r'\b(Activator Command Demo|Sound Tool|GEMS v|Art Tool)', self.title):
            return 'proto', 'development tool'
        for t in tags:
            if _UNL.match(t):
                return 'unl', t
        if re.search(r'<unlicensed>|<unknown>|pirate|bootleg', self.publisher, re.I):
            return 'unl', self.publisher
        return 'core', ''

    @property
    def machine(self):
        """'NTSC-U', 'NTSC-J' or 'PAL' from the list: compatibility, then the description's regions."""
        if self.compat:
            return self.compat[0]
        tags = set(self.tags)
        if tags & {'USA', 'World', 'Brazil', 'Argentina'}:
            return 'NTSC-U'
        if tags & {'Japan', 'Korea', 'Taiwan', 'China', 'Asia', 'Hong Kong'}:
            return 'NTSC-J'
        if tags & {'Europe', 'Germany', 'France', 'Spain', 'Italy', 'Sweden', 'Oceania', 'UK', 'Russia',
                   'Scandinavia', 'Netherlands', 'Australia'}:
            return 'PAL'
        return None


def software():
    """short name -> Software, from MAME's list (built here, published whole: threads read it)."""
    global _sw
    if _sw is None:
        x = open(os.path.join(HERE, 'hash', LIST + '.xml'), encoding='utf8').read()
        x = re.sub(r'<!--.*?-->', '', x, flags=re.S)
        found = {}
        for m in re.finditer(r'<software name="([^"]+)"([^>]*)>(.*?)</software>', x, re.S):
            found[m.group(1)] = Software(m.group(1), m.group(2), m.group(3))
        _sw = found
    return _sw


_index = None


def index():
    """(CRC -> [Software] for the one-chip images, the sizes they come in, the others)."""
    global _index
    if _index is None:
        single, sizes, multi = {}, set(), []
        for s in software().values():
            if s.simple:
                single.setdefault(s.chips[0]['crc'], []).append(s)
                sizes.add(s.chips[0]['size'])
            elif s.chips:
                multi.append(s)
        _index = (single, sorted(sizes), multi)
    return _index


def family(s):
    return s.parent or s.short


# -- dumps -----------------------------------------------------------------------------------

def is_smd(data):
    """A 512-byte copier header, then interleaved 16 KB blocks (byte 8, 9 = $AA, $BB on most)."""
    if len(data) % 0x4000 != 0x200 or len(data) < 0x4200:
        return False
    if data[8] == 0xAA and data[9] == 0xBB:
        return True
    # no marker: the first block must hold 'SEGA' at $100 once put back in order, and not before
    return data[0x100:0x104] != b'SEGA' and deinterleave(data[:0x4200])[0x100:0x104] == b'SEGA'


def deinterleave(data):
    """SMD to the 68000's image: in each 16 KB block, the first half holds the odd bytes."""
    body = data[0x200:]
    out = bytearray(len(body))
    for b in range(0, len(body), 0x4000):
        blk = body[b:b + 0x4000]
        h = len(blk) // 2
        out[b + 1:b + 2 * h:2] = blk[:h]
        out[b:b + 2 * h:2] = blk[h:2 * h]
    return bytes(out)


def _chip(img, c, fill=None):
    """The bytes of chip c as the 68000's image holds them; past the image's end, `fill` (or None: no chip)."""
    out = bytearray()
    for o, n in c['segs']:
        seg = img[o:o + 2 * n:2] if c['flag'] == 'load16_byte' else img[o:o + n]
        if len(seg) != n:
            if fill is None:
                return None
            seg += fill * (n - len(seg))
        out += seg
    if c['flag'] == 'load16_word_swap':
        out[0::2], out[1::2] = out[1::2], out[0::2]
    return bytes(out)


def _multi_match(img, s, memo, fill=None):
    """Every chip of s in img (chips' CRCs kept in memo: many entries share a layout)."""
    for c in s.chips:
        k = (tuple(c['segs']), c['flag'], fill)
        if k not in memo:
            d = _chip(img, c, fill)
            memo[k] = None if d is None else zlib.crc32(d)
        if memo[k] != c['crc']:
            return False
    return True


def identify(data):
    """(Software, image, how) for a dump the list knows, else (None, image, '').
    how: 'exact', 'smd', 'chips' (a multi-chip entry), 'padff'/'pad00' (the dump is short of the chip),
    'trunc' (overdump), joined by '+' (smd+padff)."""
    single, sizes, multi = index()
    imgs = []
    if is_smd(data):
        imgs.append(('smd', deinterleave(data)))
    imgs.append(('', bytes(data)))
    for pre, img in imgs:
        tag = (lambda h: '+'.join(x for x in (pre, h) if x))
        n = len(img)
        crc = zlib.crc32(img)
        if crc in single:
            return single[crc][0], img, tag('exact') if not pre else pre
        memo = {}
        for s in multi:
            if _multi_match(img, s, memo):
                return s, img, tag('chips')
        # the dump is short of the chip MAME lists: the rest of the chip reads $FF (or $00)
        for fill, name in ((b'\xff', 'padff'), (b'\0', 'pad00')):
            run, at = crc, n
            for size in sizes:
                if size <= n:
                    continue
                if size > 4 * n:
                    break
                run = zlib.crc32(fill * (size - at), run)
                at = size
                if run in single:
                    return single[run][0], img + fill * (size - n), tag(name)
        for s in multi:
            if s.size > n and _multi_match(img, s, memo, b'\xff'):
                return s, img + b'\xff' * (s.size - n), tag('chips+padff')
        # an overdump: the list's image is the dump's beginning (one pass, the CRC carried along)
        run, at = 0, 0
        for size in sizes:
            if size >= n:
                break
            run = zlib.crc32(img[at:size], run)
            at = size
            if run in single:
                return single[run][0], img[:size], tag('trunc')
    return None, imgs[0][1], ''


def read_dump(path):
    """The bytes of a dump: a loose file, or the first ROM in a zip."""
    if path.lower().endswith('.zip'):
        with zipfile.ZipFile(path) as z:
            names = [i for i in z.infolist() if not i.is_dir()]
            roms = [i for i in names if i.filename.lower().endswith(EXTS)] or names
            return z.read(roms[0]) if roms else b''
    with open(path, 'rb') as f:
        return f.read()


class Cart:
    def __init__(self, data, path=''):
        self.path = path
        self.raw_size = len(data)
        self.sw, self.data, self.how = identify(data)
        self.size = len(self.data)
        self.crc = zlib.crc32(self.data)
        h = self.data[0x100:0x200] if self.size >= 0x200 else bytes(0x100)
        self.header = h
        t = lambda a, n: h[a:a + n].decode('latin1').replace('\0', ' ').rstrip()
        self.console, self.copyright = t(0, 16), t(0x10, 16)
        self.domestic, self.overseas = ' '.join(t(0x20, 48).split()), ' '.join(t(0x50, 48).split())
        self.serial = t(0x80, 14)
        self.hsum = h[0x8E] << 8 | h[0x8F]
        self.io = t(0x90, 16).replace(' ', '')
        be32 = lambda a: int.from_bytes(h[a:a + 4], 'big')
        self.rom_start, self.rom_end, self.ram_start, self.ram_end = be32(0xA0), be32(0xA4), be32(0xA8), be32(0xAC)
        self.has_sram_header = h[0xB0:0xB2] == b'RA'
        self.sram_type = h[0xB2] if self.has_sram_header else 0
        self.sram_start, self.sram_end = (be32(0xB4), be32(0xB8)) if self.has_sram_header else (0, 0)
        self.region_field = t(0xF0, 3).strip()
        self.listed = self.sw is not None
        s = self.sw
        self.short = s.short if s else None
        self.title = s.title if s else self.name
        self.parent = s.parent if s else None
        self.tier = s.tier if s else 'unlisted'
        self.slot = s.slot if s else ('rom_sram' if self.has_sram_header else 'rom')
        self.sram = s.sram if s else (self.sram_end - self.sram_start + 1 if self.has_sram_header else 0)
        self.mame_supported = s.supported if s else 'yes'
        self._sum = None

    @classmethod
    def load(cls, path):
        return cls(read_dump(path), path)

    @property
    def name(self):
        return os.path.splitext(os.path.basename(self.path))[0]

    @property
    def key(self):
        if self.short:
            return f'{LIST}_{self.short}'
        return re.sub(r'[^A-Za-z0-9]+', '_', self.name).strip('_')

    def checksum(self):
        """$18E as games check it: the 16-bit sum of the big-endian words from $200 to the ROM's end."""
        end = min(self.size, (self.rom_end + 1) if 0x200 < self.rom_end < self.size else self.size)
        if self._sum is None:
            d = self.data[0x200:end]
            if len(d) & 1:
                d += b'\0'
            w = array.array('H', d)
            if sys.byteorder == 'little':
                w.byteswap()
            self._sum = sum(w) & 0xFFFF
        return self._sum

    def header_regions(self):
        """The machines the header admits, as a set of 'NTSC-J', 'NTSC-U', 'PAL'."""
        r = self.region_field
        out = set()
        if len(r) == 1 and r in '0123456789ABCDEF' and r not in 'JUE':
            v = int(r, 16)
            if v & 1: out.add('NTSC-J')
            if v & 2: out.add('PAL')
            if v & 4: out.add('NTSC-U')
            if v & 8: out.add('PAL')
            return out
        if 'J' in r: out.add('NTSC-J')
        if 'U' in r: out.add('NTSC-U')
        if 'E' in r: out.add('PAL')
        return out

    @property
    def machine(self):
        m = self.sw.machine if self.sw else None
        if m:
            return m
        h = self.header_regions()
        for m in ('NTSC-U', 'NTSC-J', 'PAL'):
            if m in h:
                return m
        return 'NTSC-U'

    @property
    def peripherals(self):
        return [IO.get(ch, ch) for ch in dict.fromkeys(self.io) if ch in IO]


def run_flags(c):
    """The runtime's options for cart c."""
    f = ['--machine', c.machine, '--slot', c.slot]
    if c.sram:
        f += ['--sram', str(c.sram)]
    if '6' in c.io:
        f.append('--pad6')
    return f


# -- the set ---------------------------------------------------------------------------------

def _score(fname, how):
    """Lower is better: a verified dump, as the list has it, with the fewest flags."""
    flags = re.findall(r'\[([^\]]*)\]', fname)
    return ('!' not in flags, how != 'exact', len([f for f in flags if f != '!']), fname.count('('), len(fname), fname)


def collect(setdir, outdir, jobs=12, log=print):
    """Write the dumps of a set that MAME's list knows into OUTDIR/<tier>/, one per software,
    as the list's image (de-interleaved, padded or cut, chips put together)."""
    paths = []
    for d, _, fs in os.walk(setdir):
        if os.path.abspath(d).startswith(os.path.abspath(outdir)):
            continue
        paths += [os.path.join(d, f) for f in sorted(fs) if f.lower().endswith(EXTS)]

    def one(p):
        try:
            data = read_dump(p)
        except (zipfile.BadZipFile, OSError) as e:
            return p, None, None, str(e)
        sw, img, how = identify(data)
        return p, sw, (img if sw else None), how

    best, bad, hows = {}, [], Counter()
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for p, sw, img, how in ex.map(one, paths):
            if sw is None:
                if img is None and how:
                    bad.append((p, how))
                continue
            hows[how] += 1
            sc = _score(os.path.basename(p), how)
            if sw.short not in best or sc < best[sw.short][0]:
                best[sw.short] = (sc, p, sw, img)
    n = 0
    for short, (_, p, sw, img) in sorted(best.items()):
        dst = os.path.join(outdir, sw.tier)
        os.makedirs(dst, exist_ok=True)
        base = re.sub(r'\.(zip|bin|md|gen|smd)$', '', os.path.basename(p), flags=re.I)
        with open(os.path.join(dst, base + '.bin'), 'wb') as f:
            f.write(img)
        n += 1
    per = Counter(sw.tier for _, _, sw, _ in best.values())
    log(f'{len(paths)} files, {sum(hows.values())} the list knows ({", ".join(f"{k} {v}" for k, v in hows.most_common())})')
    log(f'{n} software written to {outdir} (' + ', '.join(f'{t} {per[t]}' for t in TIERS if per[t]) + ')')
    for p, e in bad:
        log(f'  unreadable: {p}: {e}')
    return best


def coverage(have, log=print):
    """What of the list a set of short names covers, by tier: software and families."""
    sw = software()
    for t in TIERS:
        alls = [s for s in sw.values() if s.tier == t]
        got = [s for s in alls if s.short in have]
        fa = {family(s) for s in alls}
        fg = {family(s) for s in got}
        log(f'  {t:6} {len(got):5} of {len(alls):5} software   {len(fg):4} of {len(fa):4} families')
    core = [s for s in sw.values() if s.tier == 'core']
    miss = sorted({family(s) for s in core} - {family(s) for s in core if s.short in have})
    if miss:
        log(f'  core families the set lacks ({len(miss)}):')
        for f in miss:
            s = sw[f] if f in sw and sw[f].tier == 'core' else next(x for x in core if family(x) == f)
            log(f'    {f:12} {s.title}  [{s.year}, {s.publisher}]')


def summary(carts):
    listed = [c for c in carts if c.listed]
    print(f'{len(carts)} dumps, {len(listed)} in the list ('
          + ', '.join(f'{t} {sum(c.tier == t for c in listed)}' for t in TIERS) + ')')
    for title, f in (('tier', lambda c: c.tier), ('machine', lambda c: c.machine),
                     ('ROM', lambda c: f'{c.size // 1024}K' if c.size < 0x100000 else f'{c.size / 0x100000:g}M'),
                     ('slot', lambda c: c.slot),
                     ('SRAM', lambda c: f'{c.sram // 1024}K' if c.sram >= 1024 else f'{c.sram}B' if c.sram else '-'),
                     ('MAME', lambda c: c.mame_supported)):
        cnt = Counter(f(c) for c in listed)
        print(f'  {title:8}' + '  '.join(f'{k} {v}' for k, v in sorted(cnt.items(), key=lambda kv: -kv[1])))
    io = Counter(p for c in listed for p in c.peripherals)
    print('  I/O     ' + '  '.join(f'{k} {v}' for k, v in io.most_common()))
    print(f'  checksum bad {sum(c.checksum() != c.hsum for c in listed)}   '
          f'header region disagrees with the machine {sum(bool(c.header_regions()) and c.machine not in c.header_regions() for c in listed)}')
    odd = [c for c in listed if c.mame_supported != 'yes' or c.slot not in SUPPORTED_SLOTS]
    for c in sorted(odd, key=lambda c: (c.tier, c.slot, c.key)):
        print(f'    {c.tier:5} {c.key:24} {c.slot:18} MAME {c.mame_supported:8} {c.title}')


def main(argv=None):
    ap = argparse.ArgumentParser(prog='mdkit.cart')
    ap.add_argument('roms', nargs='*')
    ap.add_argument('--table', action='store_true')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--coverage', action='store_true', help='what of the list the dumps cover, by tier and family')
    ap.add_argument('--unlisted', action='store_true', help='with --table: only the dumps the list does not know')
    ap.add_argument('--collect', nargs=2, metavar=('SETDIR', 'OUTDIR'))
    ap.add_argument('--jobs', type=int, default=12)
    a = ap.parse_args(argv)
    if a.collect:
        best = collect(*a.collect, jobs=a.jobs)
        coverage(set(best))
        return 0
    paths = [p for g in a.roms for p in (sorted(glob.glob(g)) or [g])]
    paths = [p for p in paths if os.path.isfile(p) and p.lower().endswith(EXTS)]
    if a.summary or a.coverage:
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            carts = list(ex.map(Cart.load, paths))
        if a.summary:
            summary(carts)
        if a.coverage:
            coverage({c.short for c in carts if c.listed})
        return 0
    for p in paths:
        c = Cart.load(p)
        if a.table:
            if a.unlisted and c.listed:
                continue
            sram = f'{c.sram // 1024}K' if c.sram >= 1024 else f'{c.sram}B' if c.sram else '-'
            flags = ('6' if '6' in c.io else '-') + ('!' if c.checksum() == c.hsum else '?') + \
                    ('' if c.mame_supported == 'yes' else c.mame_supported[0])
            print(f'{c.key[:24]:24} {c.tier:5} {c.machine:6} {c.slot[:14]:14} {c.size // 1024:5d}K {sram:>5} '
                  f'{flags:4} {c.crc:08X}  {c.title[:60]}')
            continue
        print(c.name)
        print(f'  {"listed" if c.listed else "NOT in the list"}: {LIST}/{c.short}  {c.title!r}  crc {c.crc:08X}'
              + (f'  clone of {c.parent}' if c.parent else '')
              + ('' if c.mame_supported == 'yes' else f'  MAME: supported="{c.mame_supported}"'))
        if c.listed:
            print(f'  tier {c.tier}{" (" + c.sw.why + ")" if c.sw.why else ""}  {c.sw.year}  {c.sw.publisher}'
                  + (f'  as {c.how}' if c.how != 'exact' else ''))
        print(f'  size {c.size} (dump {c.raw_size})  machine {c.machine}  slot {c.slot}  SRAM {c.sram}')
        print(f'  header: {c.console!r} {c.copyright!r}  serial {c.serial!r}  regions {c.region_field!r}  '
              f'I/O {c.io!r} ({", ".join(c.peripherals)})')
        print(f'    domestic {c.domestic!r}')
        print(f'    overseas {c.overseas!r}')
        print(f'    ROM {c.rom_start:06X}-{c.rom_end:06X}  RAM {c.ram_start:06X}-{c.ram_end:06X}'
              + (f'  SRAM {c.sram_start:06X}-{c.sram_end:06X} type {c.sram_type:02X}' if c.has_sram_header else '')
              + f'  checksum {c.hsum:04X} ({"good" if c.checksum() == c.hsum else f"computed {c.checksum():04X}"})')


if __name__ == '__main__':
    sys.exit(main())

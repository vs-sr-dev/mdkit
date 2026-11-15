"""The 68000 (the Mega Drive's main CPU) instruction set: forms, lengths,
operands and control flow, as the 68000 decodes them.

An opcode word is one of MAME's disassembler *forms* (`add_er_16`,
`movem_pd_32`, `bcc_8`...: the name, mask, match and allowed addressing
modes of m68kdasm.cpp's table, restricted to the 68000), chosen as MAME
chooses: the form whose mask has the most bits, among those that match and
whose effective address is one the form allows.  Everything else is
illegal (the illegal-instruction exception), $Axxx and $Fxxx the line A and
line F exceptions.

An instruction is an `Insn`: address, length, its words, the form, a
mnemonic and size, operands, the condition (Bcc, DBcc, Scc), and its
control flow:

    kind   None (falls through), 'jump' (BRA, JMP to a fixed address),
           'branch' (Bcc), 'dbcc', 'call' (BSR, JSR to a fixed address),
           'jumpind', 'callind' (JMP, JSR through a register), 'ret' (RTS),
           'rtr', 'rte', 'trap' (TRAP #n), 'trapv', 'chk' (exceptions that
           may be taken, else fall through), 'stop', 'reset', 'bad'
           (illegal, line A, line F)
    target the jump's, branch's or call's destination (24 bits), the TRAP's
           vector number

An operand is an `EA` (one of the twelve addressing modes, with its
extension words decoded: displacement, index register, absolute or
PC-relative address, immediate value) or one of 'SR', 'CCR', 'USP', or a
`Regs` (MOVEM's register list).  An EA knows whether it reaches memory and,
when it is absolute or PC-relative, the address it reaches -- what discovery
and the access sites need.

`text()` is MAME's disassembly of the instruction, character for character
(but for the spacing), so that tools/m68kcheck.py can hold the decoder to
unidasm over all 65536 opcode words.

    python -m mdkit.m68k ROM.bin --entry -n 40          # from the reset vector
    python -m mdkit.m68k ROM.bin --at 2A4 -n 20
"""
import argparse
import sys

SIZES = {'b': 1, 'w': 2, 'l': 4}
CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']

# addressing-mode classes (MAME's ea masks)
M_DN, M_AN, M_AI, M_PI, M_PD, M_DI, M_IX, M_AW, M_AL, M_PCDI, M_PCIX, M_IMM = \
    0x800, 0x400, 0x200, 0x100, 0x080, 0x040, 0x020, 0x010, 0x008, 0x002, 0x001, 0x004


def mode_class(ea6):
    """The mask bit of the 6-bit mode/register field (0 for the reserved $3D-$3F)."""
    m, r = ea6 >> 3, ea6 & 7
    if m < 7:
        return (M_DN, M_AN, M_AI, M_PI, M_PD, M_DI, M_IX)[m]
    return {0: M_AW, 1: M_AL, 2: M_PCDI, 3: M_PCIX, 4: M_IMM}.get(r, 0)


# -- the forms: (name, mask, match, ea mask, mnemonic, size, operands, kind) ------------------------------
# operands: 'ea' (bits 5-0), 'dea' (MOVE's destination, bits 11-6), 'Dx'/'Ax' (bits 11-9), 'Dy'/'Ay'
# (bits 2-0), '-Ax'/'-Ay', 'Ax+'/'Ay+', 'is'/'iu' (an immediate of the size, signed or unsigned text),
# 'q' (ADDQ/SUBQ/shift count 1-8), 'mq' (MOVEQ's byte), 'rel8'/'rel16' (a branch), 'SR'/'CCR'/'USP',
# 'regs' (MOVEM's mask word), 'mp' (MOVEP's displacement), 'vec' (TRAP's number), 'i16s' (LINK, STOP)

def _sized(name, base, mask, eam, mnem, ops, kind=None, sizes='bwl', step=0x40):
    out = []
    for i, s in enumerate(sizes):
        em = eam[i] if isinstance(eam, tuple) else eam
        out.append((f'{name}_{8 << i}', mask, base + i * step, em, mnem, s, ops, kind))
    return out


FORMS = [
    ('1010', 0xF000, 0xA000, 0, 'linea', None, (), 'bad'),
    ('1111', 0xF000, 0xF000, 0, 'linef', None, (), 'bad'),
    ('illegal', 0xFFFF, 0x4AFC, 0, 'illegal', None, (), 'bad'),
    ('abcd_rr', 0xF1F8, 0xC100, 0, 'abcd', None, ('Dy', 'Dx'), None),
    ('abcd_mm', 0xF1F8, 0xC108, 0, 'abcd', None, ('-Ay', '-Ax'), None),
    ('sbcd_rr', 0xF1F8, 0x8100, 0, 'sbcd', None, ('Dy', 'Dx'), None),
    ('sbcd_mm', 0xF1F8, 0x8108, 0, 'sbcd', None, ('-Ay', '-Ax'), None),
    ('nbcd', 0xFFC0, 0x4800, 0xBF8, 'nbcd', None, ('ea:b',), None),
]
for _n, _b, _er, _re in (('add', 0xD000, (0xBFF, 0xFFF, 0xFFF), 0x3F8), ('sub', 0x9000, (0xBFF, 0xFFF, 0xFFF), 0x3F8),
                         ('and', 0xC000, 0xBFF, 0x3F8), ('or', 0x8000, 0xBFF, 0x3F8)):
    FORMS += _sized(f'{_n}_er', _b, 0xF1C0, _er, _n, ('ea', 'Dx'))
    FORMS += _sized(f'{_n}_re', _b + 0x100, 0xF1C0, _re, _n, ('Dx', 'ea'))
for _n, _b in (('adda', 0xD0C0), ('suba', 0x90C0), ('cmpa', 0xB0C0)):
    FORMS += _sized(_n, _b, 0xF1C0, 0xFFF, _n, ('ea', 'Ax'), sizes='wl', step=0x100)
for _n, _b, _t in (('ori', 0x0000, 'iu'), ('andi', 0x0200, 'iu'), ('subi', 0x0400, 'is'), ('addi', 0x0600, 'is'),
                   ('eori', 0x0A00, 'iu'), ('cmpi', 0x0C00, 'is')):
    FORMS += _sized(_n, _b, 0xFFC0, 0xBF8, _n, (_t, 'ea'))
for _n, _b in (('ori', 0x0000), ('andi', 0x0200), ('eori', 0x0A00)):
    FORMS += [(f'{_n}_to_ccr', 0xFFFF, _b + 0x3C, 0, _n, None, ('iu:b', 'CCR'), None),
              (f'{_n}_to_sr', 0xFFFF, _b + 0x7C, 0, _n, None, ('iu:w', 'SR'), None)]
FORMS += _sized('addq', 0x5000, 0xF1C0, (0xBF8, 0xFF8, 0xFF8), 'addq', ('q', 'ea'))
FORMS += _sized('subq', 0x5100, 0xF1C0, (0xBF8, 0xFF8, 0xFF8), 'subq', ('q', 'ea'))
for _n, _b in (('addx', 0xD100), ('subx', 0x9100)):
    FORMS += _sized(f'{_n}_rr', _b, 0xF1F8, 0, _n, ('Dy', 'Dx'))
    FORMS += _sized(f'{_n}_mm', _b + 8, 0xF1F8, 0, _n, ('-Ay', '-Ax'))
FORMS += _sized('cmp', 0xB000, 0xF1C0, (0xBFF, 0xFFF, 0xFFF), 'cmp', ('ea', 'Dx'))
FORMS += _sized('cmpm', 0xB108, 0xF1F8, 0, 'cmpm', ('Ay+', 'Ax+'))
FORMS += _sized('eor', 0xB100, 0xF1C0, 0xBF8, 'eor', ('Dx', 'ea'))
# shifts and rotates: register by count (s), by register (r), memory by one (ea)
for _i, _n in enumerate(('as', 'ls', 'rox', 'ro')):
    for _d, _dn in ((0, 'r'), (0x100, 'l')):
        FORMS += _sized(f'{_n}{_dn}_s', 0xE000 + _d + _i * 8, 0xF1F8, 0, f'{_n}{_dn}', ('q', 'Dy'))
        FORMS += _sized(f'{_n}{_dn}_r', 0xE020 + _d + _i * 8, 0xF1F8, 0, f'{_n}{_dn}', ('Dx', 'Dy'))
        FORMS.append((f'{_n}{_dn}_ea', 0xFFC0, 0xE0C0 + _d + _i * 0x200, 0x3F8, f'{_n}{_dn}', 'w', ('ea',), None))
FORMS += [
    ('bra_8', 0xFF00, 0x6000, 0, 'bra', None, ('rel8',), 'jump'),
    ('bra_16', 0xFFFF, 0x6000, 0, 'bra', None, ('rel16',), 'jump'),
    ('bsr_8', 0xFF00, 0x6100, 0, 'bsr', None, ('rel8',), 'call'),
    ('bsr_16', 0xFFFF, 0x6100, 0, 'bsr', None, ('rel16',), 'call'),
    ('bcc_8', 0xF000, 0x6000, 0, 'b', None, ('rel8',), 'branch'),
    ('bcc_16', 0xF0FF, 0x6000, 0, 'b', None, ('rel16',), 'branch'),
    ('dbcc', 0xF0F8, 0x50C8, 0, 'db', None, ('Dy', 'rel16'), 'dbcc'),
    ('dbra', 0xFFF8, 0x51C8, 0, 'dbra', None, ('Dy', 'rel16'), 'dbcc'),
    ('scc', 0xF0C0, 0x50C0, 0xBF8, 's', None, ('ea:b',), None),
]
for _n, _b in (('btst', 0), ('bchg', 0x40), ('bclr', 0x80), ('bset', 0xC0)):
    FORMS += [(f'{_n}_r', 0xF1C0, 0x0100 + _b, 0xBFF if _n == 'btst' else 0xBF8, _n, None, ('Dx', 'ea:b'), None),
              (f'{_n}_s', 0xFFC0, 0x0800 + _b, 0xBFB if _n == 'btst' else 0xBF8, _n, None, ('iu:b', 'ea:b'), None)]
FORMS += [
    ('chk_16', 0xF1C0, 0x4180, 0xBFF, 'chk', 'w', ('ea', 'Dx'), 'chk'),
    ('divs', 0xF1C0, 0x81C0, 0xBFF, 'divs', 'w', ('ea', 'Dx'), None),
    ('divu', 0xF1C0, 0x80C0, 0xBFF, 'divu', 'w', ('ea', 'Dx'), None),
    ('muls', 0xF1C0, 0xC1C0, 0xBFF, 'muls', 'w', ('ea', 'Dx'), None),
    ('mulu', 0xF1C0, 0xC0C0, 0xBFF, 'mulu', 'w', ('ea', 'Dx'), None),
    ('exg_dd', 0xF1F8, 0xC140, 0, 'exg', None, ('Dx', 'Dy'), None),
    ('exg_aa', 0xF1F8, 0xC148, 0, 'exg', None, ('Ax', 'Ay'), None),
    ('exg_da', 0xF1F8, 0xC188, 0, 'exg', None, ('Dx', 'Ay'), None),
    ('ext_16', 0xFFF8, 0x4880, 0, 'ext', 'w', ('Dy',), None),
    ('ext_32', 0xFFF8, 0x48C0, 0, 'ext', 'l', ('Dy',), None),
    ('swap', 0xFFF8, 0x4840, 0, 'swap', None, ('Dy',), None),
    ('jmp', 0xFFC0, 0x4EC0, 0x27B, 'jmp', None, ('ea:l',), 'jump'),
    ('jsr', 0xFFC0, 0x4E80, 0x27B, 'jsr', None, ('ea:l',), 'call'),
    ('lea', 0xF1C0, 0x41C0, 0x27B, 'lea', None, ('ea:l', 'Ax'), None),
    ('pea', 0xFFC0, 0x4840, 0x27B, 'pea', None, ('ea:l',), None),
    ('link_16', 0xFFF8, 0x4E50, 0, 'link', None, ('Ay', 'i16s'), None),
    ('unlk', 0xFFF8, 0x4E58, 0, 'unlk', None, ('Ay',), None),
    ('move_8', 0xF000, 0x1000, 0xBFF, 'move', 'b', ('ea', 'dea'), None),
    ('move_16', 0xF000, 0x3000, 0xFFF, 'move', 'w', ('ea', 'dea'), None),
    ('move_32', 0xF000, 0x2000, 0xFFF, 'move', 'l', ('ea', 'dea'), None),
    ('movea_16', 0xF1C0, 0x3040, 0xFFF, 'movea', 'w', ('ea', 'Ax'), None),
    ('movea_32', 0xF1C0, 0x2040, 0xFFF, 'movea', 'l', ('ea', 'Ax'), None),
    ('move_to_ccr', 0xFFC0, 0x44C0, 0xBFF, 'move', None, ('ea:b', 'CCR'), None),
    ('move_to_sr', 0xFFC0, 0x46C0, 0xBFF, 'move', None, ('ea:w', 'SR'), None),
    ('move_fr_sr', 0xFFC0, 0x40C0, 0xBF8, 'move', None, ('SR', 'ea:w'), None),
    ('move_to_usp', 0xFFF8, 0x4E60, 0, 'move', None, ('Ay', 'USP'), None),
    ('move_fr_usp', 0xFFF8, 0x4E68, 0, 'move', None, ('USP', 'Ay'), None),
    ('movem_pd_16', 0xFFF8, 0x48A0, 0, 'movem', 'w', ('regs', 'ea'), None),
    ('movem_pd_32', 0xFFF8, 0x48E0, 0, 'movem', 'l', ('regs', 'ea'), None),
    ('movem_re_16', 0xFFC0, 0x4880, 0x2F8, 'movem', 'w', ('regs', 'ea'), None),
    ('movem_re_32', 0xFFC0, 0x48C0, 0x2F8, 'movem', 'l', ('regs', 'ea'), None),
    ('movem_er_16', 0xFFC0, 0x4C80, 0x37B, 'movem', 'w', ('regs', 'ea'), None),
    ('movem_er_32', 0xFFC0, 0x4CC0, 0x37B, 'movem', 'l', ('regs', 'ea'), None),
    ('movep_er_16', 0xF1F8, 0x0108, 0, 'movep', 'w', ('mp', 'Dx'), None),
    ('movep_er_32', 0xF1F8, 0x0148, 0, 'movep', 'l', ('mp', 'Dx'), None),
    ('movep_re_16', 0xF1F8, 0x0188, 0, 'movep', 'w', ('Dx', 'mp'), None),
    ('movep_re_32', 0xF1F8, 0x01C8, 0, 'movep', 'l', ('Dx', 'mp'), None),
    ('moveq', 0xF100, 0x7000, 0, 'moveq', None, ('mq', 'Dx'), None),
    ('nop', 0xFFFF, 0x4E71, 0, 'nop', None, (), None),
    ('reset', 0xFFFF, 0x4E70, 0, 'reset', None, (), 'reset'),
    ('stop', 0xFFFF, 0x4E72, 0, 'stop', None, ('i16s',), 'stop'),
    ('rte', 0xFFFF, 0x4E73, 0, 'rte', None, (), 'rte'),
    ('rts', 0xFFFF, 0x4E75, 0, 'rts', None, (), 'ret'),
    ('trapv', 0xFFFF, 0x4E76, 0, 'trapv', None, (), 'trapv'),
    ('rtr', 0xFFFF, 0x4E77, 0, 'rtr', None, (), 'rtr'),
    ('trap', 0xFFF0, 0x4E40, 0, 'trap', None, ('vec',), 'trap'),
    ('tas', 0xFFC0, 0x4AC0, 0xBF8, 'tas', None, ('ea:b',), None),
]
FORMS += _sized('clr', 0x4200, 0xFFC0, 0xBF8, 'clr', ('ea',))
FORMS += _sized('neg', 0x4400, 0xFFC0, 0xBF8, 'neg', ('ea',))
FORMS += _sized('negx', 0x4000, 0xFFC0, 0xBF8, 'negx', ('ea',))
FORMS += _sized('not', 0x4600, 0xFFC0, 0xBF8, 'not', ('ea',))
FORMS += _sized('tst', 0x4A00, 0xFFC0, 0xBF8, 'tst', ('ea',))
del _n, _b, _er, _re, _t, _i, _d, _dn

FORM = {f[0]: f for f in FORMS}
_table = None


def table():
    """opcode word -> form name (None: illegal), chosen as MAME's disassembler builds its table."""
    global _table
    if _table is None:
        order = sorted(FORMS, key=lambda f: -bin(f[1]).count('1'))     # stable: ties keep the list's order
        bynib = [[f for f in order if (f[2] >> 12) == n] for n in range(16)]
        t = [None] * 0x10000
        for op in range(0x10000):
            for name, mask, match, eam, *_ in bynib[op >> 12]:
                if op & mask != match:
                    continue
                if name.startswith('move_') and name[5:] in ('8', '16', '32') and \
                        not mode_class(((op >> 9) & 7) | ((op >> 3) & 0x38)) & 0xBF8:
                    continue
                if eam == 0 or mode_class(op & 0x3F) & eam:
                    t[op] = name
                    break
        _table = t
    return _table


# -- operands ------------------------------------------------------------------------------------------

def _sh(v, bits):
    """MAME's signed hex: $12, -$12."""
    m = 1 << (bits - 1)
    v &= (1 << bits) - 1
    if v == m:
        return f'-${m:x}'
    return f'-${(-v) & (m - 1):x}' if v & m else f'${v & (m - 1):x}'


def _sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class EA:
    """One effective address: mode 0-7 and register 0-7 as the opcode holds them, and what its
    extension words say.  kind: 'dn', 'an', 'ai', 'pi', 'pd', 'di', 'ix', 'aw', 'al', 'pcdi', 'pcix', 'imm'."""
    __slots__ = ('mode', 'reg', 'kind', 'size', 'disp', 'xreg', 'xlong', 'addr', 'value', 'ext_at', 'text')

    KINDS = ('dn', 'an', 'ai', 'pi', 'pd', 'di', 'ix')

    def __init__(self, mode, reg, size):
        self.mode, self.reg, self.size = mode, reg, size
        self.kind = self.KINDS[mode] if mode < 7 else ('aw', 'al', 'pcdi', 'pcix', 'imm', '?', '?', '?')[reg]
        self.disp = self.xreg = self.xlong = self.addr = self.value = self.ext_at = None
        self.text = ''

    @property
    def memory(self):
        return self.kind not in ('dn', 'an', 'imm')

    @property
    def static(self):
        """The address it reaches when that is fixed (absolute, PC-relative), else None."""
        return self.addr

    def __repr__(self):
        return self.text


class Regs:
    """MOVEM's register list: mask bit n = D0-D7 (0-7), A0-A7 (8-15), whatever the order in the word."""
    __slots__ = ('mask', 'word', 'text')

    def __init__(self, word, predec):
        self.word = word
        self.mask = int(f'{word:016b}'[::-1], 2) if predec else word
        parts = []
        for base, ch in ((0, 'D'), (8, 'A')):
            i = 0
            while i < 8:
                if self.mask >> (base + i) & 1:
                    j = i
                    while j < 7 and self.mask >> (base + j + 1) & 1:
                        j += 1
                    parts.append(f'{ch}{i}' + (f'-{ch}{j}' if j > i else ''))
                    i = j + 1
                else:
                    i += 1
        self.text = '/'.join(parts)

    def __repr__(self):
        return self.text


class Insn:
    __slots__ = ('addr', 'words', 'op', 'form', 'mnem', 'size', 'ops', 'cond', 'kind', 'target')

    def __init__(self, addr, op):
        self.addr, self.op, self.words = addr, op, [op]
        self.form = self.mnem = self.size = self.cond = self.kind = self.target = None
        self.ops = []

    @property
    def length(self):
        return 2 * len(self.words)

    @property
    def next(self):
        return (self.addr + self.length) & 0xFFFFFF

    @property
    def ends(self):
        """True when nothing falls through to the next instruction."""
        return self.kind in ('jump', 'jumpind', 'ret', 'rtr', 'rte', 'bad')

    @property
    def name(self):
        """The mnemonic with its condition and size, as MAME writes it."""
        if self.kind == 'bad':
            return 'dc.w'
        m = self.mnem + (CC[self.cond] if self.mnem in ('b', 'db', 's') else '')
        return m + (f'.{self.size}' if self.size else '')

    def text(self):
        """MAME's disassembly of it (one space after the mnemonic)."""
        if self.kind == 'bad':
            what = {'1010': 'opcode 1010', '1111': 'opcode 1111'}.get(self.form, 'ILLEGAL')
            return f'dc.w ${self.op:04x}; {what}'
        args = ', '.join(o if isinstance(o, str) else o.text for o in self.ops)
        return f'{self.name} {args}'.rstrip()

    def __repr__(self):
        return f'{self.addr:06X}  {" ".join(f"{w:04x}" for w in self.words):24} {self.text()}'


class _Reader:
    def __init__(self, read16, insn):
        self.read16, self.insn = read16, insn
        self.pc = (insn.addr + 2) & 0xFFFFFF

    def word(self):
        w = self.read16(self.pc) & 0xFFFF
        self.insn.words.append(w)
        self.pc = (self.pc + 2) & 0xFFFFFF
        return w

    def long(self):
        hi = self.word()
        return hi << 16 | self.word()


def _ea(rd, mode, reg, size, imm_signed=False):
    e = EA(mode, reg, size)
    k = e.kind
    if k == 'dn':
        e.text = f'D{reg}'
    elif k == 'an':
        e.text = f'A{reg}'
    elif k == 'ai':
        e.text = f'(A{reg})'
    elif k == 'pi':
        e.text = f'(A{reg})+'
    elif k == 'pd':
        e.text = f'-(A{reg})'
    elif k in ('di', 'pcdi'):
        e.ext_at = rd.pc
        w = rd.word()
        e.disp = _sx(w, 16)
        if k == 'di':
            e.text = f'({_sh(w, 16)},A{reg})'
        else:
            e.addr = (e.ext_at + e.disp) & 0xFFFFFF
            e.text = f'(${(e.ext_at + e.disp) & 0xFFFFFFFF:x},PC)'
    elif k in ('ix', 'pcix'):
        e.ext_at = rd.pc
        w = rd.word()
        e.disp = _sx(w & 0xFF, 8)
        e.xreg = ('A' if w & 0x8000 else 'D') + str((w >> 12) & 7)
        e.xlong = bool(w & 0x800)
        base = f'A{reg}' if k == 'ix' else 'PC'
        idx = f'{e.xreg}.{"l" if e.xlong else "w"}'
        # the 68000 ignores the scale and full-format bits (8-10); MAME's disassembler calls them invalid
        if w & 0x700:
            e.text = f'INVALID {mode << 3 | reg:x}'
        else:
            e.text = f'({_sh(w, 8)},{base},{idx})' if w & 0xFF else f'({base},{idx})'
    elif k == 'aw':
        w = rd.word()
        e.addr = _sx(w, 16) & 0xFFFFFF
        e.text = f'${w:x}.w'
    elif k == 'al':
        v = rd.long()
        e.addr = v & 0xFFFFFF
        e.text = f'${v:x}.l'
    elif k == 'imm':
        if size == 'l':
            v = rd.long()
        else:
            v = rd.word()
            if size == 'b':
                v &= 0xFF
        e.value = v
        bits = SIZES[size] * 8
        e.text = '#' + (_sh(v, bits) if imm_signed else f'${v:x}')
    else:
        e.text = f'INVALID {mode << 3 | reg:x}'
    return e


def decode(read16, addr):
    """The instruction at `addr`, read16(a) giving the big-endian word at a (24-bit addresses)."""
    a = addr & 0xFFFFFF
    op = read16(a) & 0xFFFF
    i = Insn(a, op)
    name = table()[op]
    if name is None:
        i.form, i.mnem, i.kind = 'illegal', 'illegal', 'bad'
        return i
    _, mask, match, eam, mnem, size, ops, kind = FORM[name]
    i.form, i.mnem, i.size, i.kind = name, mnem, size, kind
    if mnem in ('b', 'db', 's', 'dbra'):
        i.cond = (op >> 8) & 15
    rd = _Reader(read16, i)
    x, y = (op >> 9) & 7, op & 7
    for o in ops:
        sz = size
        if ':' in o:
            o, sz = o.split(':')
        if o == 'ea':
            i.ops.append(_ea(rd, (op >> 3) & 7, y, sz or 'w'))
        elif o == 'dea':
            i.ops.append(_ea(rd, (op >> 6) & 7, x, sz))
        elif o == 'Dx':
            i.ops.append(f'D{x}')
        elif o == 'Dy':
            i.ops.append(f'D{y}')
        elif o == 'Ax':
            i.ops.append(f'A{x}')
        elif o == 'Ay':
            i.ops.append(f'A{y}')
        elif o == '-Ax':
            i.ops.append(f'-(A{x})')
        elif o == '-Ay':
            i.ops.append(f'-(A{y})')
        elif o == 'Ax+':
            i.ops.append(f'(A{x})+')
        elif o == 'Ay+':
            i.ops.append(f'(A{y})+')
        elif o in ('is', 'iu'):
            i.ops.append(_ea(rd, 7, 4, sz, o == 'is'))
        elif o == 'q':
            i.ops.append(f'#{x or 8}')
        elif o == 'mq':
            i.ops.append('#' + _sh(op & 0xFF, 8))
        elif o in ('rel8', 'rel16'):
            base = (a + 2) & 0xFFFFFFFF
            d = _sx(op & 0xFF, 8) if o == 'rel8' else _sx(rd.word(), 16)
            t = (base + d) & 0xFFFFFFFF
            i.target = t & 0xFFFFFF
            i.ops.append(f'${t:x}')
        elif o in ('SR', 'CCR', 'USP'):
            i.ops.append(o)
        elif o == 'regs':
            i.ops.append(Regs(rd.word(), name.startswith('movem_pd')))
        elif o == 'mp':
            w = rd.word()
            e = EA(5, y, size)
            e.disp, e.ext_at, e.text = _sx(w, 16), (rd.pc - 2) & 0xFFFFFF, f'(${w:x},A{y})'
            i.ops.append(e)
        elif o == 'vec':
            i.ops.append(f'#${op & 15:x}')
            i.target = 32 + (op & 15)
        elif o == 'i16s':
            i.ops.append('#' + _sh(rd.word(), 16))
    if name.startswith('movem_er'):
        i.ops.reverse()             # the mask word comes first, but memory is the source
    if name in ('jmp', 'jsr'):
        e = i.ops[0]
        if e.addr is not None and e.kind != 'pcix':
            i.target = e.addr
        else:
            i.kind = 'jumpind' if name == 'jmp' else 'callind'
    if i.kind == 'branch' and i.cond == 0:
        i.kind = 'jump'
    return i


def reader(data, base=0):
    """read16(a) over a ROM image mapped at `base` (words past its end read $FFFF)."""
    n = len(data)

    def read16(a):
        o = (a - base) & 0xFFFFFF
        return data[o] << 8 | data[o + 1] if o + 1 < n else 0xFFFF
    return read16


def main(argv=None):
    from .cart import Cart
    ap = argparse.ArgumentParser(prog='mdkit.m68k')
    ap.add_argument('rom')
    ap.add_argument('--entry', action='store_true', help='from the reset vector ($000004)')
    ap.add_argument('--at', help='address (hex)')
    ap.add_argument('-n', type=int, default=32)
    a = ap.parse_args(argv)
    c = Cart.load(a.rom)
    read16 = reader(c.data)
    addr = (read16(4) << 16 | read16(6)) & 0xFFFFFF
    if a.at:
        addr = int(a.at, 16)
    for _ in range(a.n):
        i = decode(read16, addr)
        print(repr(i))
        addr = i.next


if __name__ == '__main__':
    sys.exit(main())

"""68000 to C++: the Program's instructions as functions over ROM ranges (md_native.h).

A function holds up to CHUNK instructions, by address; its range runs from its first
instruction to the next function's.  A switch sends a way in (any of its instructions)
to its label; at each, the boundary's poll (MD_POLL), then the instruction (MD_OP: the
interpreter's handler, its fetch charged, its cycles added); then on to the next
instruction if that is where the PC went, else back to the switch with the PC -- a
branch to an instruction of the function is a jump in the switch, one elsewhere a return
to the runtime, which finds the function that holds it.

The table md_native[] lists the ranges in order, for the runtime's binary search.
"""
CHUNK = 1500
SPECIAL = True          # write the instructions special() knows in C++ (False: all through the handlers)
FILES = 8


def h(v):
    return f'0x{v:06X}'


def chunks(prog):
    order = sorted(prog.code)
    out = [order[k:k + CHUNK] for k in range(0, len(order), CHUNK)]
    ranges = []
    for n, part in enumerate(out):
        lo = part[0]
        hi = out[n + 1][0] if n + 1 < len(out) else max(a + prog.code[a].length for a in part)
        ranges.append((lo, hi, part))
    return ranges


def _flow(t, have, nxt=None):
    """Go on at t: a goto when t is an instruction of this function, else back to the switch."""
    return f'goto L_{t:06X};' if t in have else 'e = c.pc; goto top;'


def special(i, have):
    """The instruction written out in C++ (the interpreter's charges, in the same order), or None.

    Bcc, BRA, BSR and DBcc to an even address, and MOVEQ."""
    a, op = i.addr, i.op
    pre = f'c.ipc = {h(a)}; c.ir = 0x{op:04X}; c.cyc = 4; '
    end = 'm.t += uint64_t(c.cyc) * CPU_T; c.cyc = 0; m.n_native++; '
    if i.mnem == 'moveq':
        v = op & 0xFF
        v = v - 256 if v & 0x80 else v
        r = (op >> 9) & 7
        return (pre + f'c.pc = {h(i.next)}; c.d[{r}] = 0x{v & 0xFFFFFFFF:08X}u; flags_logic(c, 0x{v & 0xFFFFFFFF:08X}u, 4); '
                + end + _flow(i.next, have))
    if i.mnem == 'b' and i.target is not None and not i.target & 1:
        word = (op & 0xFF) == 0
        cc = (op >> 8) & 15
        t = i.target
        fetch = 'c.cyc += 4; ' if word else ''
        if cc == 1:     # BSR
            return (pre + fetch + f'c.pc = {h(i.next)}; push32(c, {h(i.next)}); c.pc = {h(t)}; c.cyc += {2 if word else 6}; '
                    + end + _flow(t, have))
        if cc == 0:     # BRA
            return pre + fetch + f'c.pc = {h(t)}; c.cyc += {2 if word else 6}; ' + end + _flow(t, have)
        return (pre + fetch + f'if (cond(c, {cc})) {{ c.pc = {h(t)}; c.cyc += {2 if word else 6}; {end}{_flow(t, have)} }} '
                + f'c.pc = {h(i.next)}; c.cyc += 4; ' + end + _flow(i.next, have))
    if i.mnem == 'db' and i.target is not None and not i.target & 1:
        cc = (op >> 8) & 15
        r = op & 7
        t = i.target
        return (pre + f'c.cyc += 4; c.pc = {h(i.next)}; '
                + (f'if (cond(c, {cc})) {{ c.cyc += 4; {end}{_flow(i.next, have)} }} ' if cc != 1 else '')
                + f'{{ const uint16_t n = uint16_t(c.d[{r}] - 1); c.d[{r}] = (c.d[{r}] & 0xFFFF0000u) | n; '
                + f'if (n != 0xFFFF) {{ c.pc = {h(t)}; c.cyc += 2; {end}{_flow(t, have)} }} }} '
                + f'c.cyc += 6; ' + end + _flow(i.next, have))
    return None


def function(prog, lo, hi, addrs, comments=False):
    name = f'R_{lo:06X}'
    have = set(addrs)
    L = [f'void {name}(MD& m, uint32_t e) {{', '    M68K& c = m.cpu;', '    try {', '    top:', '        switch (e) {']
    L += [f'        case {h(a)}: goto L_{a:06X};' for a in addrs]
    L += ['        default: return;', '        }']
    for k, a in enumerate(addrs):
        i = prog.code[a]
        nxt = i.next
        follows = k + 1 < len(addrs) and addrs[k + 1] == nxt     # the next label emitted is the next instruction
        cm = f'  // {i.text()}' if comments else ''
        sp = special(i, have) if SPECIAL else None
        if sp:
            L.append(f'    L_{a:06X}: MD_POLL({h(a)}) {sp}{cm}')
            continue
        tail = (f'if (c.pc != {h(nxt)}) {{ e = c.pc; goto top; }}' + ('' if follows else f' goto L_{nxt:06X};')
                if nxt in have else 'e = c.pc; goto top;')
        L.append(f'    L_{a:06X}: MD_POLL({h(a)}) MD_OP({h(a)}, 0x{i.op:04X}); {tail}{cm}')
    L += ['    } MD_FAULT', '}', '']
    return name, L


def emit(prog, out_dir, crc, comments=False):
    """The C++ files of the recompiled code, in out_dir: rN.cpp and table.cpp."""
    import os
    os.makedirs(out_dir, exist_ok=True)
    rs = chunks(prog)
    files = [[] for _ in range(min(FILES, max(1, len(rs))))]
    names = []
    for k, (lo, hi, part) in enumerate(rs):
        name, L = function(prog, lo, hi, part, comments)
        names.append((lo, hi, name))
        files[k % len(files)].extend(L)
    paths = []
    for n, body in enumerate(files):
        p = os.path.join(out_dir, f'r{n}.cpp')
        with open(p, 'w', newline='\n') as f:
            f.write('// generated by mdkit.recomp: do not edit\n#include "md_native.h"\n\n')
            f.write('\n'.join(body))
        paths.append(p)
    p = os.path.join(out_dir, 'table.cpp')
    with open(p, 'w', newline='\n') as f:
        f.write('// generated by mdkit.recomp: do not edit\n#include "md_native.h"\n\n')
        for _, _, name in names:
            f.write(f'void {name}(MD&, uint32_t);\n')
        f.write('\nconst NativeRange md_native[] = {\n')
        for lo, hi, name in names:
            f.write(f'    {{{h(lo)}, {h(hi)}, {name}}},\n')
        if not names:
            f.write('    {0, 0, nullptr},\n')
        f.write('};\n')
        f.write(f'const int md_native_n = {len(names)};\n')
        f.write(f'const char* md_native_rom = "{crc:08X}";\n')
    paths.append(p)
    return paths

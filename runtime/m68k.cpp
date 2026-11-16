// The 68000 interpreter (see m68k.h): one handler per instruction family, chosen for
// each of the 65536 opcodes when the table is built (an addressing mode the
// instruction does not take gets the illegal handler).  The semantics come from
// cdiPC's core (checked against SingleStepTests/m68000 there); the cycles are the
// 68000's, as MAME's microcoded core counts them.
#include "m68k.h"

M68KHandler m68k_op[65536];         // the handlers by opcode (the recompiled code calls them too)
#define table m68k_op
static bool table_built;

// ---- status register ----

void m68k_set_sr(M68K& c, uint16_t sr) {
    sr &= 0xA71F;
    if ((sr ^ c.sr) & SR_S) {
        if (c.sr & SR_S) { c.ssp = c.a[7]; c.a[7] = c.usp; }
        else { c.usp = c.a[7]; c.a[7] = c.ssp; }
    }
    c.sr = sr;
}
uint32_t m68k_get_usp(const M68K& c) { return supervisor(c) ? c.usp : c.a[7]; }
uint32_t m68k_get_ssp(const M68K& c) { return supervisor(c) ? c.a[7] : c.ssp; }
void m68k_set_sps(M68K& c, uint32_t usp, uint32_t ssp) {
    c.usp = usp; c.ssp = ssp;
    c.a[7] = supervisor(c) ? ssp : usp;
}
static void set_flag(M68K& c, uint16_t f, bool on) { if (on) c.sr |= f; else c.sr &= uint16_t(~f); }

// ---- exceptions ----

static bool trace_ok;

void m68k_exception(M68K& c, int vector, uint32_t pc) {
    const uint16_t old = c.sr;
    m68k_set_sr(c, uint16_t((c.sr | SR_S) & ~SR_T));
    c.cyc += 6;
    // the 68000 stacks the PC's low word, then the SR, then the PC's high word
    c.a[7] -= 6;
    wr16(c, c.a[7] + 4, pc & 0xFFFF);
    wr16(c, c.a[7], old);
    wr16(c, c.a[7] + 2, pc >> 16);
    c.pc = rd32(c, uint32_t(vector) * 4);
    c.cyc += 4;
}

static void exception_here(M68K& c, int vector) {
    trace_ok = false;
    m68k_exception(c, vector, c.ipc);
}

static void group0(M68K& c, const AddressError& e) {
    // the 68000's long frame: the access, the opcode, the SR and the PC
    const uint16_t old = c.sr;
    m68k_set_sr(c, uint16_t((c.sr | SR_S) & ~SR_T));
    push32(c, c.pc);
    push16(c, old);
    push16(c, c.ir);
    push32(c, e.addr);
    push16(c, uint16_t((c.ir & 0xFFE0) | (e.read ? 0x10 : 0) | (e.ins ? 0 : 0x08) | e.fc));
    c.pc = rd32(c, V_ADDRESS * 4);
    c.cyc += 8;
}

static void op_illegal(M68K& c) {
    const int line = c.ir >> 12;
    exception_here(c, line == 0xA ? V_LINE_A : line == 0xF ? V_LINE_F : V_ILLEGAL);
}

static bool privileged(M68K& c) {
    if (supervisor(c)) return true;
    exception_here(c, V_PRIVILEGE);
    return false;
}

// ---- effective addresses ----

enum { EA_D, EA_A, EA_M, EA_I };
struct EA { int kind, reg; uint32_t addr; };

static uint32_t index_ext(M68K& c, uint32_t base, uint16_t ext) {
    const int n = (ext >> 12) & 7;
    uint32_t x = (ext & 0x8000) ? c.a[n] : c.d[n];
    if (!(ext & 0x800)) x = uint32_t(int32_t(int16_t(x)));
    TAINT(if ((base & 0xFFFFFF) < 0xE00000) taint_base(c, (ext >> 12) & 15, x);)   // else a (negative) offset
    c.cyc += 2;
    return base + uint32_t(int32_t(int8_t(ext))) + x;
}

// pd: the 2 cycles -(An) takes as a source (MOVE's destination does without)
static void ea_calc(M68K& c, EA& e, int mode, int reg, int size, bool pd = true) {
    e.reg = reg;
    e.kind = EA_M;
    switch (mode) {
    case 0: e.kind = EA_D; break;
    case 1: e.kind = EA_A; break;
    case 2: e.addr = c.a[reg]; TAINT(taint_base(c, 8 + reg, e.addr);) break;
    case 3:
        e.addr = c.a[reg];
        TAINT(taint_base(c, 8 + reg, e.addr);)
        c.a[reg] += (reg == 7 && size == 1) ? 2 : uint32_t(size);
        break;
    case 4:
        if (pd) c.cyc += 2;
        c.a[reg] -= (reg == 7 && size == 1) ? 2 : uint32_t(size);
        e.addr = c.a[reg];
        TAINT(taint_base(c, 8 + reg, e.addr);)
        break;
    case 5: { const uint32_t b = c.a[reg]; TAINT(taint_base(c, 8 + reg, b);) e.addr = b + uint32_t(int32_t(int16_t(fetch(c)))); break; }
    case 6: { const uint32_t b = c.a[reg]; TAINT(taint_base(c, 8 + reg, b);) e.addr = index_ext(c, b, fetch(c)); break; }
    default:
        switch (reg) {
        case 0: e.addr = uint32_t(int32_t(int16_t(fetch(c)))); break;
        case 1: e.addr = fetch32(c); break;
        case 2: { const uint32_t b = c.pc; e.addr = b + uint32_t(int32_t(int16_t(fetch(c)))); break; }
        case 3: { const uint32_t b = c.pc; e.addr = index_ext(c, b, fetch(c)); break; }
        default:
            e.kind = EA_I;
            e.addr = size == 4 ? fetch32(c) : size == 2 ? fetch(c) : (fetch(c) & 0xFFu);
        }
    }
}

static uint32_t ea_read(M68K& c, const EA& e, int size) {
    switch (e.kind) {
    case EA_D: return c.d[e.reg] & szmask(size);
    case EA_A: return c.a[e.reg] & szmask(size);
    case EA_I: return e.addr;
    default: return rd(c, e.addr, size);
    }
}

static void ea_write(M68K& c, const EA& e, int size, uint32_t v) {
    const uint32_t m = szmask(size);
    switch (e.kind) {
    case EA_D: c.d[e.reg] = (c.d[e.reg] & ~m) | (v & m); break;
    case EA_A: c.a[e.reg] = v; break;
    default: wr(c, e.addr, size, v);
    }
}

static int op_size(int bits) { return bits == 0 ? 1 : bits == 1 ? 2 : 4; }
static int ea_mode(const M68K& c) { return (c.ir >> 3) & 7; }
static int ea_reg(const M68K& c) { return c.ir & 7; }
static int reg9(const M68K& c) { return (c.ir >> 9) & 7; }
static void src_ea(M68K& c, EA& e, int size) { ea_calc(c, e, ea_mode(c), ea_reg(c), size); }

// ---- line 0: immediates, bits, MOVEP ----

static void op_imm(M68K& c) {
    const int size = op_size((c.ir >> 6) & 3), kind = (c.ir >> 9) & 7;
    const uint32_t imm = size == 4 ? fetch32(c) : size == 2 ? fetch(c) : (fetch(c) & 0xFFu);
    EA e;
    src_ea(c, e, size);
    const uint32_t d = ea_read(c, e, size);
    uint32_t r;
    switch (kind) {
    case 0: r = d | imm; flags_logic(c, r, size); break;
    case 1: r = d & imm; flags_logic(c, r, size); break;
    case 2: r = do_sub(c, d, imm, 0, size, true, false); break;
    case 3: r = do_add(c, d, imm, 0, size, true, false); break;
    case 5: r = d ^ imm; flags_logic(c, r, size); break;
    default:
        do_sub(c, d, imm, 0, size, false, false);    // CMPI
        if (e.kind == EA_D && size == 4) c.cyc += 2;
        return;
    }
    if (e.kind == EA_D && size == 4) c.cyc += 4;
    ea_write(c, e, size, r);
}

static void op_imm_ccr_sr(M68K& c) {
    const int kind = (c.ir >> 9) & 7;
    const bool to_sr = c.ir & 0x40;
    if (to_sr && !privileged(c)) return;
    const uint16_t imm = fetch(c);
    uint16_t v = to_sr ? c.sr : (c.sr & 0xFF);
    const uint16_t i = to_sr ? imm : (imm & 0xFF);
    if (kind == 0) v |= i; else if (kind == 1) v &= i; else v ^= i;
    if (to_sr) m68k_set_sr(c, v);
    else c.sr = uint16_t((c.sr & 0xFF00) | (v & 0x1F));
    c.cyc += 12;
}

static void op_bit(M68K& c) {
    const int type = (c.ir >> 6) & 3;
    uint32_t bit = (c.ir & 0x100) ? c.d[reg9(c)] : (fetch(c) & 0xFF);
    EA e;
    const int size = ea_mode(c) == 0 ? 4 : 1;
    src_ea(c, e, size);
    bit &= size == 4 ? 31 : 7;
    const uint32_t v = ea_read(c, e, size);
    set_flag(c, SR_Z, !(v & (1u << bit)));
    if (type == 0) {
        if (size == 4) c.cyc += 2;
        else if (e.kind == EA_I) c.cyc += 2;    // BTST Dn, #imm
        return;
    }
    uint32_t r = v;
    if (type == 1) r ^= 1u << bit;
    else if (type == 2) r &= ~(1u << bit);
    else r |= 1u << bit;
    if (size == 4) c.cyc += (bit >= 16 ? 4 : 2) + (type == 2 ? 2 : 0);
    ea_write(c, e, size, r);
}

static void op_movep(M68K& c) {
    const int dn = reg9(c), opm = (c.ir >> 6) & 3;
    TAINT(taint_base(c, 8 + ea_reg(c), c.a[ea_reg(c)]);)
    const uint32_t addr = c.a[ea_reg(c)] + uint32_t(int32_t(int16_t(fetch(c))));
    const int n = (opm & 1) ? 4 : 2;
    if (opm & 2) {
        for (int i = 0; i < n; i++) wr8(c, addr + 2 * uint32_t(i), c.d[dn] >> (8 * (n - 1 - i)));
    } else {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 8) | rd8(c, addr + 2 * uint32_t(i));
        c.d[dn] = n == 4 ? v : (c.d[dn] & 0xFFFF0000u) | v;
    }
}

// ---- MOVE ----

static void op_move(M68K& c) {
    const int bits = (c.ir >> 12) & 3, size = bits == 1 ? 1 : bits == 3 ? 2 : 4;
    EA s, d;
    src_ea(c, s, size);
    const uint32_t v = ea_read(c, s, size);
    const int dmode = (c.ir >> 6) & 7;
    if (dmode == 1) {                    // MOVEA: no flags, words sign-extended
        c.a[reg9(c)] = size == 2 ? uint32_t(int32_t(int16_t(v))) : v;
        return;
    }
    ea_calc(c, d, dmode, reg9(c), size, false);
    flags_logic(c, v, size);
    if (d.kind == EA_M && dmode == 4 && size == 4) wr32_pd(c, d.addr, v);
    else ea_write(c, d, size, v);
}

static void op_moveq(M68K& c) {
    const uint32_t v = uint32_t(int32_t(int8_t(c.ir)));
    c.d[reg9(c)] = v;
    flags_logic(c, v, 4);
}

// ---- line 4 ----

static void op_negx_neg_clr_not(M68K& c) {
    const int size = op_size((c.ir >> 6) & 3), kind = (c.ir >> 9) & 7;
    EA e;
    src_ea(c, e, size);
    const uint32_t d = ea_read(c, e, size);     // CLR reads too, on a 68000
    uint32_t r;
    switch (kind) {
    case 0: r = do_sub(c, 0, d, (c.sr & SR_X) != 0, size, true, true); break;
    case 1: r = 0; c.sr = uint16_t((c.sr & ~(SR_N | SR_V | SR_C)) | SR_Z); break;
    case 2: r = do_sub(c, 0, d, 0, size, true, false); break;
    default: r = ~d; flags_logic(c, r, size); break;
    }
    if (e.kind == EA_D && size == 4) c.cyc += 2;
    ea_write(c, e, size, r);
}

static void op_move_from_sr(M68K& c) {      // not privileged on the 68000
    EA e;
    src_ea(c, e, 2);
    ea_read(c, e, 2);                          // the 68000 reads before it writes
    ea_write(c, e, 2, c.sr);
    if (e.kind == EA_D) c.cyc += 2;
}

static void op_move_to_ccr_sr(M68K& c) {
    const bool to_sr = c.ir & 0x200;
    if (to_sr && !privileged(c)) return;
    EA e;
    src_ea(c, e, 2);
    const uint16_t v = uint16_t(ea_read(c, e, 2));
    if (to_sr) m68k_set_sr(c, v);
    else c.sr = uint16_t((c.sr & 0xFF00) | (v & 0x1F));
    c.cyc += 8;
}

static void op_chk(M68K& c) {
    EA e;
    src_ea(c, e, 2);
    const int16_t bound = int16_t(ea_read(c, e, 2)), v = int16_t(c.d[reg9(c)]);
    c.sr &= uint16_t(~(SR_Z | SR_V | SR_C));
    if (!v) c.sr |= SR_Z;
    const bool trap = v < 0 || v > bound;
    set_flag(c, SR_N, v < 0);
    // the microcode compares with the bound first (Dn - bound, its sign without the overflow): over it,
    // the trap comes 2 cycles sooner than for a negative Dn
    c.cyc += (v > bound || int16_t(uint16_t(v - bound)) > 0) ? 4 : 6;
    if (trap) m68k_exception(c, V_CHK, c.pc);
}

static void op_lea(M68K& c) {
    EA e;
    src_ea(c, e, 4);
    if (ea_mode(c) == 6 || (ea_mode(c) == 7 && ea_reg(c) == 3)) c.cyc += 2;
    c.a[reg9(c)] = e.addr;
}

static void op_pea(M68K& c) {
    EA e;
    src_ea(c, e, 4);
    if (ea_mode(c) == 6 || (ea_mode(c) == 7 && ea_reg(c) == 3)) c.cyc += 2;
    push32(c, e.addr);
}

// BCD after the real ALU: carries out of bits 3 and 7 decide the corrections, and V is
// set when the correction turned bit 7 on
static uint32_t do_abcd(M68K& c, uint32_t d, uint32_t s) {
    const uint32_t x = (c.sr & SR_X) != 0;
    const uint32_t ss = (d + s + x) & 0x1FF;
    const uint32_t bc = ((d & s) | (~ss & (d | s))) & 0x88;
    const uint32_t dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
    const uint32_t corf = (bc | dc) - ((bc | dc) >> 2);
    const uint32_t res = (ss + corf) & 0x1FF;
    const bool carry = ((bc | (ss & ~res)) >> 7) & 1;
    c.sr &= uint16_t(~(SR_X | SR_N | SR_V | SR_C));
    if (carry) c.sr |= SR_X | SR_C;
    if ((~ss & res) & 0x80) c.sr |= SR_V;
    if (res & 0x80) c.sr |= SR_N;
    if (res & 0xFF) c.sr &= uint16_t(~SR_Z);
    return res & 0xFF;
}

static uint32_t do_sbcd(M68K& c, uint32_t d, uint32_t s) {
    const uint32_t x = (c.sr & SR_X) != 0;
    const uint32_t dd = (d - s - x) & 0x1FF;
    const uint32_t bc = ((~d & s) | (dd & ~(d ^ s))) & 0x88;
    const uint32_t corf = bc - (bc >> 2);
    const uint32_t res = (dd - corf) & 0x1FF;
    const bool carry = ((bc | (~dd & res)) >> 7) & 1;
    c.sr &= uint16_t(~(SR_X | SR_N | SR_V | SR_C));
    if (carry) c.sr |= SR_X | SR_C;
    if ((dd & ~res) & 0x80) c.sr |= SR_V;
    if (res & 0x80) c.sr |= SR_N;
    if (res & 0xFF) c.sr &= uint16_t(~SR_Z);
    return res & 0xFF;
}

static void op_nbcd(M68K& c) {
    EA e;
    src_ea(c, e, 1);
    const uint32_t d = ea_read(c, e, 1);
    if (e.kind == EA_D) c.cyc += 2;
    ea_write(c, e, 1, do_sbcd(c, 0, d));
}

static void op_swap(M68K& c) {
    uint32_t& d = c.d[ea_reg(c)];
    d = (d >> 16) | (d << 16);
    flags_logic(c, d, 4);
}

static void op_ext(M68K& c) {
    uint32_t& d = c.d[ea_reg(c)];
    if (c.ir & 0x40) { d = uint32_t(int32_t(int16_t(d))); flags_logic(c, d, 4); }
    else { d = (d & 0xFFFF0000u) | (uint32_t(int32_t(int8_t(d))) & 0xFFFF); flags_logic(c, d, 2); }
}

static void op_movem(M68K& c) {
    const int size = (c.ir & 0x40) ? 4 : 2, mode = ea_mode(c), reg = ea_reg(c);
    const uint16_t list = fetch(c);
    if (c.ir & 0x400) {
        // memory to registers; words are sign-extended into whole registers
        uint32_t addr;
        if (mode == 3) { addr = c.a[reg]; TAINT(taint_base(c, 8 + reg, addr);) }
        else { EA e; ea_calc(c, e, mode, reg, size); addr = e.addr; }
        for (int i = 0; i < 16; i++)
            if (list & (1 << i)) {
                uint32_t v = rd(c, addr, size);
                if (size == 2) v = uint32_t(int32_t(int16_t(v)));
                if (i < 8) c.d[i] = v; else c.a[i - 8] = v;
                addr += uint32_t(size);
            }
        rd16(c, addr);          // the 68000 reads one word more
        if (mode == 3) c.a[reg] = addr;
    } else if (mode == 4) {
        // to -(An): the list is reversed, A7 first; An itself is stored as it was
        uint32_t addr = c.a[reg];
        TAINT(taint_base(c, 8 + reg, addr);)
        for (int i = 0; i < 16; i++)
            if (list & (1 << i)) {
                addr -= uint32_t(size);
                const int r = 15 - i;
                const uint32_t v = r < 8 ? c.d[r] : c.a[r - 8];
                if (size == 4) wr32_pd(c, addr, v); else wr16(c, addr, v);
            }
        c.a[reg] = addr;
    } else {
        EA e;
        ea_calc(c, e, mode, reg, size);
        uint32_t addr = e.addr;
        for (int i = 0; i < 16; i++)
            if (list & (1 << i)) {
                wr(c, addr, size, i < 8 ? c.d[i] : c.a[i - 8]);
                addr += uint32_t(size);
            }
    }
}

static void op_tst(M68K& c) {
    const int size = op_size((c.ir >> 6) & 3);
    EA e;
    src_ea(c, e, size);
    flags_logic(c, ea_read(c, e, size), size);
}

static void op_tas(M68K& c) {
    EA e;
    src_ea(c, e, 1);
    const uint32_t v = ea_read(c, e, 1);
    flags_logic(c, v, 1);
    if (e.kind == EA_D) { ea_write(c, e, 1, v | 0x80); return; }
    c.cyc += 2;
    if (c.tas_writes) ea_write(c, e, 1, v | 0x80);
    else c.cyc += 4;            // the cycle runs, the write does not reach memory
}

static void op_trap(M68K& c) { m68k_exception(c, V_TRAP + (c.ir & 15), c.pc); }

static void op_link(M68K& c) {
    const int r = ea_reg(c);
    const int32_t disp = int16_t(fetch(c));
    push32(c, c.a[r]);
    c.a[r] = c.a[7];
    c.a[7] += uint32_t(disp);
}

static void op_unlk(M68K& c) {
    const int r = ea_reg(c);
    c.a[7] = c.a[r];
    c.a[r] = pop32(c);
}

static void op_move_usp(M68K& c) {
    if (!privileged(c)) return;
    if (c.ir & 8) c.a[ea_reg(c)] = c.usp;
    else c.usp = c.a[ea_reg(c)];
}

static void op_reset(M68K& c) {
    if (!privileged(c)) return;
    if (c.on_reset) c.on_reset(c.ctx);
    c.cyc += 128;
}

static void op_nop(M68K&) {}

static void op_stop(M68K& c) {
    if (!privileged(c)) return;
    m68k_set_sr(c, fetch(c));
    c.stopped = true;
    c.cyc -= 4;                 // the 68000 stops with the immediate already in its prefetch
}

static void op_rte(M68K& c) {
    if (!privileged(c)) return;
    const uint16_t sr = uint16_t(pop16(c));
    const uint32_t pc = pop32(c);
    m68k_set_sr(c, sr);
    c.pc = pc;
    c.cyc += 4;
}

static void op_rts(M68K& c) { c.pc = pop32(c); c.cyc += 4; }

static void op_trapv(M68K& c) {
    if (c.sr & SR_V) m68k_exception(c, V_TRAPV, c.pc);
}

static void op_rtr(M68K& c) {
    const uint16_t ccr = uint16_t(pop16(c));
    c.sr = uint16_t((c.sr & 0xFF00) | (ccr & 0x1F));
    c.pc = pop32(c);
    c.cyc += 4;
}

static void op_jsr_jmp(M68K& c) {
    EA e;
    src_ea(c, e, 4);
    const int mode = ea_mode(c), reg = ea_reg(c);
    // what the jump costs beyond its words: (An) 4, d16 and abs.w 2, index 4 (2 + its own 2), abs.l 0
    c.cyc += mode == 2 ? 4 : mode == 5 ? 2 : mode == 6 ? 4 : reg == 0 ? 2 : reg == 1 ? 0 : reg == 2 ? 2 : 4;
    if (!(c.ir & 0x40)) push32(c, c.pc);
    c.pc = e.addr;
}

// ---- lines 5, 6, 7 ----

static void op_addq_subq(M68K& c) {
    const int size = op_size((c.ir >> 6) & 3);
    uint32_t q = (c.ir >> 9) & 7;
    if (!q) q = 8;
    if (ea_mode(c) == 1) {               // to an address register: all 32 bits, no flags
        uint32_t& a = c.a[ea_reg(c)];
        a = (c.ir & 0x100) ? a - q : a + q;
        c.cyc += 4;
        return;
    }
    EA e;
    src_ea(c, e, size);
    const uint32_t d = ea_read(c, e, size);
    const uint32_t r = (c.ir & 0x100) ? do_sub(c, d, q, 0, size, true, false) : do_add(c, d, q, 0, size, true, false);
    if (e.kind == EA_D && size == 4) c.cyc += 4;
    ea_write(c, e, size, r);
}

static void op_scc(M68K& c) {
    EA e;
    src_ea(c, e, 1);
    const bool t = cond(c, (c.ir >> 8) & 15);
    if (e.kind != EA_D) ea_read(c, e, 1);
    else if (t) c.cyc += 2;
    ea_write(c, e, 1, t ? 0xFF : 0);
}

static void op_dbcc(M68K& c) {
    const uint32_t base = c.pc;
    const int32_t disp = int16_t(fetch(c));
    if (cond(c, (c.ir >> 8) & 15)) { c.cyc += 4; return; }
    uint32_t& d = c.d[ea_reg(c)];
    const uint16_t n = uint16_t(d - 1);
    d = (d & 0xFFFF0000u) | n;
    if (n != 0xFFFF) { c.pc = base + uint32_t(disp); c.cyc += 2; }
    else c.cyc += 6;
}

static void op_bcc(M68K& c) {
    const uint32_t base = c.pc;
    int32_t disp = int8_t(c.ir);
    const int cc = (c.ir >> 8) & 15;
    const bool word = !disp;
    if (word) disp = int16_t(fetch(c));
    if (cc == 1) {                       // BSR
        push32(c, c.pc);
        c.pc = base + uint32_t(disp);
        c.cyc += word ? 2 : 6;
        return;
    }
    if (cond(c, cc)) { c.pc = base + uint32_t(disp); c.cyc += word ? 2 : 6; }
    else c.cyc += 4;
}

// ---- arithmetic and logic between a register and an operand ----

static void op_logic_arith(M68K& c) {
    const int line = c.ir >> 12, size = op_size((c.ir >> 6) & 3), dn = reg9(c);
    const bool to_ea = c.ir & 0x100;
    EA e;
    src_ea(c, e, size);
    const uint32_t v = ea_read(c, e, size), dv = c.d[dn] & szmask(size);
    uint32_t r;
    if (to_ea) {                         // Dn op <ea> -> <ea>
        switch (line) {
        case 0x8: r = v | dv; flags_logic(c, r, size); break;
        case 0x9: r = do_sub(c, v, dv, 0, size, true, false); break;
        case 0xB: r = v ^ dv; flags_logic(c, r, size); break;
        case 0xC: r = v & dv; flags_logic(c, r, size); break;
        default: r = do_add(c, v, dv, 0, size, true, false); break;
        }
        if (e.kind == EA_D && size == 4) c.cyc += 4;
        ea_write(c, e, size, r);
        return;
    }
    switch (line) {
    case 0x8: r = dv | v; flags_logic(c, r, size); break;
    case 0x9: r = do_sub(c, dv, v, 0, size, true, false); break;
    case 0xB: do_sub(c, dv, v, 0, size, false, false); if (size == 4) c.cyc += 2; return;    // CMP
    case 0xC: r = dv & v; flags_logic(c, r, size); break;
    default: r = do_add(c, dv, v, 0, size, true, false); break;
    }
    if (size == 4) c.cyc += (e.kind == EA_D || e.kind == EA_A || e.kind == EA_I) ? 4 : 2;
    c.d[dn] = (c.d[dn] & ~szmask(size)) | r;
}

static void op_addr_arith(M68K& c) {
    // ADDA, SUBA, CMPA: the operand sign-extended to 32 bits
    const int size = (c.ir & 0x100) ? 4 : 2, line = c.ir >> 12;
    EA e;
    src_ea(c, e, size);
    uint32_t v = ea_read(c, e, size);
    if (size == 2) v = uint32_t(int32_t(int16_t(v)));
    uint32_t& a = c.a[reg9(c)];
    if (line == 0xB) { do_sub(c, a, v, 0, 4, false, false); c.cyc += 2; }
    else {
        a = line == 0x9 ? a - v : a + v;
        c.cyc += (size == 2 || e.kind == EA_D || e.kind == EA_A || e.kind == EA_I) ? 4 : 2;
    }
}

static void op_addx_subx(M68K& c) {
    const int size = op_size((c.ir >> 6) & 3), rx = reg9(c), ry = ea_reg(c);
    const bool add = (c.ir >> 12) == 0xD;
    const uint32_t x = (c.sr & SR_X) != 0;
    if (c.ir & 8) {                      // -(Ay), -(Ax)
        EA s, d;
        ea_calc(c, s, 4, ry, size);
        const uint32_t sv = ea_read(c, s, size);
        ea_calc(c, d, 4, rx, size, false);
        const uint32_t dv = ea_read(c, d, size);
        const uint32_t r = add ? do_add(c, dv, sv, x, size, true, true) : do_sub(c, dv, sv, x, size, true, true);
        if (size == 4) wr32_pd(c, d.addr, r); else ea_write(c, d, size, r);
    } else {
        const uint32_t r = add ? do_add(c, c.d[rx], c.d[ry], x, size, true, true)
                               : do_sub(c, c.d[rx], c.d[ry], x, size, true, true);
        c.d[rx] = (c.d[rx] & ~szmask(size)) | r;
        if (size == 4) c.cyc += 4;
    }
}

static void op_abcd_sbcd(M68K& c) {
    const int rx = reg9(c), ry = ea_reg(c);
    const bool add = (c.ir >> 12) == 0xC;
    if (c.ir & 8) {
        EA s, d;
        ea_calc(c, s, 4, ry, 1);
        const uint32_t sv = ea_read(c, s, 1);
        ea_calc(c, d, 4, rx, 1, false);
        const uint32_t dv = ea_read(c, d, 1);
        ea_write(c, d, 1, add ? do_abcd(c, dv, sv) : do_sbcd(c, dv, sv));
    } else {
        const uint32_t r = add ? do_abcd(c, c.d[rx] & 0xFF, c.d[ry] & 0xFF) : do_sbcd(c, c.d[rx] & 0xFF, c.d[ry] & 0xFF);
        c.d[rx] = (c.d[rx] & ~0xFFu) | r;
        c.cyc += 2;
    }
}

static void op_cmpm(M68K& c) {
    const int size = op_size((c.ir >> 6) & 3);
    EA s, d;
    ea_calc(c, s, 3, ea_reg(c), size);
    const uint32_t sv = ea_read(c, s, size);
    ea_calc(c, d, 3, reg9(c), size);
    const uint32_t dv = ea_read(c, d, size);
    do_sub(c, dv, sv, 0, size, false, false);
}

static void op_exg(M68K& c) {
    const int x = reg9(c), y = ea_reg(c), mode = (c.ir >> 3) & 0x1F;
    uint32_t& rx = mode == 9 ? c.a[x] : c.d[x];
    uint32_t& ry = mode == 8 ? c.d[y] : c.a[y];
    const uint32_t t = rx;
    rx = ry;
    ry = t;
    c.cyc += 2;
}

static void op_mul(M68K& c) {
    EA e;
    src_ea(c, e, 2);
    const uint32_t s = ea_read(c, e, 2) & 0xFFFF;
    uint32_t& d = c.d[reg9(c)];
    uint32_t r;
    int n = 0;
    if (c.ir & 0x100) {
        r = uint32_t(int32_t(int16_t(s)) * int32_t(int16_t(d)));
        const uint32_t p = (s << 1) ^ s;           // bit pairs that differ in s:0
        for (int i = 0; i < 16; i++) n += (p >> i) & 1;
    } else {
        r = s * (d & 0xFFFF);
        for (int i = 0; i < 16; i++) n += (s >> i) & 1;
    }
    d = r;
    flags_logic(c, r, 4);
    c.cyc += 34 + 2 * n;
}

// the division's cycles as the 68000's microcode spends them (Jorge Cwik's analysis)
static int divu_cycles(uint32_t dividend, uint16_t divisor) {
    if ((dividend >> 16) >= divisor) return 10 - 4;
    int mc = 38;
    const uint32_t hd = uint32_t(divisor) << 16;
    for (int i = 0; i < 15; i++) {
        const uint32_t t = dividend;
        dividend <<= 1;
        if (int32_t(t) < 0) dividend -= hd;
        else {
            mc += 2;
            if (dividend >= hd) { dividend -= hd; mc--; }
        }
    }
    return mc * 2 - 4;
}

static int divs_cycles(int32_t dividend, int16_t divisor) {
    int mc = 6;
    if (dividend < 0) mc++;
    const uint32_t ad = dividend < 0 ? uint32_t(-int64_t(dividend)) : uint32_t(dividend);
    const uint32_t av = divisor < 0 ? uint32_t(-int32_t(divisor)) : uint32_t(divisor);
    if ((ad >> 16) >= av) return (mc + 2) * 2 - 4;
    uint32_t q = ad / av;
    mc += 55;
    if (divisor >= 0) { if (dividend >= 0) mc--; else mc++; }
    for (int i = 0; i < 15; i++) {
        if (int16_t(q) >= 0) mc++;
        q <<= 1;
    }
    return mc * 2 - 4;
}

static void op_div(M68K& c) {
    EA e;
    src_ea(c, e, 2);
    const uint32_t s = ea_read(c, e, 2);
    uint32_t& d = c.d[reg9(c)];
    if (!(s & 0xFFFF)) {
        c.sr &= uint16_t(~(SR_N | SR_Z | SR_V | SR_C));
        c.cyc += 4 + 4;
        m68k_exception(c, V_ZERO_DIVIDE, c.pc);
        return;
    }
    if (c.ir & 0x100) {
        const int32_t dividend = int32_t(d), divisor = int16_t(s);
        c.cyc += divs_cycles(dividend, int16_t(divisor));
        if (dividend == INT32_MIN && divisor == -1) {
            c.sr = uint16_t((c.sr & ~(SR_N | SR_Z | SR_C)) | SR_V | SR_N);
            return;
        }
        const int32_t q = dividend / divisor, r = dividend % divisor;
        if (q < -32768 || q > 32767) {
            c.sr = uint16_t((c.sr & ~(SR_N | SR_Z | SR_C)) | SR_V | SR_N);
            return;
        }
        d = (uint32_t(r) << 16) | (uint32_t(q) & 0xFFFF);
        flags_logic(c, uint32_t(q), 2);
    } else {
        const uint32_t divisor = s & 0xFFFF, q = d / divisor, r = d % divisor;
        c.cyc += divu_cycles(d, uint16_t(divisor));
        if (q > 0xFFFF) {
            c.sr = uint16_t((c.sr & ~(SR_N | SR_Z | SR_C)) | SR_V | SR_N);
            return;
        }
        d = (r << 16) | q;
        flags_logic(c, q, 2);
    }
}

// ---- shifts and rotates ----

static uint32_t do_shift(M68K& c, int type, bool left, uint32_t v, int count, int size) {
    const uint32_t m = szmask(size), top = szmsb(size);
    v &= m;
    bool carry = false, over = false, x = (c.sr & SR_X) != 0;
    for (int i = 0; i < count; i++) {
        if (left) {
            carry = v & top;
            uint32_t in = 0;
            if (type == 2) in = x;
            else if (type == 3) in = carry;
            const uint32_t nv = ((v << 1) | in) & m;
            if (type == 0 && ((nv ^ v) & top)) over = true;
            v = nv;
        } else {
            carry = v & 1;
            uint32_t in = 0;
            if (type == 0) in = v & top;
            else if (type == 2) in = x ? top : 0;
            else if (type == 3) in = carry ? top : 0;
            v = (v >> 1) | in;
        }
        if (type != 3) x = carry;
    }
    c.sr &= uint16_t(~(SR_N | SR_Z | SR_V | SR_C));
    if (count) {
        if (type != 3) set_flag(c, SR_X, x);
        if (carry) c.sr |= SR_C;
    } else if (type == 2 && x) c.sr |= SR_C;      // ROXd by 0: C is X
    if (over) c.sr |= SR_V;
    if (!v) c.sr |= SR_Z;
    if (v & top) c.sr |= SR_N;
    c.cyc += 2 * count;
    return v;
}

static void op_shift_reg(M68K& c) {
    const int size = op_size((c.ir >> 6) & 3), type = (c.ir >> 3) & 3;
    const bool left = c.ir & 0x100;
    int count = reg9(c);
    if (c.ir & 0x20) count = int(c.d[count] & 63);
    else if (!count) count = 8;
    uint32_t& d = c.d[ea_reg(c)];
    const uint32_t r = do_shift(c, type, left, d, count, size);
    d = (d & ~szmask(size)) | r;
    c.cyc += size == 4 ? 4 : 2;
}

static void op_shift_mem(M68K& c) {
    const int type = (c.ir >> 9) & 3;
    const bool left = c.ir & 0x100;
    EA e;
    src_ea(c, e, 2);
    const uint32_t v = ea_read(c, e, 2);
    const uint32_t r = do_shift(c, type, left, v, 1, 2);
    c.cyc -= 2;                 // one step in memory costs nothing beyond the read and the write
    ea_write(c, e, 2, r);
}

// ---- the opcode table ----

enum {
    A_DREG = 1, A_AREG = 2, A_IND = 4, A_POST = 8, A_PRE = 16, A_D16 = 32, A_IDX = 64,
    A_ABSW = 128, A_ABSL = 256, A_PCD = 512, A_PCX = 1024, A_IMM = 2048,
    A_ALL = 4095,
    A_DATA = A_ALL & ~A_AREG,
    A_MEM = A_ALL & ~(A_DREG | A_AREG),
    A_CTRL = A_IND | A_D16 | A_IDX | A_ABSW | A_ABSL | A_PCD | A_PCX,
    A_ALT = A_ALL & ~(A_PCD | A_PCX | A_IMM),
    A_DALT = A_DATA & A_ALT,
    A_MALT = A_MEM & A_ALT,
    A_CALT = A_CTRL & A_ALT,
};

static bool ea_ok(int mode, int reg, int allowed) {
    const int bit = mode < 7 ? 1 << mode : reg <= 4 ? 128 << reg : 0;
    return bit & allowed;
}
static void set(int op, M68KHandler h, int allowed) {
    if (ea_ok((op >> 3) & 7, op & 7, allowed)) table[op] = h;
}

static void build() {
    for (int op = 0; op < 65536; op++) table[op] = op_illegal;
    for (int op = 0; op < 65536; op++) {
        const int line = op >> 12, sz = (op >> 6) & 3, mode = (op >> 3) & 7;
        switch (line) {
        case 0x0:
            if (op & 0x100) {
                if (mode == 1) table[op] = op_movep;
                else set(op, op_bit, ((op >> 6) & 3) == 0 ? A_DATA : A_DALT);
            } else if (((op >> 9) & 7) == 4)
                set(op, op_bit, sz == 0 ? (A_DATA & ~A_IMM) : A_DALT);
            else if (sz != 3 && ((op >> 9) & 7) != 7) {
                const int kind = (op >> 9) & 7;
                if ((op & 0x3F) == 0x3C && (kind == 0 || kind == 1 || kind == 5) && sz <= 1)
                    table[op] = op_imm_ccr_sr;
                else if (kind != 7) set(op, op_imm, A_DALT);
            }
            break;
        case 0x1: case 0x2: case 0x3: {
            const int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
            int src = A_ALL;
            if (line == 1) src &= ~A_AREG;
            if (dmode == 1 && line == 1) break;
            if (dmode != 1 && !ea_ok(dmode, dreg, A_DALT)) break;
            set(op, op_move, src);
            break;
        }
        case 0x4:
            if ((op & 0xF100) == 0x4100) {      // 111 LEA, 110 CHK.W
                if (sz == 3) set(op, op_lea, A_CTRL);
                else if (sz == 2) set(op, op_chk, A_DATA);
                break;
            }
            switch ((op >> 8) & 0xF) {
            case 0x0: case 0x2: case 0x4: case 0x6:
                if (sz != 3) set(op, op_negx_neg_clr_not, A_DALT);
                else if (((op >> 8) & 0xF) == 0x0) set(op, op_move_from_sr, A_DALT);
                else if (((op >> 8) & 0xF) == 0x4 || ((op >> 8) & 0xF) == 0x6) set(op, op_move_to_ccr_sr, A_DATA);
                break;
            case 0x8:
                if (sz == 0) set(op, op_nbcd, A_DALT);
                else if (sz == 1) { if (mode == 0) table[op] = op_swap; else set(op, op_pea, A_CTRL); }
                else if (mode == 0) table[op] = op_ext;
                else set(op, op_movem, A_CALT | A_PRE);
                break;
            case 0xC:
                if (sz >= 2) set(op, op_movem, A_CTRL | A_POST);
                break;
            case 0xA:
                if (op == 0x4AFC) break;
                if (sz != 3) set(op, op_tst, A_DALT);
                else set(op, op_tas, A_DALT);
                break;
            case 0xE:
                if ((op & 0xFFF0) == 0x4E40) table[op] = op_trap;
                else if ((op & 0xFFF8) == 0x4E50) table[op] = op_link;
                else if ((op & 0xFFF8) == 0x4E58) table[op] = op_unlk;
                else if ((op & 0xFFF0) == 0x4E60) table[op] = op_move_usp;
                else if (op == 0x4E70) table[op] = op_reset;
                else if (op == 0x4E71) table[op] = op_nop;
                else if (op == 0x4E72) table[op] = op_stop;
                else if (op == 0x4E73) table[op] = op_rte;
                else if (op == 0x4E75) table[op] = op_rts;
                else if (op == 0x4E76) table[op] = op_trapv;
                else if (op == 0x4E77) table[op] = op_rtr;
                else if (sz >= 2) set(op, op_jsr_jmp, A_CTRL);
                break;
            }
            break;
        case 0x5:
            if (sz == 3) { if (mode == 1) table[op] = op_dbcc; else set(op, op_scc, A_DALT); }
            else set(op, op_addq_subq, sz == 0 ? A_DALT : A_ALT);
            break;
        case 0x6: table[op] = op_bcc; break;
        case 0x7: if (!(op & 0x100)) table[op] = op_moveq; break;
        case 0x8: case 0xC:
            if (sz == 3) set(op, line == 8 ? op_div : op_mul, A_DATA);
            else if ((op & 0x1F0) == 0x100) table[op] = op_abcd_sbcd;
            else if (line == 0xC && ((op & 0x1F8) == 0x140 || (op & 0x1F8) == 0x148 || (op & 0x1F8) == 0x188))
                table[op] = op_exg;
            else if (op & 0x100) set(op, op_logic_arith, A_MALT);
            else set(op, op_logic_arith, A_DATA);
            break;
        case 0x9: case 0xD:
            if (sz == 3) set(op, op_addr_arith, A_ALL);
            else if ((op & 0x130) == 0x100) table[op] = op_addx_subx;
            else if (op & 0x100) set(op, op_logic_arith, A_MALT);
            else set(op, op_logic_arith, sz == 0 ? A_DATA : A_ALL);
            break;
        case 0xB:
            if (sz == 3) set(op, op_addr_arith, A_ALL);
            else if (op & 0x100) { if (mode == 1) table[op] = op_cmpm; else set(op, op_logic_arith, A_DALT); }
            else set(op, op_logic_arith, sz == 0 ? A_DATA : A_ALL);
            break;
        case 0xE:
            if (sz == 3) { if (!(op & 0x800)) set(op, op_shift_mem, A_MALT); }
            else table[op] = op_shift_reg;
            break;
        }
    }
    table_built = true;
}

void m68k_init() { if (!table_built) build(); }

void m68k_fault(M68K& c, const AddressError& e) {
    try { group0(c, e); } catch (const AddressError&) { c.halted = true; }
}

bool m68k_legal(uint16_t op) {
    if (!table_built) build();
    return table[op] != op_illegal;
}

// ---- running ----

void m68k_reset(M68K& c) {
    if (!table_built) build();
    c.stopped = c.halted = c.trace_pending = false;
    c.sr = SR_S | 0x0700;
    c.cyc = 0;
    try {
        c.a[7] = rd32(c, 0);
        c.pc = rd32(c, 4);
    } catch (const AddressError&) {
        c.halted = true;
    }
    c.cyc = 132;
}

void m68k_set_irq(M68K& c, int level) {
    if (level == 7 && c.ipl != 7) c.nmi_pending = true;
    c.ipl = level;
}

static void interrupt(M68K& c, int level) {
    c.stopped = false;
    int vector = c.iack ? c.iack(c.ctx, level) : -1;
    if (vector < 0) vector = V_AUTOVECTOR + level;
    c.cyc += 10;
    m68k_exception(c, vector, c.pc);
    c.sr = uint16_t((c.sr & ~0x0700) | (level << 8));
    c.cyc += 4;
}

int m68k_step(M68K& c) {
    if (!table_built) build();
    c.cyc = 0;
    if (c.halted) return c.cyc = 4;
    try {
        try {
            if (c.trace_pending) {
                c.trace_pending = false;
                c.cyc += 4;
                m68k_exception(c, V_TRACE, c.pc);
                return c.cyc;
            }
            const int mask_level = (c.sr >> 8) & 7;
            if (c.ipl > mask_level || (c.ipl == 7 && c.nmi_pending)) {
                if (c.ipl == 7) c.nmi_pending = false;
                interrupt(c, c.ipl);
                return c.cyc;
            }
            if (c.stopped) return c.cyc = 4;
            const bool trace = c.sr & SR_T;
            trace_ok = true;
            c.ipc = c.pc;
            c.ir = fetch(c);
            table[c.ir](c);
            // a jump to an odd address faults at once: the CPU prefetches from the target
            if (c.pc & 1) address_error(c, c.pc, true, true, prog_fc(c));
            if (trace && trace_ok) c.trace_pending = true;
        } catch (const AddressError& e) {
            group0(c, e);       // a second one while stacking the first halts the CPU
        }
    } catch (const AddressError&) {
        c.halted = true;
    }
    return c.cyc;
}

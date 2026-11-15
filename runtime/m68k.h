// The 68000 under the Mega Drive's recompiled code: its state, the bus it sees, and
// the helpers the interpreter and the generated C++ are both written in.
//
// The bus is in 64 KB pages over the 24-bit address space: a page that is memory
// (ROM, work RAM and their mirrors) is read and written directly, big-endian, as
// the 68000 sees it; any other goes through the machine's I/O handlers.
//
// Cycles: every bus word costs 4 (a fetch, a read, a write; a long is two words),
// charged as the access is made, and each instruction adds its internal cycles
// where the 68000 spends them, so that an instruction's total is the one MAME's
// microcoded 68000 takes (SingleStepTests/m68000, which that core generated, holds
// the interpreter to it: tests/m68ktest).  The words are counted where the
// instruction reads them, not two ahead as the prefetch queue does; the totals
// are the same.  A word access to an odd address is an address error (a C++
// exception out of the instruction, taken by the step).
#pragma once
#include <cstdint>
#include <cstring>

enum : uint16_t { SR_C = 1, SR_V = 2, SR_Z = 4, SR_N = 8, SR_X = 16, SR_S = 0x2000, SR_T = 0x8000 };
enum { V_BUS = 2, V_ADDRESS, V_ILLEGAL, V_ZERO_DIVIDE, V_CHK, V_TRAPV, V_PRIVILEGE, V_TRACE, V_LINE_A,
       V_LINE_F, V_SPURIOUS = 24, V_AUTOVECTOR = 24, V_TRAP = 32 };

struct AddressError {
    uint32_t addr; bool read, ins; int fc;
};

struct M68K {
    // ---- registers ----
    uint32_t d[8] = {}, a[8] = {};   // a[7] is the active stack pointer
    uint32_t usp = 0, ssp = 0;       // the inactive one is kept here
    uint32_t pc = 0;                 // the next word to fetch
    uint16_t sr = SR_S | 0x0700;
    uint16_t ir = 0;                 // the opcode being run
    uint32_t ipc = 0;                // the address of the instruction being run
    int ipl = 0;                     // the interrupt level asked for (0-7)
    bool nmi_pending = false;        // level 7 is edge-triggered
    bool stopped = false, halted = false;
    bool trace_pending = false;
    bool tas_writes = true;          // the Mega Drive's bus drops TAS's write: false there
    int cyc = 0;                     // cycles of the instruction being run (so far)

    // ---- the bus ----
    uint8_t* rpage[256] = {};        // a page's memory, read (nullptr: I/O)
    uint8_t* wpage[256] = {};        // ... written (nullptr: I/O; ROM is read-only)
    uint32_t pmask[256] = {};        // offset mask within the page's memory (mirrors)
    void* ctx = nullptr;
    uint8_t (*io_r8)(void*, uint32_t) = nullptr;
    uint16_t (*io_r16)(void*, uint32_t) = nullptr;
    void (*io_w8)(void*, uint32_t, uint8_t) = nullptr;
    void (*io_w16)(void*, uint32_t, uint16_t) = nullptr;
    int (*iack)(void*, int level) = nullptr;     // the vector, or -1 for the autovector
    void (*on_reset)(void*) = nullptr;           // RESET pulsed the line

    void map(uint32_t first_page, uint32_t pages, uint8_t* mem, uint32_t mask, bool writable) {
        for (uint32_t p = first_page; p < first_page + pages && p < 256; p++) {
            rpage[p] = mem + (((p - first_page) << 16) & mask & ~0xFFFFu);
            wpage[p] = writable ? rpage[p] : nullptr;
            pmask[p] = mask & 0xFFFF;
        }
    }
};

// ---- raw bus access (no cycles) ----

inline uint8_t bus_r8(M68K& c, uint32_t a) {
    a &= 0xFFFFFF;
    const uint8_t* p = c.rpage[a >> 16];
    return p ? p[a & c.pmask[a >> 16]] : c.io_r8(c.ctx, a);
}
inline uint16_t bus_r16(M68K& c, uint32_t a) {
    a &= 0xFFFFFF;
    const uint8_t* p = c.rpage[a >> 16];
    if (p) { const uint32_t o = a & c.pmask[a >> 16]; return uint16_t(p[o] << 8 | p[o + 1]); }
    return c.io_r16(c.ctx, a);
}
inline void bus_w8(M68K& c, uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    uint8_t* p = c.wpage[a >> 16];
    if (p) p[a & c.pmask[a >> 16]] = v;
    else if (!c.rpage[a >> 16]) c.io_w8(c.ctx, a, v);
}
inline void bus_w16(M68K& c, uint32_t a, uint16_t v) {
    a &= 0xFFFFFF;
    uint8_t* p = c.wpage[a >> 16];
    if (p) { const uint32_t o = a & c.pmask[a >> 16]; p[o] = uint8_t(v >> 8); p[o + 1] = uint8_t(v); }
    else if (!c.rpage[a >> 16]) c.io_w16(c.ctx, a, v);
}

// ---- the 68000's accesses (cycles, address errors) ----

inline bool supervisor(const M68K& c) { return c.sr & SR_S; }
inline int data_fc(const M68K& c) { return supervisor(c) ? 5 : 1; }
inline int prog_fc(const M68K& c) { return supervisor(c) ? 6 : 2; }

[[noreturn]] inline void address_error(M68K& c, uint32_t a, bool read, bool ins, int fc) {
    throw AddressError{a, read, ins, fc};
}

inline uint16_t fetch(M68K& c) {
    if (c.pc & 1) address_error(c, c.pc, true, true, prog_fc(c));
    const uint16_t v = bus_r16(c, c.pc);
    c.pc += 2;
    c.cyc += 4;
    return v;
}
inline uint32_t fetch32(M68K& c) { const uint32_t hi = fetch(c); return hi << 16 | fetch(c); }

inline uint32_t rd8(M68K& c, uint32_t a) { c.cyc += 4; return bus_r8(c, a); }
inline uint32_t rd16(M68K& c, uint32_t a) {
    if (a & 1) address_error(c, a, true, false, data_fc(c));
    c.cyc += 4;
    return bus_r16(c, a);
}
inline uint32_t rd32(M68K& c, uint32_t a) {
    if (a & 1) address_error(c, a, true, false, data_fc(c));
    c.cyc += 8;
    const uint32_t hi = bus_r16(c, a);
    return hi << 16 | bus_r16(c, a + 2);
}
inline void wr8(M68K& c, uint32_t a, uint32_t v) { c.cyc += 4; bus_w8(c, a, uint8_t(v)); }
inline void wr16(M68K& c, uint32_t a, uint32_t v) {
    if (a & 1) address_error(c, a, false, false, data_fc(c));
    c.cyc += 4;
    bus_w16(c, a, uint16_t(v));
}
// a long write: the high word first (MAME's microcode), except to -(An), which writes the low word first
inline void wr32(M68K& c, uint32_t a, uint32_t v) {
    if (a & 1) address_error(c, a, false, false, data_fc(c));
    c.cyc += 8;
    bus_w16(c, a, uint16_t(v >> 16));
    bus_w16(c, a + 2, uint16_t(v));
}
inline void wr32_pd(M68K& c, uint32_t a, uint32_t v) {
    if (a & 1) address_error(c, a, false, false, data_fc(c));
    c.cyc += 8;
    bus_w16(c, a + 2, uint16_t(v));
    bus_w16(c, a, uint16_t(v >> 16));
}
inline uint32_t rd(M68K& c, uint32_t a, int size) {
    return size == 1 ? rd8(c, a) : size == 2 ? rd16(c, a) : rd32(c, a);
}
inline void wr(M68K& c, uint32_t a, int size, uint32_t v) {
    if (size == 1) wr8(c, a, v); else if (size == 2) wr16(c, a, v); else wr32(c, a, v);
}

inline void push16(M68K& c, uint32_t v) { c.a[7] -= 2; wr16(c, c.a[7], v); }
inline void push32(M68K& c, uint32_t v) { c.a[7] -= 4; wr32_pd(c, c.a[7], v); }
inline uint32_t pop16(M68K& c) { const uint32_t v = rd16(c, c.a[7]); c.a[7] += 2; return v; }
inline uint32_t pop32(M68K& c) { const uint32_t v = rd32(c, c.a[7]); c.a[7] += 4; return v; }

// ---- flags ----

inline uint32_t szmask(int size) { return size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1; }
inline uint32_t szmsb(int size) { return 1u << (size * 8 - 1); }

inline void flags_logic(M68K& c, uint32_t r, int size) {
    c.sr &= uint16_t(~(SR_N | SR_Z | SR_V | SR_C));
    r &= szmask(size);
    if (!r) c.sr |= SR_Z;
    if (r & szmsb(size)) c.sr |= SR_N;
}

// d + s + x; setx 0 keeps X; keepz (ADDX) clears Z only on a non-zero result
inline uint32_t do_add(M68K& c, uint32_t d, uint32_t s, uint32_t x, int size, bool setx, bool keepz) {
    const uint32_t m = szmask(size);
    d &= m; s &= m;
    const uint64_t full = uint64_t(d) + s + x;
    const uint32_t r = uint32_t(full) & m;
    const bool carry = (full >> (size * 8)) & 1;
    const bool over = (~(d ^ s) & (d ^ r)) & szmsb(size);
    const uint16_t keep = setx ? 0 : (c.sr & SR_X);
    const bool z = keepz ? (r ? false : (c.sr & SR_Z) != 0) : !r;
    c.sr &= uint16_t(~(SR_X | SR_N | SR_Z | SR_V | SR_C));
    c.sr |= keep;
    if (carry) c.sr |= SR_C | (setx ? SR_X : 0);
    if (over) c.sr |= SR_V;
    if (z) c.sr |= SR_Z;
    if (r & szmsb(size)) c.sr |= SR_N;
    return r;
}

// d - s - x; setx 0 for CMP (X untouched)
inline uint32_t do_sub(M68K& c, uint32_t d, uint32_t s, uint32_t x, int size, bool setx, bool keepz) {
    const uint32_t m = szmask(size);
    d &= m; s &= m;
    const uint32_t r = (d - s - x) & m;
    const bool borrow = (uint64_t(s) + x) > d;
    const bool over = ((d ^ s) & (d ^ r)) & szmsb(size);
    const uint16_t keep = setx ? 0 : (c.sr & SR_X);
    const bool z = keepz ? (r ? false : (c.sr & SR_Z) != 0) : !r;
    c.sr &= uint16_t(~(SR_X | SR_N | SR_Z | SR_V | SR_C));
    c.sr |= keep;
    if (borrow) c.sr |= SR_C | (setx ? SR_X : 0);
    if (over) c.sr |= SR_V;
    if (z) c.sr |= SR_Z;
    if (r & szmsb(size)) c.sr |= SR_N;
    return r;
}

inline bool cond(const M68K& c, int cc) {
    const bool C = c.sr & SR_C, V = c.sr & SR_V, Z = c.sr & SR_Z, N = c.sr & SR_N;
    switch (cc) {
    case 0: return true;   case 1: return false;
    case 2: return !C && !Z; case 3: return C || Z;
    case 4: return !C;     case 5: return C;
    case 6: return !Z;     case 7: return Z;
    case 8: return !V;     case 9: return V;
    case 10: return !N;    case 11: return N;
    case 12: return N == V; case 13: return N != V;
    case 14: return !Z && N == V;
    default: return Z || N != V;
    }
}

void m68k_set_sr(M68K& c, uint16_t sr);
uint32_t m68k_get_usp(const M68K& c);
uint32_t m68k_get_ssp(const M68K& c);
void m68k_set_sps(M68K& c, uint32_t usp, uint32_t ssp);

// the reset exception: supervisor, level 7, SSP and PC from vectors 0 and 1
void m68k_reset(M68K& c);
// ask for interrupt level 0-7 (level 7 is taken on its rising edge)
void m68k_set_irq(M68K& c, int level);
// run one instruction, or take a pending interrupt or trace; its cycles
int m68k_step(M68K& c);
// take an exception from inside an instruction (TRAP, CHK, a zero divide...): the frame and the vector
void m68k_exception(M68K& c, int vector, uint32_t pc);

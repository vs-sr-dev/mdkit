// From Pocketshift (ggref/z80.*, MIT, the same author), unchanged but for this line.
// The Z80 as the Game Gear has it (an NMOS part): every documented and
// undocumented instruction, the X/Y flags and MEMPTR (WZ) as the hardware
// leaves them, Q for SCF/CCF, IM 0/1/2, NMI, HALT, EI's one-instruction
// delay, and the NMOS quirk of LD A,I / LD A,R (an interrupt accepted right
// after one clears P/V).  Timing is per instruction, in T-states.
// MAME's z80 (src/devices/cpu/z80/, BSD-3-Clause) was read as documentation.
#pragma once
#include <cstdint>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;

struct Z80Bus {
    virtual u8 rd(u16 a) = 0;
    virtual void wr(u16 a, u8 v) = 0;
    virtual u8 in(u16 port) = 0;
    virtual void out(u16 port, u8 v) = 0;
    virtual u8 irq_vector() { return 0xFF; }   // the byte on the bus at an IM 0/2 acknowledge
    virtual ~Z80Bus() {}
};

enum { FC = 0x01, FN = 0x02, FP = 0x04, FX = 0x08, FH = 0x10, FY = 0x20, FZ = 0x40, FS = 0x80 };

struct Z80 {
    u8 A = 0xFF, F = 0xFF, B = 0, C = 0, D = 0, E = 0, H = 0, L = 0;
    u16 AF_ = 0xFFFF, BC_ = 0, DE_ = 0, HL_ = 0;
    u16 IX = 0xFFFF, IY = 0xFFFF, SP = 0xFFFF, PC = 0, WZ = 0;
    u8 I = 0, R = 0, R7 = 0;            // R7: bit 7 of R, kept apart (only LD R,A sets it)
    bool iff1 = false, iff2 = false;
    int im = 0;
    bool halted = false;
    bool after_ei = false;              // interrupts are not taken right after EI
    bool after_ldair = false;           // ... and P/V is cleared if one is taken right after LD A,I/R
    u8 Q = 0;                           // the flags the last instruction wrote (0 if it wrote none)
    bool qw = false;
    bool irq = false;                   // the /INT line (level)
    bool nmi = false;                   // an NMI edge waiting
    u64 cycles = 0;
    Z80Bus *bus = nullptr;

    void reset();
    int step();                         // one instruction or one interrupt acceptance: its T-states

    u16 bc() const { return (u16)(B << 8 | C); }
    u16 de() const { return (u16)(D << 8 | E); }
    u16 hl() const { return (u16)(H << 8 | L); }
    u16 af() const { return (u16)(A << 8 | F); }
    void set_bc(u16 v) { B = v >> 8; C = (u8)v; }
    void set_de(u16 v) { D = v >> 8; E = (u8)v; }
    void set_hl(u16 v) { H = v >> 8; L = (u8)v; }
    void set_af(u16 v) { A = v >> 8; F = (u8)v; }
    u8 r_reg() const { return (u8)((R & 0x7F) | R7); }

private:
    int prefix = 0;                     // 0, 0xDD or 0xFD for the instruction running
    u8 rd(u16 a) { return bus->rd(a); }
    void wr(u16 a, u8 v) { bus->wr(a, v); }
    u8 fetch() { return bus->rd(PC++); }
    u16 fetch16() { u8 lo = fetch(); return (u16)(lo | fetch() << 8); }
    u16 rd16(u16 a) { u8 lo = rd(a); return (u16)(lo | rd((u16)(a + 1)) << 8); }
    void wr16(u16 a, u16 v) { wr(a, (u8)v); wr((u16)(a + 1), (u8)(v >> 8)); }
    void push(u16 v) { SP -= 2; wr16(SP, v); }
    u16 pop() { u16 v = rd16(SP); SP += 2; return v; }
    void setf(u8 f) { F = f; qw = true; }
    void inc_r() { R = (u8)((R + 1) & 0x7F); }

    // the registers an instruction names, under its prefix
    u16 xy() const { return prefix == 0xDD ? IX : prefix == 0xFD ? IY : hl(); }
    void set_xy(u16 v) { if (prefix == 0xDD) IX = v; else if (prefix == 0xFD) IY = v; else set_hl(v); }
    u8 get_r(int r);                    // r in 0..7 except 6; H/L are IXH/IXL under a prefix
    void set_r(int r, u8 v);
    u8 get_r_plain(int r);              // H/L always H/L (next to an (ix+d) operand)
    void set_r_plain(int r, u8 v);
    u16 mem_addr();                     // (hl), or (ix+d) with d fetched

    int exec_main(u8 op);
    int exec_cb();
    int exec_xycb();
    int exec_ed();
    int interrupt();

    // ALU
    void add8(u8 v, int carry);
    void sub8(u8 v, int carry);
    void cp8(u8 v);
    void and8(u8 v);
    void xor8(u8 v);
    void or8(u8 v);
    void alu(int op, u8 v);
    u8 inc8(u8 v);
    u8 dec8(u8 v);
    u16 add16(u16 a, u16 b);
    void adc16(u16 v);
    void sbc16(u16 v);
    u8 rot(int op, u8 v);
    void bit(int n, u8 v, u8 xyfrom);
    void daa();
};

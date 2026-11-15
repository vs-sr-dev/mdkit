// From Pocketshift (ggref/z80.*, MIT, the same author), unchanged but for this line.
// The Z80 core (see z80.h).  Opcodes decoded by their fields:
// x = op >> 6, y = op >> 3 & 7, z = op & 7, p = y >> 1, q = y & 1.
#include "z80.h"

static u8 SZXY[256], SZXYP[256];

static struct Tables {
    Tables() {
        for (int i = 0; i < 256; i++) {
            u8 f = (u8)(i & (FS | FY | FX));
            if (i == 0) f |= FZ;
            SZXY[i] = f;
            int p = 0;
            for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
            SZXYP[i] = (u8)(f | (p ? 0 : FP));
        }
    }
} tables_init;

static inline u8 parity(u8 v) { return SZXYP[v] & FP; }

void Z80::reset() {
    PC = 0;
    I = 0;
    R = 0;
    R7 = 0;
    iff1 = iff2 = false;
    im = 0;
    halted = false;
    after_ei = after_ldair = false;
    SP = 0xFFFF;
    A = F = 0xFF;
    prefix = 0;
}

// ---- ALU ---------------------------------------------------------------------------------

void Z80::add8(u8 v, int carry) {
    int r = A + v + carry;
    u8 f = SZXY[r & 0xFF] | ((A ^ v ^ r) & FH);
    if ((A ^ ~v) & (A ^ r) & 0x80) f |= FP;
    if (r & 0x100) f |= FC;
    A = (u8)r;
    setf(f);
}

void Z80::sub8(u8 v, int carry) {
    int r = A - v - carry;
    u8 f = SZXY[r & 0xFF] | FN | ((A ^ v ^ r) & FH);
    if ((A ^ v) & (A ^ r) & 0x80) f |= FP;
    if (r & 0x100) f |= FC;
    A = (u8)r;
    setf(f);
}

void Z80::cp8(u8 v) {
    int r = A - v;
    u8 f = (SZXY[r & 0xFF] & (FS | FZ)) | (v & (FX | FY)) | FN | ((A ^ v ^ r) & FH);
    if ((A ^ v) & (A ^ r) & 0x80) f |= FP;
    if (r & 0x100) f |= FC;
    setf(f);
}

void Z80::and8(u8 v) { A &= v; setf(SZXYP[A] | FH); }
void Z80::xor8(u8 v) { A ^= v; setf(SZXYP[A]); }
void Z80::or8(u8 v) { A |= v; setf(SZXYP[A]); }

void Z80::alu(int op, u8 v) {
    switch (op) {
    case 0: add8(v, 0); break;
    case 1: add8(v, F & FC); break;
    case 2: sub8(v, 0); break;
    case 3: sub8(v, F & FC); break;
    case 4: and8(v); break;
    case 5: xor8(v); break;
    case 6: or8(v); break;
    default: cp8(v); break;
    }
}

u8 Z80::inc8(u8 v) {
    u8 r = (u8)(v + 1);
    u8 f = (F & FC) | SZXY[r];
    if ((v & 0x0F) == 0x0F) f |= FH;
    if (v == 0x7F) f |= FP;
    setf(f);
    return r;
}

u8 Z80::dec8(u8 v) {
    u8 r = (u8)(v - 1);
    u8 f = (F & FC) | SZXY[r] | FN;
    if ((v & 0x0F) == 0) f |= FH;
    if (v == 0x80) f |= FP;
    setf(f);
    return r;
}

u16 Z80::add16(u16 a, u16 b) {
    u32 r = (u32)a + b;
    WZ = (u16)(a + 1);
    setf((u8)((F & (FS | FZ | FP)) | ((r >> 8) & (FY | FX)) | (((a ^ b ^ r) >> 8) & FH) | ((r >> 16) ? FC : 0)));
    return (u16)r;
}

void Z80::adc16(u16 v) {
    u16 h = hl();
    u32 r = (u32)h + v + (F & FC);
    WZ = (u16)(h + 1);
    u8 f = (u8)(((r >> 8) & (FS | FY | FX)) | (((h ^ v ^ r) >> 8) & FH));
    if (!(r & 0xFFFF)) f |= FZ;
    if ((h ^ ~v) & (h ^ r) & 0x8000) f |= FP;
    if (r & 0x10000) f |= FC;
    set_hl((u16)r);
    setf(f);
}

void Z80::sbc16(u16 v) {
    u16 h = hl();
    u32 r = (u32)h - v - (F & FC);
    WZ = (u16)(h + 1);
    u8 f = (u8)(((r >> 8) & (FS | FY | FX)) | (((h ^ v ^ r) >> 8) & FH) | FN);
    if (!(r & 0xFFFF)) f |= FZ;
    if ((h ^ v) & (h ^ r) & 0x8000) f |= FP;
    if (r & 0x10000) f |= FC;
    set_hl((u16)r);
    setf(f);
}

u8 Z80::rot(int op, u8 v) {
    u8 r, c;
    switch (op) {
    case 0: c = v >> 7; r = (u8)(v << 1 | c); break;                 // rlc
    case 1: c = v & 1; r = (u8)(v >> 1 | c << 7); break;             // rrc
    case 2: c = v >> 7; r = (u8)(v << 1 | (F & FC)); break;          // rl
    case 3: c = v & 1; r = (u8)(v >> 1 | (F & FC) << 7); break;      // rr
    case 4: c = v >> 7; r = (u8)(v << 1); break;                     // sla
    case 5: c = v & 1; r = (u8)(v >> 1 | (v & 0x80)); break;         // sra
    case 6: c = v >> 7; r = (u8)(v << 1 | 1); break;                 // sll
    default: c = v & 1; r = (u8)(v >> 1); break;                     // srl
    }
    setf(SZXYP[r] | c);
    return r;
}

void Z80::bit(int n, u8 v, u8 xyfrom) {
    u8 f = (F & FC) | FH | (xyfrom & (FX | FY));
    if (!(v & (1 << n))) f |= FZ | FP;
    if (n == 7 && (v & 0x80)) f |= FS;
    setf(f);
}

void Z80::daa() {
    u8 cor = 0;
    int c = F & FC;
    int h;
    if ((F & FH) || (A & 0x0F) > 9) cor |= 0x06;
    if (c || A > 0x99) { cor |= 0x60; c = 1; }
    u8 r;
    if (F & FN) {
        h = (F & FH) && (A & 0x0F) < 6;
        r = (u8)(A - cor);
    } else {
        h = (A & 0x0F) > 9;
        r = (u8)(A + cor);
    }
    A = r;
    setf((u8)(SZXYP[r] | (F & FN) | (h ? FH : 0) | (c ? FC : 0)));
}

// ---- operands -----------------------------------------------------------------------------

u8 Z80::get_r_plain(int r) {
    switch (r) {
    case 0: return B; case 1: return C; case 2: return D; case 3: return E;
    case 4: return H; case 5: return L; default: return A;
    }
}

void Z80::set_r_plain(int r, u8 v) {
    switch (r) {
    case 0: B = v; break; case 1: C = v; break; case 2: D = v; break; case 3: E = v; break;
    case 4: H = v; break; case 5: L = v; break; default: A = v; break;
    }
}

u8 Z80::get_r(int r) {
    if (prefix && (r == 4 || r == 5)) {
        u16 x = prefix == 0xDD ? IX : IY;
        return r == 4 ? (u8)(x >> 8) : (u8)x;
    }
    return get_r_plain(r);
}

void Z80::set_r(int r, u8 v) {
    if (prefix && (r == 4 || r == 5)) {
        u16 &x = prefix == 0xDD ? IX : IY;
        x = r == 4 ? (u16)((x & 0xFF) | v << 8) : (u16)((x & 0xFF00) | v);
        return;
    }
    set_r_plain(r, v);
}

u16 Z80::mem_addr() {
    if (!prefix) return hl();
    s8 d = (s8)fetch();
    u16 a = (u16)(xy() + d);
    WZ = a;
    return a;
}

static inline bool cond(u8 f, int cc) {
    switch (cc) {
    case 0: return !(f & FZ); case 1: return f & FZ;
    case 2: return !(f & FC); case 3: return f & FC;
    case 4: return !(f & FP); case 5: return f & FP;
    case 6: return !(f & FS); default: return f & FS;
    }
}

// ---- stepping ------------------------------------------------------------------------------

int Z80::interrupt() {
    halted = false;
    iff1 = iff2 = false;
    inc_r();
    int t;
    if (im == 2) {
        push(PC);
        u16 v = (u16)(I << 8 | bus->irq_vector());
        PC = rd16(v);
        t = 19;
    } else if (im == 1) {
        push(PC);
        PC = 0x0038;
        t = 13;
    } else {
        u8 v = bus->irq_vector();                 // only RST is supported in IM 0
        push(PC);
        PC = v & 0x38;
        t = 13;
    }
    WZ = PC;
    return t;
}

int Z80::step() {
    bool ldair = after_ldair, aei = after_ei;
    after_ldair = after_ei = false;
    int t;
    if (nmi) {
        nmi = false;
        halted = false;
        iff1 = false;
        inc_r();
        push(PC);
        PC = 0x0066;
        WZ = PC;
        t = 11;
        cycles += t;
        return t;
    }
    if (irq && iff1 && !aei) {
        t = interrupt();
        if (ldair) F &= ~FP;                      // the NMOS quirk
        Q = 0;
        cycles += t;
        return t;
    }
    if (halted) {
        inc_r();
        cycles += 4;
        return 4;
    }
    prefix = 0;
    t = 0;
    u8 op;
    for (;;) {
        op = fetch();
        inc_r();
        if (op == 0xDD || op == 0xFD) {
            prefix = op;
            t += 4;
            continue;
        }
        break;
    }
    qw = false;
    if (op == 0xCB) {
        t += prefix ? exec_xycb() : exec_cb();
    } else if (op == 0xED) {
        prefix = 0;
        t += exec_ed();
    } else {
        t += exec_main(op);
    }
    Q = qw ? F : 0;
    cycles += t;
    return t;
}

int Z80::exec_main(u8 op) {
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return 4;
            case 1: { u16 t = af(); set_af(AF_); AF_ = t; return 4; }
            case 2: {
                s8 d = (s8)fetch();
                if (--B) { PC = (u16)(PC + d); WZ = PC; return 13; }
                return 8;
            }
            case 3: { s8 d = (s8)fetch(); PC = (u16)(PC + d); WZ = PC; return 12; }
            default: {
                s8 d = (s8)fetch();
                if (cond(F, y - 4)) { PC = (u16)(PC + d); WZ = PC; return 12; }
                return 7;
            }
            }
        case 1:
            if (!q) {
                u16 v = fetch16();
                switch (p) { case 0: set_bc(v); break; case 1: set_de(v); break; case 2: set_xy(v); break; default: SP = v; }
                return 10;
            } else {
                u16 v = p == 0 ? bc() : p == 1 ? de() : p == 2 ? xy() : SP;
                set_xy(add16(xy(), v));
                return 11;
            }
        case 2:
            switch (y) {
            case 0: wr(bc(), A); WZ = (u16)(A << 8 | ((C + 1) & 0xFF)); return 7;
            case 2: wr(de(), A); WZ = (u16)(A << 8 | ((E + 1) & 0xFF)); return 7;
            case 4: { u16 a = fetch16(); wr16(a, xy()); WZ = (u16)(a + 1); return 16; }
            case 6: { u16 a = fetch16(); wr(a, A); WZ = (u16)(A << 8 | ((a + 1) & 0xFF)); return 13; }
            case 1: A = rd(bc()); WZ = (u16)(bc() + 1); return 7;
            case 3: A = rd(de()); WZ = (u16)(de() + 1); return 7;
            case 5: { u16 a = fetch16(); set_xy(rd16(a)); WZ = (u16)(a + 1); return 16; }
            default: { u16 a = fetch16(); A = rd(a); WZ = (u16)(a + 1); return 13; }
            }
        case 3: {
            int dlt = q ? -1 : 1;
            switch (p) {
            case 0: set_bc((u16)(bc() + dlt)); break;
            case 1: set_de((u16)(de() + dlt)); break;
            case 2: set_xy((u16)(xy() + dlt)); break;
            default: SP = (u16)(SP + dlt);
            }
            return 6;
        }
        case 4:
        case 5:
            if (y == 6) {
                u16 a = mem_addr();
                u8 v = rd(a);
                wr(a, z == 4 ? inc8(v) : dec8(v));
                return prefix ? 19 : 11;
            }
            set_r(y, z == 4 ? inc8(get_r(y)) : dec8(get_r(y)));
            return 4;
        case 6:
            if (y == 6) {
                u16 a = mem_addr();
                wr(a, fetch());
                return prefix ? 15 : 10;
            }
            set_r(y, fetch());
            return 7;
        default:
            switch (y) {
            case 0: A = (u8)(A << 1 | A >> 7); setf((F & (FS | FZ | FP)) | (A & (FX | FY | FC))); break;
            case 1: { u8 c = A & 1; A = (u8)(A >> 1 | c << 7); setf((F & (FS | FZ | FP)) | (A & (FX | FY)) | c); break; }
            case 2: { u8 c = A >> 7; A = (u8)(A << 1 | (F & FC)); setf((F & (FS | FZ | FP)) | (A & (FX | FY)) | c); break; }
            case 3: { u8 c = A & 1; A = (u8)(A >> 1 | (F & FC) << 7); setf((F & (FS | FZ | FP)) | (A & (FX | FY)) | c); break; }
            case 4: daa(); break;
            case 5: A = (u8)~A; setf((F & (FS | FZ | FP | FC)) | FH | FN | (A & (FX | FY))); break;
            case 6: setf((u8)((F & (FS | FZ | FP)) | FC | (((Q ^ F) | A) & (FX | FY)))); break;
            default: setf((u8)((F & (FS | FZ | FP)) | ((F & FC) ? FH : 0) | ((F & FC) ^ FC) | (((Q ^ F) | A) & (FX | FY)))); break;
            }
            return 4;
        }
    case 1:
        if (op == 0x76) { halted = true; return 4; }
        if (y == 6) {
            u16 a = mem_addr();
            wr(a, get_r_plain(z));
            return prefix ? 15 : 7;
        }
        if (z == 6) {
            u16 a = mem_addr();
            set_r_plain(y, rd(a));
            return prefix ? 15 : 7;
        }
        set_r(y, get_r(z));
        return 4;
    case 2:
        if (z == 6) {
            u16 a = mem_addr();
            alu(y, rd(a));
            return prefix ? 15 : 7;
        }
        alu(y, get_r(z));
        return 4;
    default:
        switch (z) {
        case 0:
            if (cond(F, y)) { PC = pop(); WZ = PC; return 11; }
            return 5;
        case 1:
            if (!q) {
                u16 v = pop();
                switch (p) { case 0: set_bc(v); break; case 1: set_de(v); break; case 2: set_xy(v); break; default: set_af(v); }
                return 10;
            }
            switch (p) {
            case 0: PC = pop(); WZ = PC; return 10;
            case 1: { u16 t = bc(); set_bc(BC_); BC_ = t; t = de(); set_de(DE_); DE_ = t; t = hl(); set_hl(HL_); HL_ = t; return 4; }
            case 2: PC = xy(); return 4;
            default: SP = xy(); return 6;
            }
        case 2: {
            u16 a = fetch16();
            WZ = a;
            if (cond(F, y)) PC = a;
            return 10;
        }
        case 3:
            switch (y) {
            case 0: PC = fetch16(); WZ = PC; return 10;
            case 2: { u8 n = fetch(); bus->out((u16)(A << 8 | n), A); WZ = (u16)(A << 8 | ((n + 1) & 0xFF)); return 11; }
            case 3: { u8 n = fetch(); u16 port = (u16)(A << 8 | n); A = bus->in(port); WZ = (u16)(port + 1); return 11; }
            case 4: {
                u16 v = rd16(SP);
                wr16(SP, xy());
                set_xy(v);
                WZ = v;
                return 19;
            }
            case 5: { u16 t = de(); set_de(hl()); set_hl(t); return 4; }
            case 6: iff1 = iff2 = false; return 4;
            case 7: iff1 = iff2 = true; after_ei = true; return 4;
            default: return 4;    // CB: not reached
            }
        case 4: {
            u16 a = fetch16();
            WZ = a;
            if (cond(F, y)) { push(PC); PC = a; return 17; }
            return 10;
        }
        case 5:
            if (!q) {
                u16 v = p == 0 ? bc() : p == 1 ? de() : p == 2 ? xy() : af();
                push(v);
                return 11;
            }
            {   // p == 0 (CD); DD, ED, FD do not reach here
                u16 a = fetch16();
                WZ = a;
                push(PC);
                PC = a;
                return 17;
            }
        case 6:
            alu(y, fetch());
            return 7;
        default:
            push(PC);
            PC = (u16)(y * 8);
            WZ = PC;
            return 11;
        }
    }
}

int Z80::exec_cb() {
    u8 op = fetch();
    inc_r();
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (z == 6) {
        u16 a = hl();
        u8 v = rd(a);
        if (x == 1) { bit(y, v, (u8)(WZ >> 8)); return 12; }
        u8 r = x == 0 ? rot(y, v) : x == 2 ? (u8)(v & ~(1 << y)) : (u8)(v | (1 << y));
        wr(a, r);
        return 15;
    }
    u8 v = get_r_plain(z);
    if (x == 1) { bit(y, v, v); return 8; }
    u8 r = x == 0 ? rot(y, v) : x == 2 ? (u8)(v & ~(1 << y)) : (u8)(v | (1 << y));
    set_r_plain(z, r);
    return 8;
}

int Z80::exec_xycb() {
    s8 d = (s8)fetch();
    u8 op = fetch();
    u16 a = (u16)(xy() + d);
    WZ = a;
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    u8 v = rd(a);
    if (x == 1) { bit(y, v, (u8)(a >> 8)); return 16; }
    u8 r = x == 0 ? rot(y, v) : x == 2 ? (u8)(v & ~(1 << y)) : (u8)(v | (1 << y));
    wr(a, r);
    if (z != 6) set_r_plain(z, r);                // undocumented: the result copied to a register
    return 19;
}

int Z80::exec_ed() {
    u8 op = fetch();
    inc_r();
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 1) {
        switch (z) {
        case 0: {
            u8 v = bus->in(bc());
            WZ = (u16)(bc() + 1);
            if (y != 6) set_r_plain(y, v);
            setf((F & FC) | SZXYP[v]);
            return 12;
        }
        case 1:
            bus->out(bc(), y == 6 ? 0 : get_r_plain(y));
            WZ = (u16)(bc() + 1);
            return 12;
        case 2: {
            u16 v = p == 0 ? bc() : p == 1 ? de() : p == 2 ? hl() : SP;
            if (q) adc16(v); else sbc16(v);
            return 15;
        }
        case 3: {
            u16 a = fetch16();
            WZ = (u16)(a + 1);
            if (!q) {
                u16 v = p == 0 ? bc() : p == 1 ? de() : p == 2 ? hl() : SP;
                wr16(a, v);
            } else {
                u16 v = rd16(a);
                switch (p) { case 0: set_bc(v); break; case 1: set_de(v); break; case 2: set_hl(v); break; default: SP = v; }
            }
            return 20;
        }
        case 4: { u8 v = A; A = 0; sub8(v, 0); return 8; }
        case 5: iff1 = iff2; PC = pop(); WZ = PC; return 14;
        case 6: { static const int IM[8] = { 0, 0, 1, 2, 0, 0, 1, 2 }; im = IM[y]; return 8; }
        default:
            switch (y) {
            case 0: I = A; return 9;
            case 1: R = A & 0x7F; R7 = A & 0x80; return 9;
            case 2: A = I; setf((F & FC) | SZXY[A] | (iff2 ? FP : 0)); after_ldair = true; return 9;
            case 3: A = r_reg(); setf((F & FC) | SZXY[A] | (iff2 ? FP : 0)); after_ldair = true; return 9;
            case 4: {
                u16 a = hl();
                u8 v = rd(a);
                wr(a, (u8)(A << 4 | v >> 4));
                A = (u8)((A & 0xF0) | (v & 0x0F));
                setf((F & FC) | SZXYP[A]);
                WZ = (u16)(a + 1);
                return 18;
            }
            case 5: {
                u16 a = hl();
                u8 v = rd(a);
                wr(a, (u8)(v << 4 | (A & 0x0F)));
                A = (u8)((A & 0xF0) | (v >> 4));
                setf((F & FC) | SZXYP[A]);
                WZ = (u16)(a + 1);
                return 18;
            }
            default: return 8;
            }
        }
    }
    if (x == 2 && z <= 3 && y >= 4) {
        bool dec = y & 1, rep = y >= 6;
        int dlt = dec ? -1 : 1;
        switch (z) {
        case 0: {   // ldi ldd ldir lddr
            u8 v = rd(hl());
            wr(de(), v);
            set_hl((u16)(hl() + dlt));
            set_de((u16)(de() + dlt));
            set_bc((u16)(bc() - 1));
            u8 n = (u8)(v + A);
            u8 f = (F & (FS | FZ | FC)) | (n & FX) | ((n << 4) & FY) | (bc() ? FP : 0);
            if (rep && bc()) {
                PC -= 2;
                WZ = (u16)(PC + 1);
                f = (u8)((f & ~(FX | FY)) | ((PC >> 8) & (FX | FY)));
                setf(f);
                return 21;
            }
            setf(f);
            return 16;
        }
        case 1: {   // cpi cpd cpir cpdr
            u8 v = rd(hl());
            u8 r = (u8)(A - v);
            u8 h = (A ^ v ^ r) & FH;
            u8 n = (u8)(r - (h ? 1 : 0));
            set_hl((u16)(hl() + dlt));
            set_bc((u16)(bc() - 1));
            WZ = (u16)(WZ + dlt);
            u8 f = (F & FC) | FN | (r & FS) | (r ? 0 : FZ) | h | (n & FX) | ((n << 4) & FY) | (bc() ? FP : 0);
            if (rep && bc() && r) {
                PC -= 2;
                WZ = (u16)(PC + 1);
                f = (u8)((f & ~(FX | FY)) | ((PC >> 8) & (FX | FY)));
                setf(f);
                return 21;
            }
            setf(f);
            return 16;
        }
        case 2: {   // ini ind inir indr
            u8 v = bus->in(bc());
            WZ = (u16)(bc() + dlt);
            B--;
            wr(hl(), v);
            set_hl((u16)(hl() + dlt));
            int k = v + ((C + dlt) & 0xFF);
            u8 f = SZXY[B] | ((v & 0x80) ? FN : 0) | (k > 255 ? FH | FC : 0) | parity((u8)((k & 7) ^ B));
            setf(f);
            if (rep && B) { PC -= 2; return 21; }
            return 16;
        }
        default: {  // outi outd otir otdr
            u8 v = rd(hl());
            B--;
            WZ = (u16)(bc() + dlt);
            bus->out(bc(), v);
            set_hl((u16)(hl() + dlt));
            int k = v + L;
            u8 f = SZXY[B] | ((v & 0x80) ? FN : 0) | (k > 255 ? FH | FC : 0) | parity((u8)((k & 7) ^ B));
            setf(f);
            if (rep && B) { PC -= 2; return 21; }
            return 16;
        }
        }
    }
    return 8;   // the undefined ED opcodes: an 8-cycle no-op
}

// The pointers' provenance (see md_taint.h).  One CPU is followed; the state is global.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "m68k.h"
#include "md_taint.h"

namespace {

struct Origin {
    uint32_t addr, pc, value;
    uint8_t size;
    uint32_t flags = 0, uses = 0, use_pc = 0;
};

struct Access { uint32_t addr; int size; uint32_t value; };

// an input of the instruction a written value may come from
struct Cand {
    uint32_t v;          // its value (a word zero-extended)
    int size;            // 1, 2, 4
    uint32_t tag;        // known tag, or...
    uint32_t oaddr;      // ...a ROM origin made when chosen (oaddr != ~0)
    int prio;            // ties: higher first
    int reg = -1;        // the register it is (A7 only explains A7, or a value equal to it)
};

struct State {
    M68K* cpu = nullptr;
    uint32_t rom_size = 0;
    std::vector<Origin> origins{Origin{}};      // 0: no origin
    std::unordered_map<uint64_t, uint32_t> index;
    uint32_t reg[16] = {};                       // d0-d7, a0-a7
    std::vector<uint32_t> ram = std::vector<uint32_t>(0x8000, 0);   // a tag per word
    std::map<uint32_t, std::pair<uint64_t, uint32_t>> unattributed;   // PC -> (uses, the PC that wrote the register)
    uint32_t lost[16] = {};                      // the PC that last wrote each register without an origin
    // the PC that loaded each address register (from the ROM, an immediate, a data register, the RAM;
    // a copy from another address register takes its load, an offset or an increment keeps it), and
    // each such load's accesses to the work RAM: PC -> (register, uses, first use, origin)
    uint32_t loader[16] = {};
    struct Load { int reg; uint64_t uses; uint32_t use_pc, tag; };
    std::map<uint32_t, Load> loads;
    // the step
    uint32_t pc0 = 0, before[16] = {};
    std::vector<Access> reads, writes, ext;
    uint32_t named = 0;                          // registers the opcode names (bits 0-15)
    // the probes (md_taint.h): every access of the step, bytes included
    std::vector<Access> preads, pwrites;
    std::map<std::tuple<uint32_t, int, uint32_t, int>, uint64_t> header;   // (PC, via, address, size) -> reads
    struct Sub { uint64_t count = 0; uint32_t depth = 0, shallow = ~0u, written_at = 0; };
    std::map<std::pair<uint32_t, int>, Sub> sub;  // (PC, via: 0 CPU, 1 DMA, 2 a stale local) -> reads,
                                                  // the depths under A7 (a stale local: its distance above)
    std::vector<uint8_t> stale = std::vector<uint8_t>(0x10000, 0);      // each work RAM byte: under A7 since its
    std::vector<uint32_t> swriter = std::vector<uint32_t>(0x10000, 0);   // last write, and the PC of that write
    struct StackLoad { uint64_t count = 0; uint32_t lo = ~0u, hi = 0, jump = 0; std::set<uint32_t> values; };
    std::map<uint32_t, StackLoad> stack_loads;
    // code in RAM: each work RAM byte's ROM source (+1; 0: none, written from no ROM byte) and the PC that
    // wrote it; each PC run from the work RAM: its encodings (opcode and extension words) and sources
    std::vector<uint32_t> csrc = std::vector<uint32_t>(0x10000, 0), ccopy = std::vector<uint32_t>(0x10000, 0);
    struct RamPc { uint64_t count = 0; uint32_t copy = 0; std::set<uint64_t> enc; std::set<uint32_t> srcs; };
    std::map<uint32_t, RamPc> ram_pcs;
    // an address register's work RAM address leaving the address registers: (PC, An, 'd' a data register,
    // 'm' memory, 'c' compared) -> times
    std::map<std::tuple<uint32_t, int, char>, uint64_t> exits;
};
State g;

// a work RAM address: $E00000-$FFFFFF, its top byte $00 or $FF as games write them ($07FF0000 is a number)
bool in_ram(uint32_t v) { return (v & 0xFFFFFF) >= 0xE00000 && ((v >> 24) == 0 || (v >> 24) == 0xFF); }
bool in_rom(const State& s, uint32_t a) { a &= 0xFFFFFF; return a < s.rom_size && a < 0x400000; }

// the probes: an address of the work RAM (24 bits, mirrors included) and how far under the stack pointer
bool wram(uint32_t a) { return (a & 0xFFFFFF) >= 0xE00000; }
uint32_t under(uint32_t a, uint32_t sp) {
    if (!wram(a) || !wram(sp)) return 0;
    const uint32_t d = (sp - a) & 0xFFFF;
    return d >= 1 && d <= 1024 ? d : 0;
}
void header_read(uint32_t pc, int via, uint32_t a, int size) {
    a &= 0xFFFFFF;
    if (a < 0x104 && a + uint32_t(size) > 0x100) g.header[{pc & 0xFFFFFF, via, a, size}]++;
}
void sub_read(uint32_t pc, int via, uint32_t depth, uint32_t written_at) {
    State::Sub& s = g.sub[{pc & 0xFFFFFF, via}];
    s.count++;
    if (s.count == 1 || depth > s.depth) { s.depth = depth; s.written_at = written_at; }
    s.shallow = std::min(s.shallow, depth);
}
// an instruction that loads A7 itself: MOVEA, LEA (not off A7), EXG, MOVEM (not (A7)+)
bool loads_a7(uint16_t ir) {
    if ((ir & 0xFFC0) == 0x2E40 || (ir & 0xFFC0) == 0x3E40) return true;
    if ((ir & 0xFFC0) == 0x4FC0) { const int m = (ir >> 3) & 7; return !((ir & 7) == 7 && (m == 2 || m == 5 || m == 6)); }
    if ((ir & 0xF1F8) == 0xC148) return ((ir >> 9) & 7) == 7 || (ir & 7) == 7;
    if ((ir & 0xF1F8) == 0xC188) return (ir & 7) == 7;
    if ((ir & 0xFF80) == 0x4C80)                 // its list (the first extension word) names A7
        return !g.ext.empty() && (g.ext[0].value & 0x8000) && !(((ir >> 3) & 7) == 3 && (ir & 7) == 7);
    return false;
}

uint32_t origin(uint32_t addr, int size, uint32_t pc, uint32_t value) {
    const uint64_t key = uint64_t(addr & 0xFFFFFF) | uint64_t(pc & 0xFFFFFF) << 24 | uint64_t(size) << 48;
    auto it = g.index.find(key);
    if (it != g.index.end()) return it->second;
    Origin o;
    o.addr = addr & 0xFFFFFF; o.pc = pc & 0xFFFFFF; o.value = value; o.size = uint8_t(size);
    g.origins.push_back(o);
    const uint32_t id = uint32_t(g.origins.size() - 1);
    g.index.emplace(key, id);
    return id;
}

uint32_t cand_tag(const Cand& k) {
    if (k.oaddr == ~0u) return k.tag;
    return origin(k.oaddr, k.size, g.cpu->ipc, k.v);
}

uint32_t ram_tag(uint32_t a, int size) {
    const uint32_t w = (a & 0xFFFF) >> 1;
    if (size == 2) return g.ram[w];
    const uint32_t hi = g.ram[w], lo = g.ram[(w + 1) & 0x7FFF];
    return hi ? hi : lo;
}

int32_t sext16(uint32_t v) { return int32_t(int16_t(uint16_t(v))); }

// how well a candidate explains value v written as `width` bytes (to a register when old != nullptr)
int score(const Cand& k, uint32_t v, int width, const uint32_t* old, uint32_t& dist) {
    dist = 0;
    if (width == 2) {
        if ((k.v & 0xFFFF) == (v & 0xFFFF)) return 3;
        const int32_t d = int32_t(v & 0xFFFF) - int32_t(k.v & 0xFFFF);     // no wrap: -16 is not near 16
        if (d > -0x1000 && d < 0x1000) { dist = uint32_t(d < 0 ? -d : d); return 1; }
        return 0;
    }
    if (k.v == v) return 3;
    if (k.size <= 2 && (uint32_t(sext16(k.v)) == v || (k.v & 0xFFFF) == v)) return 3;
    // a register's low word sign-extended (ext.l d0, movea.w d0,a0), when it is not a small number
    if (k.size == 4 && uint32_t(sext16(k.v)) == v && (k.v & 0xFFFF) >= 0x100) return 3;
    if (k.size == 1 && uint32_t(int32_t(int8_t(uint8_t(k.v)))) == v) return 3;
    // a word written into a register: from a read or the instruction's own word, or a register's low
    // word when that is not a small number any register may hold by chance
    if (old && (v & 0xFFFF) == (k.v & 0xFFFF) && (v >> 16) == (*old >> 16) && (k.size <= 2 || (k.v & 0xFFFF) >= 0x100))
        return 2;
    if (k.size < 4) return 0;                    // near a word: a number, more often than not
    // a pointer plus an offset stays near in all 32 bits: $FFFFFFF0 is not near $00FFFD00
    const int32_t d = int32_t(v - k.v);
    if (d > -0x8000 && d < 0x8000) { dist = uint32_t(d < 0 ? -d : d); return 1; }
    return 0;
}

uint32_t best(const std::vector<Cand>& cands, uint32_t v, int width, const uint32_t* old, int* quality = nullptr,
              int target = -1, const Cand** winner = nullptr) {
    int bq = 0, bp = -1;
    uint32_t bd = 0;
    const Cand* b = nullptr;
    for (const Cand& k : cands) {
        uint32_t d;
        const int q = score(k, v, width, old, d);
        if (!q || (k.reg == 15 && target != 15 && q < 3)) continue;
        if (q > bq || (q == bq && (d < bd || (d == bd && k.prio > bp)))) { bq = q; bd = d; bp = k.prio; b = &k; }
    }
    if (quality) *quality = bq;
    if (winner) *winner = b;
    return b ? cand_tag(*b) : 0;
}

void mark(uint32_t tag, uint32_t flag) {
    if (!tag) return;
    Origin& o = g.origins[tag];
    if (flag == TAINT_RAM_BASE) { if (!o.uses++) o.use_pc = g.cpu->ipc & 0xFFFFFF; }
    o.flags |= flag;
}

bool is_cmp(uint16_t ir, bool& word_a) {
    word_a = false;
    if ((ir & 0xFF00) == 0x0C00) return ((ir >> 6) & 3) == 2;          // CMPI.L
    if ((ir & 0xF000) != 0xB000) return false;
    const int opm = (ir >> 6) & 7;
    if (opm == 3) { word_a = true; return true; }                       // CMPA.W
    if (opm == 7 || opm == 2) return true;                               // CMPA.L, CMP.L
    return opm == 6 && ((ir >> 3) & 7) == 1;                             // CMPM.L
}

// the step's reads against A7 before it, its writes against A7 after it; the loads of A7
void probe(M68K& c, bool ran, uint16_t ir) {
    const uint32_t pc = c.ipc & 0xFFFFFF, sp0 = g.before[15], sp1 = c.a[7];
    const bool load = ran && loads_a7(ir);
    // CLR, Scc and MOVE from SR read their operand before they write it, and drop what they read: not a read here
    const bool dummy = (ir & 0xFF00) == 0x4200 || ((ir & 0xF0C0) == 0x50C0 && ((ir >> 3) & 7) != 1) ||
                       (ir & 0xFFC0) == 0x40C0;
    if (ran && !dummy)
        for (const Access& r : g.preads) {
            header_read(pc, 0, r.addr, r.size);
            if (const uint32_t d = under(r.addr, sp0)) { sub_read(pc, 0, d, 0); continue; }
            if (!wram(r.addr) || !wram(sp0)) continue;
            // at or above A7: a byte gone under it since it was last written is a stale local
            for (int i = 0; i < r.size; i++) {
                const uint32_t b = (r.addr + i) & 0xFFFF, above = (b - sp0) & 0xFFFF;
                if (above < 0x8000 && g.stale[b]) { sub_read(pc, 2, above, g.swriter[b]); break; }
            }
        }
    for (size_t j = 0; j < g.pwrites.size(); j++) {
        const Access& w = g.pwrites[j];
        if (!wram(w.addr)) continue;
        // the ROM read it copies: the read in the same place in the step (MOVEM), or any of its size and value
        const Access* src = nullptr;
        auto copies = [&](const Access& r) { return in_rom(g, r.addr) && r.size == w.size && r.value == w.value; };
        if (j < g.preads.size() && copies(g.preads[j])) src = &g.preads[j];
        else for (const Access& r : g.preads) if (copies(r)) { src = &r; break; }
        for (int i = 0; i < w.size; i++) {
            const uint32_t b = (w.addr + i) & 0xFFFF;
            g.stale[b] = under(w.addr + i, sp1) != 0;       // written under A7 (a push is not: A7 after it)
            g.swriter[b] = ran ? pc : 0xFFFFFF;
            g.csrc[b] = src ? src->addr + i + 1 : 0;
            g.ccopy[b] = ran ? pc : 0xFFFFFF;
        }
    }
    if (ran && wram(pc)) {                          // an instruction run from the work RAM
        State::RamPc& r = g.ram_pcs[pc];
        r.count++;
        uint64_t h = ir;
        for (const Access& e : g.ext) h = h * 1000003u ^ e.value;
        if (r.enc.size() < 8) r.enc.insert(h);
        const uint32_t b0 = pc & 0xFFFF, n = 2 + 2 * uint32_t(g.ext.size());
        uint32_t from = g.csrc[b0];                 // its bytes from consecutive ROM bytes, or none
        for (uint32_t i = 1; from && i < n; i++)
            if (g.csrc[(b0 + i) & 0xFFFF] != from + i) from = 0;
        if (r.srcs.size() < 8) r.srcs.insert(from ? from - 1 : 0xFFFFFFFF);
        if (!r.copy) r.copy = g.ccopy[b0];
    }
    // A7 back up by up to 1024 bytes (RTS, UNLK, RTE, a pop, lea n(A7),A7): what it leaves under it is dead;
    // a load of A7, or a longer way up, is another stack, not a return
    const int32_t up = int32_t((sp1 - sp0) << 8) >> 8;     // in 24 bits: $01000000 and $FFFFF0BE are 3906 apart
    if (!load && up > 0 && up <= 1024 && wram(sp0) && wram(sp1))
        for (uint32_t a = sp0; a != sp1; a++) g.stale[a & 0xFFFF] = 1;
    if (load) {
        State::StackLoad& s = g.stack_loads[pc];
        s.count++;
        const uint32_t v = sp1;                     // all 32 bits: lea $1000000,A7 is the top of the RAM
        s.lo = std::min(s.lo, v); s.hi = std::max(s.hi, v);
        s.jump = std::max(s.jump, uint32_t(std::abs(up)));
        if (s.values.size() < 16) s.values.insert(v);
    }
}

}  // namespace

void taint_attach(M68K* c, uint32_t rom_size) {
    g.cpu = c; g.rom_size = rom_size;
    g.reg[15] = origin(0, 4, 0, c->a[7]);       // the reset's stack pointer, vector 0
}

void taint_read(M68K& c, uint32_t a, int size, uint32_t v) {
    if (&c != g.cpu) return;
    if (size > 1) g.reads.push_back({a & 0xFFFFFF, size, v});
    g.preads.push_back({a & 0xFFFFFF, size, v});
}
void taint_write(M68K& c, uint32_t a, int size, uint32_t v) {
    if (&c != g.cpu) return;
    if (size > 1) g.writes.push_back({a & 0xFFFFFF, size, v});
    g.pwrites.push_back({a & 0xFFFFFF, size, v});
}
void taint_dma(M68K& c, uint32_t a) {
    if (&c != g.cpu) return;
    header_read(c.ipc, 1, a, 2);
    if (const uint32_t d = under(a, c.a[7])) sub_read(c.ipc, 1, d, 0);
}
void taint_fetch(M68K& c, uint32_t a, uint16_t v) {
    if (&c == g.cpu && (a & 0xFFFFFF) != (c.ipc & 0xFFFFFF)) g.ext.push_back({a & 0xFFFFFF, 2, v});
}
void taint_base(M68K& c, int reg, uint32_t value) {
    if (&c != g.cpu) return;
    g.named |= 1u << reg;
    const uint32_t tag = g.reg[reg];
    if (in_ram(value)) {
        if (reg >= 8) {
            auto [it, fresh] = g.loads.try_emplace(g.loader[reg], State::Load{reg, 0, c.ipc & 0xFFFFFF, tag});
            it->second.uses++;
            if (!it->second.tag) it->second.tag = tag;
        }
        if (tag) mark(tag, TAINT_RAM_BASE);
        else {
            auto& u = g.unattributed[c.ipc & 0xFFFFFF];
            if (!u.first++) u.second = g.lost[reg];
        }
    } else if (reg >= 8 && tag) mark(tag, TAINT_OTHER_BASE);
}

void taint_begin(M68K& c) {
    if (&c != g.cpu) return;
    g.pc0 = c.pc;
    for (int i = 0; i < 8; i++) { g.before[i] = c.d[i]; g.before[8 + i] = c.a[i]; }
    g.reads.clear(); g.writes.clear(); g.ext.clear();
    g.preads.clear(); g.pwrites.clear();
    g.named = 0;
}

void taint_end(M68K& c) {
    if (&c != g.cpu) return;
    const bool ran = (c.ipc & 0xFFFFFF) == (g.pc0 & 0xFFFFFF);    // an instruction, not an exception's entry
    const uint16_t ir = c.ir;
    std::vector<Cand> cands;
    for (const Access& r : g.reads) {
        if (in_rom(g, r.addr)) cands.push_back({r.value, r.size, 0, r.addr, 3});
        else if (in_ram(r.addr)) cands.push_back({r.value, r.size, ram_tag(r.addr, r.size), ~0u, 3});
        else cands.push_back({r.value, r.size, 0, ~0u, 3});
    }
    if (ran) {
        const bool code_rom = in_rom(g, c.ipc);
        for (size_t i = 0; i < g.ext.size(); i++) {
            const Access& e = g.ext[i];
            cands.push_back({e.value, 2, 0, code_rom ? e.addr : ~0u, 2});
            if (i + 1 < g.ext.size() && g.ext[i + 1].addr == e.addr + 2)
                cands.push_back({e.value << 16 | g.ext[i + 1].value, 4, 0, code_rom ? e.addr : ~0u, 2});
        }
        if ((ir & 0xF100) == 0x7000) cands.push_back({ir & 0xFFu, 1, 0, code_rom ? c.ipc & 0xFFFFFF : ~0u, 2});
        // the registers the opcode names: bits 9-11 and the effective address's, any MOVEM's list
        const int r9 = (ir >> 9) & 7, mode = (ir >> 3) & 7, r0 = ir & 7;
        g.named |= 1u << r9 | 1u << (8 + r9);
        if (mode != 7) g.named |= 1u << r0 | 1u << (8 + r0);
        if ((ir & 0xFB80) == 0x4880) g.named = 0xFFFF;
    }
    g.named |= 1u << 15;                         // A7 moves without being named (JSR, RTS, PEA, exceptions)
    for (int i = 0; i < 16; i++)
        if (g.named >> i & 1) cands.push_back({g.before[i], 4, g.reg[i], ~0u, i == 15 ? 0 : 1, i});

    // compared with a work RAM address
    bool word_a;
    if (ran && is_cmp(ir, word_a)) {
        int n = 0;
        for (const Cand& k : cands) n += in_ram(word_a && k.size == 2 ? uint32_t(sext16(k.v)) : k.v);
        if (n >= 2)
            for (const Cand& k : cands)
                if (in_ram(word_a && k.size == 2 ? uint32_t(sext16(k.v)) : k.v)) mark(cand_tag(k), TAINT_RAM_CMP);
        for (const Cand& k : cands)
            if (k.reg >= 8 && k.reg < 15 && in_ram(k.v)) g.exits[{c.ipc & 0xFFFFFF, k.reg - 8, 'c'}]++;
    }
    // the registers written
    uint32_t after[16];
    for (int i = 0; i < 8; i++) { after[i] = c.d[i]; after[8 + i] = c.a[i]; }
    for (int i = 0; i < 16; i++) {
        if (after[i] == g.before[i]) continue;
        int q = 0;
        const Cand* win = nullptr;
        uint32_t t = best(cands, after[i], 4, &g.before[i], &q, i, &win);
        bool same = win && win->reg == i;           // the register made from itself: its load stands
        // a register that stays a work RAM address in the same 64 KB keeps its origin unless an input is
        // equal to it: adda.l d0,a0 ($FF0000 + $D00); moveq #-1,d3 / move.b (a4,d0.w),d3
        if (q < 3 && in_ram(after[i]) && (after[i] >> 16) == (g.before[i] >> 16) && (g.reg[i] || !q)) { t = g.reg[i]; same = true; }
        // the stack pointer pushed and popped is the same stack
        if (i == 15 && g.reg[15] && int32_t(after[i] - g.before[i]) > -0x8000 && int32_t(after[i] - g.before[i]) < 0x8000)
        { t = g.reg[15]; same = true; }
        if (!ran) same = true;
        if (i >= 8 && !same)
            g.loader[i] = win && win->reg >= 8 ? g.loader[win->reg] : c.ipc & 0xFFFFFF;
        g.reg[i] = t;
        if (!t) g.lost[i] = ran ? c.ipc & 0xFFFFFF : 0xFFFFFF;     // $FFFFFF: an exception's entry
        if (i >= 8 && t) mark(t, TAINT_TO_AN);
        if (ran && i < 8 && win && win->reg >= 8 && win->v == after[i] && in_ram(after[i]))
            g.exits[{c.ipc & 0xFFFFFF, win->reg - 8, 'd'}]++;
    }
    // the work RAM written
    for (const Access& w : g.writes) {
        if (!in_ram(w.addr)) continue;
        const Cand* win = nullptr;
        const uint32_t t = best(cands, w.value, w.size, nullptr, nullptr, -1, &win);
        if (ran && w.size == 4 && win && win->reg >= 8 && win->v == w.value && in_ram(w.value))
            g.exits[{c.ipc & 0xFFFFFF, win->reg - 8, 'm'}]++;
        const uint32_t k = (w.addr & 0xFFFF) >> 1;
        g.ram[k] = t;
        if (w.size == 4) g.ram[(k + 1) & 0x7FFF] = t;
    }
    probe(c, ran, ir);
}

bool taint_save(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "# mdkit taint 1: O ADDR SIZE PC VALUE FLAGS USES USE_PC; U PC COUNT WRITTEN_AT; A LOAD_PC REG USES USE_PC ORIGIN_ADDR ORIGIN_SIZE ORIGIN_PC; "
               "H PC VIA ADDR SIZE COUNT; S PC VIA MIN MAX COUNT WRITTEN_AT; K PC COUNT LO HI JUMP VALUES; R PC COUNT COPY_PC ENCODINGS SOURCES (-: none); X PC AN TO COUNT (to: d, m, c) (via: 0 CPU, 1 DMA, 2 a stale local; flags: 1 to An, 2 work RAM base, "
               "4 compared with a work RAM address, 8 other base)%s", "\n");
    for (size_t i = 1; i < g.origins.size(); i++) {
        const Origin& o = g.origins[i];
        if (o.flags)
            fprintf(f, "O %06X %u %06X %08X %X %u %06X%s", o.addr, o.size, o.pc, o.value, o.flags, o.uses, o.use_pc, "\n");
    }
    for (auto& [pc, u] : g.unattributed) fprintf(f, "U %06X %llu %06X%s", pc, (unsigned long long)u.first, u.second, "\n");
    for (auto& [pc, l] : g.loads) {
        const Origin& o = g.origins[l.tag];
        fprintf(f, "A %06X %d %llu %06X %06X %u %06X%s", pc, l.reg - 8, (unsigned long long)l.uses, l.use_pc,
                l.tag ? o.addr : 0, l.tag ? o.size : 0, l.tag ? o.pc : 0, "\n");
    }
    for (auto& [k, n] : g.header)
        fprintf(f, "H %06X %d %06X %d %llu%s", std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k),
                (unsigned long long)n, "\n");
    for (auto& [k, s] : g.sub)
        fprintf(f, "S %06X %d %u %u %llu %06X%s", k.first, k.second, s.shallow, s.depth, (unsigned long long)s.count,
                s.written_at, "\n");
    for (auto& [pc, r] : g.ram_pcs) {
        fprintf(f, "R %06X %llu %06X ", pc, (unsigned long long)r.count, r.copy);
        bool first = true;
        for (uint64_t h : r.enc) { fprintf(f, first ? "%016llX" : ",%016llX", (unsigned long long)h); first = false; }
        first = true;
        for (uint32_t v : r.srcs) {
            if (v == 0xFFFFFFFF) fprintf(f, first ? " -" : ",-"); else fprintf(f, first ? " %06X" : ",%06X", v);
            first = false;
        }
        fprintf(f, "%s", "\n");
    }
    for (auto& [k, n] : g.exits)
        fprintf(f, "X %06X %d %c %llu%s", std::get<0>(k), std::get<1>(k), std::get<2>(k), (unsigned long long)n, "\n");
    for (auto& [pc, s] : g.stack_loads) {
        fprintf(f, "K %06X %llu %08X %08X %X ", pc, (unsigned long long)s.count, s.lo, s.hi, s.jump);
        bool first = true;
        for (uint32_t v : s.values) { fprintf(f, first ? "%08X" : ",%08X", v); first = false; }
        fprintf(f, "%s", "\n");
    }
    fclose(f);
    return true;
}

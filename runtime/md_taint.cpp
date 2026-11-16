// The pointers' provenance (see md_taint.h).  One CPU is followed; the state is global.
#include <cstdio>
#include <cstdlib>
#include <map>
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
};
State g;

// a work RAM address: $E00000-$FFFFFF, its top byte $00 or $FF as games write them ($07FF0000 is a number)
bool in_ram(uint32_t v) { return (v & 0xFFFFFF) >= 0xE00000 && ((v >> 24) == 0 || (v >> 24) == 0xFF); }
bool in_rom(const State& s, uint32_t a) { a &= 0xFFFFFF; return a < s.rom_size && a < 0x400000; }

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

}  // namespace

void taint_attach(M68K* c, uint32_t rom_size) {
    g.cpu = c; g.rom_size = rom_size;
    g.reg[15] = origin(0, 4, 0, c->a[7]);       // the reset's stack pointer, vector 0
}

void taint_read(M68K& c, uint32_t a, int size, uint32_t v) {
    if (&c == g.cpu) g.reads.push_back({a & 0xFFFFFF, size, v});
}
void taint_write(M68K& c, uint32_t a, int size, uint32_t v) {
    if (&c == g.cpu) g.writes.push_back({a & 0xFFFFFF, size, v});
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
    }
    // the work RAM written
    for (const Access& w : g.writes) {
        if (!in_ram(w.addr)) continue;
        const uint32_t t = best(cands, w.value, w.size, nullptr);
        const uint32_t k = (w.addr & 0xFFFF) >> 1;
        g.ram[k] = t;
        if (w.size == 4) g.ram[(k + 1) & 0x7FFF] = t;
    }
}

bool taint_save(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "# mdkit taint 1: O ADDR SIZE PC VALUE FLAGS USES USE_PC; U PC COUNT WRITTEN_AT; A LOAD_PC REG USES USE_PC ORIGIN_ADDR ORIGIN_SIZE ORIGIN_PC (flags: 1 to An, 2 work RAM base, "
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
    fclose(f);
    return true;
}

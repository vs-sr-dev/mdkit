// The tapes (md_tape.h), the monkey's hand and the picture as a PPM.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "md.h"
#include "md_tape.h"

void md_write_ppm(const std::string& path, const MD& m) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", m.vdp.out_w, m.vdp.out_h);
    std::vector<uint8_t> row(size_t(m.vdp.out_w) * 3);
    for (int y = 0; y < m.vdp.out_h; y++) {
        for (int x = 0; x < m.vdp.out_w; x++) {
            const uint32_t c = m.vdp.out[y * MAX_W + x];
            row[x * 3] = uint8_t(c >> 16); row[x * 3 + 1] = uint8_t(c >> 8); row[x * 3 + 2] = uint8_t(c);
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

// the monkey: a new hand every 6 frames from a xorshift seeded by SEED
uint16_t md_monkey(uint64_t frame, uint32_t seed) {
    uint32_t x = seed * 2654435761u ^ uint32_t(frame / 6) * 40503u ^ 0x9E3779B9u;
    for (int i = 0; i < 4; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
    uint16_t b = 0;
    const uint64_t f = frame % 600;
    if ((frame > 90 && f < 4) || (frame < 400 && frame % 120 < 3)) b |= 0x80;   // Start
    if (x & 1) b |= 0x10;                       // A
    if (x & 2) b |= 0x20;                       // B
    if ((x & 12) == 12) b |= 0x40;              // C
    static const uint16_t dirs[8] = {0, 1, 2, 4, 8, 8, 5, 9};
    b |= dirs[(x >> 4) & 7];
    return b;
}

static uint16_t buttons(const std::string& v) {
    static const char* names[12] = {"up", "down", "left", "right", "a", "b", "c", "start", "x", "y", "z", "mode"};
    uint16_t b = 0;
    size_t p = 0;
    while (p <= v.size()) {
        size_t q = v.find('+', p);
        if (q == std::string::npos) q = v.size();
        const std::string n = v.substr(p, q - p);
        bool known = n == "none";
        for (int i = 0; i < 12; i++) if (n == names[i]) { b |= uint16_t(1 << i); known = true; }
        if (!known) { fprintf(stderr, "tape: unknown button %s\n", n.c_str()); exit(2); }
        p = q + 1;
    }
    return b;
}

bool Tape::load(const std::string& path) {
    Tape& t = *this;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[1024];
    int line = 0;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        std::string s(buf);
        const size_t h = s.find('#');
        if (h != std::string::npos) s.resize(h);
        TapeOp op{{}, line};
        size_t p = 0;
        while (p < s.size()) {
            while (p < s.size() && isspace(uint8_t(s[p]))) p++;
            size_t q = p;
            while (q < s.size() && !isspace(uint8_t(s[q]))) q++;
            if (q > p) op.w.push_back(s.substr(p, q - p));
            p = q;
        }
        if (!op.w.empty()) t.ops.push_back(op);
    }
    fclose(f);
    return true;
}

static int size_of(const std::string& s) { return s == "b" ? 1 : s == "w" ? 2 : 4; }
static uint32_t ram_get(const MD& m, uint32_t a, int size) {
    uint32_t v = 0;
    for (int k = 0; k < size; k++) v = v << 8 | m.ram[(a + k) & 0xFFFF];
    return v;
}
static void ram_put(MD& m, uint32_t a, int size, uint32_t v) {
    for (int k = size - 1; k >= 0; k--) { m.ram[(a + k) & 0xFFFF] = uint8_t(v); v >>= 8; }
}

bool Tape::frame(MD& m, int f, uint16_t& pad) {
    Tape& t = *this;
    // every write to m's work RAM, and to whoever listens (on_write)
    auto ram_put = [&](MD& mm, uint32_t a, int size, uint32_t v) { ::ram_put(mm, a, size, v); if (on_write) on_write(a & 0xFFFFFF, size, v); };
    while (t.i < t.ops.size()) {
        const TapeOp& op = t.ops[t.i];
        const std::vector<std::string>& w = op.w;
        auto arg = [&](size_t k) -> const std::string& {
            if (k >= w.size()) { fprintf(stderr, "tape: line %d: too few words\n", op.line); exit(2); }
            return w[k];
        };
        auto hex = [&](size_t k) { return uint32_t(std::stoul(arg(k), nullptr, 16)); };
        auto dec = [&](size_t k) { return std::stol(arg(k)); };
        const std::string& c = w[0];
        if (c == "poke") { ram_put(m, hex(1), size_of(arg(2)), hex(3)); t.i++; continue; }
        if (c == "pokeat") {
            const uint32_t base = hex(1), stride = hex(2), count = hex(3), key = hex(4), byte = hex(5), off = hex(6);
            for (uint32_t k = 0; k < count; k++)
                if (ram_get(m, base + k * stride + key, 1) == byte) ram_put(m, base + k * stride + off, size_of(arg(7)), hex(8));
            t.i++; continue;
        }
        if (c == "keep") { t.keeps.push_back({hex(1), size_of(arg(2)), hex(3)}); t.i++; continue; }
        if (c == "unkeep") {
            const uint32_t a = hex(1);
            std::erase_if(t.keeps, [&](const Tape::Keep& k) { return k.addr == a; });
            t.i++; continue;
        }
        if (c == "snap") { md_write_ppm(out + "/" + arg(1) + ".ppm", m); t.i++; continue; }
        if (c == "echo") {
            printf("tape: frame %d:", f);
            for (size_t k = 1; k < w.size(); k++) printf(" %s", w[k].c_str());
            printf("%s", "\n");
            t.i++; continue;
        }
        if (c == "until") {
            if (t.left < 0) t.left = dec(5);
            const uint32_t v = ram_get(m, hex(1), size_of(arg(2))), want = hex(4);
            const std::string& o = arg(3);
            const bool met = o == "==" ? v == want : o == "!=" ? v != want : o == "<" ? v < want :
                             o == ">=" ? v >= want : (v & want) != 0;
            if (met || t.left == 0) {
                if (!met) { printf("tape: line %d: until timed out at frame %d%s", op.line, f, "\n"); t.timeouts++; }
                t.i++; t.left = -1; continue;
            }
            pad = w.size() > 6 ? buttons(w[6]) : 0;
        } else if (c == "hold" || c == "wait" || c == "tap" || c == "monkey") {
            const long n = c == "wait" ? dec(1) : c == "tap" ? (w.size() > 2 ? dec(2) : 3) : dec(2);
            if (t.left < 0) t.left = c == "tap" ? 2 * n : n;
            if (t.left == 0) { t.i++; t.left = -1; continue; }
            pad = c == "hold" ? buttons(arg(1)) : c == "tap" ? (t.left > n ? buttons(arg(1)) : 0) :
                  c == "monkey" ? md_monkey(uint64_t(f), uint32_t(dec(1))) : 0;
        } else { fprintf(stderr, "tape: line %d: unknown command %s\n", op.line, c.c_str()); exit(2); }
        t.left--;
        for (const Tape::Keep& k : t.keeps) ram_put(m, k.addr, k.size, k.value);
        return true;
    }
    return false;
}

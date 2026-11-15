// md: the Mega Drive runtime as an executable (headless for now).
//
//   md ROM.bin [--machine NTSC-U|NTSC-J|PAL] [--sram BYTES] [--pad6] [--frames N]
//              [--snap F1,F2,...] [--out DIR] [--monkey SEED] [--state]
//
// The ROM is the list's image (cart.py --collect writes it so).  --monkey drives pad 1
// with a scripted, seeded hand: Start now and then, the buttons and the directions at random.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <set>
#include <vector>
#include <chrono>
#include "md.h"

static std::vector<uint8_t> load(const char* path) {
    std::vector<uint8_t> d;
    FILE* f = fopen(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

static void write_ppm(const std::string& path, const MD& m) {
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
static uint16_t monkey(uint64_t frame, uint32_t seed) {
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

static uint64_t fnv(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t md_hash(const MD& m) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(m.cpu.d, sizeof m.cpu.d, h); h = fnv(m.cpu.a, sizeof m.cpu.a, h);
    h = fnv(&m.cpu.pc, 4, h); h = fnv(&m.cpu.sr, 2, h);
    h = fnv(m.ram, sizeof m.ram, h); h = fnv(m.zram, sizeof m.zram, h);
    h = fnv(m.vdp.vram, sizeof m.vdp.vram, h); h = fnv(m.vdp.cram, sizeof m.vdp.cram, h);
    h = fnv(m.vdp.vsram, sizeof m.vdp.vsram, h); h = fnv(m.vdp.reg, sizeof m.vdp.reg, h);
    h = fnv(m.vdp.out, sizeof m.vdp.out, h);
    h = fnv(&m.t, sizeof m.t, h);
    return h;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "md ROM [--machine NTSC-U|NTSC-J|PAL] [--sram N] [--pad6] [--frames N] [--snap F,...] "
                        "[--out DIR] [--monkey SEED] [--state]\n");
        return 2;
    }
    const char* rom = argv[1];
    Machine mach = M_NTSC_U;
    uint32_t sram = 0, seed = 0;
    int frames = 600;
    bool pad6 = false, state = false, use_monkey = false;
    std::set<int> snaps;
    std::string out = ".";
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--machine") { const std::string v = val(); mach = v == "PAL" ? M_PAL : v == "NTSC-J" ? M_NTSC_J : M_NTSC_U; }
        else if (a == "--sram") sram = uint32_t(std::stoul(val()));
        else if (a == "--pad6") pad6 = true;
        else if (a == "--frames") frames = std::stoi(val());
        else if (a == "--snap") { std::string v = val(); size_t p = 0; while (p < v.size()) { size_t q = v.find(',', p); if (q == std::string::npos) q = v.size(); snaps.insert(std::stoi(v.substr(p, q - p))); p = q + 1; } }
        else if (a == "--out") out = val();
        else if (a == "--monkey") { use_monkey = true; seed = uint32_t(std::stoul(val())); }
        else if (a == "--state") state = true;
        else if (a == "--slot") val();
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    const std::vector<uint8_t> image = load(rom);
    if (image.size() < 0x200) { fprintf(stderr, "cannot read %s\n", rom); return 1; }
    static MD m;
    m.init(image, mach, sram);
    m.pad[0].six = pad6;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; f++) {
        if (use_monkey) m.pad[0].buttons = monkey(uint64_t(f), seed);
        m.run_frame();
        if (snaps.count(f)) {
            char name[64];
            snprintf(name, sizeof name, "/f%05d.ppm", f);
            write_ppm(out + name, m);
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%d frames in %.2f s (%.0f fps)  hash %016llx\n", frames, secs, frames / secs, (unsigned long long)md_hash(m));
    if (state) {
        printf("PC %06X SR %04X  D", m.cpu.pc, m.cpu.sr);
        for (int i = 0; i < 8; i++) printf(" %08X", m.cpu.d[i]);
        printf("\n  A");
        for (int i = 0; i < 8; i++) printf(" %08X", m.cpu.a[i]);
        printf("\n  VDP regs");
        for (int i = 0; i < 24; i++) printf(" %02X", m.vdp.reg[i]);
        printf("\n  ints: VINT %u HINT %u  Z80 PC %04X %s%s  YM writes %u\n", m.vints, m.hints, m.z80.PC,
               m.z80_reset ? "reset " : "", m.z80_busreq ? "busreq" : "", m.ym.writes);
    }
    return 0;
}

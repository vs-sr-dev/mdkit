// mdkit's 68000 against SingleStepTests/m68000 (v1, files .json.bin; generated with MAME's
// microcoded 68000): one instruction per test from a given state, then the registers, SR,
// PC, RAM and the instruction's cycles compared.  Tests whose reference raised an
// address error are counted apart (the frame's details matter to no Mega Drive game).
//
//   m68ktest DIR [NAME...] [-v N]   (NAME: a file stem such as ADD.b; -v: show N failures)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include "../runtime/m68k.h"

static std::vector<uint8_t> mem(1 << 24);
static std::vector<uint32_t> touched;   // every byte written, cleared after the test

// the memory through the I/O handlers, so that every write is seen
static uint8_t r8(void*, uint32_t a) { return mem[a & 0xFFFFFF]; }
static uint16_t r16(void*, uint32_t a) { a &= 0xFFFFFF; return uint16_t(mem[a] << 8 | mem[(a + 1) & 0xFFFFFF]); }
static void w8(void*, uint32_t a, uint8_t v) { a &= 0xFFFFFF; touched.push_back(a); mem[a] = v; }
static void w16(void*, uint32_t a, uint16_t v) {
    a &= 0xFFFFFF;
    touched.push_back(a); touched.push_back((a + 1) & 0xFFFFFF);
    mem[a] = uint8_t(v >> 8); mem[(a + 1) & 0xFFFFFF] = uint8_t(v);
}

struct State {
    uint32_t r[19];             // d0-d7 a0-a6 usp ssp sr pc
    uint32_t pf[2];
    std::vector<std::pair<uint32_t, uint32_t>> ram;   // address, word
};
static const char* const names[19] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2",
                                      "a3", "a4", "a5", "a6", "usp", "ssp", "sr", "pc"};

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

static const uint8_t* read_state(const uint8_t* p, State& s) {
    p += 8;
    for (int i = 0; i < 19; i++, p += 4) s.r[i] = le32(p);
    s.pf[0] = le32(p); s.pf[1] = le32(p + 4);
    p += 8;
    const uint32_t n = le32(p);
    p += 4;
    s.ram.clear();
    for (uint32_t i = 0; i < n; i++, p += 6) s.ram.push_back({le32(p), uint32_t(p[4] | (p[5] << 8))});
    return p;
}

static long total_pass, total_fail, total_ae_pass, total_ae_fail, total_cyc_bad;

static void run_file(const std::string& path, const std::string& stem, int verbose) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(size);
    if (fread(buf.data(), 1, size, f) != size_t(size)) { fclose(f); return; }
    fclose(f);
    const uint32_t count = le32(buf.data() + 4);
    const uint8_t* p = buf.data() + 8;
    M68K cpu;
    cpu.io_r8 = r8; cpu.io_r16 = r16; cpu.io_w8 = w8; cpu.io_w16 = w16;
    long pass = 0, fail = 0, ae_pass = 0, ae_fail = 0, cyc_bad = 0;
    std::map<int, std::pair<int, std::string>> cyc_diff;   // mine - want -> count, an example
    int shown = 0;
    for (uint32_t t = 0; t < count; t++) {
        p += 16;
        const uint32_t nlen = le32(p);
        std::string name((const char*)p + 4, std::min<uint32_t>(nlen, 127));
        p += 4 + nlen;
        State in, out;
        p = read_state(p, in);
        p = read_state(p, out);
        p += 8;
        const uint32_t want_cyc = le32(p), ntr = le32(p + 4);
        p += 8;
        bool ae = false;
        for (uint32_t k = 0; k < ntr; k++) {
            const uint8_t tw = p[0];
            p += 5;
            if (tw) { if (tw == 4 || tw == 5) ae = true; p += 20; }
        }
        for (auto& [a, w] : in.ram) { const uint32_t x = a & 0xFFFFFF; mem[x] = uint8_t(w >> 8); mem[(x + 1) & 0xFFFFFF] = uint8_t(w); }
        // the prefetch queue holds the opcode and the word after it: put them where the PC was
        const uint32_t pc0 = in.r[18] - 4;
        mem[pc0 & 0xFFFFFF] = uint8_t(in.pf[0] >> 8); mem[(pc0 + 1) & 0xFFFFFF] = uint8_t(in.pf[0]);
        mem[(pc0 + 2) & 0xFFFFFF] = uint8_t(in.pf[1] >> 8); mem[(pc0 + 3) & 0xFFFFFF] = uint8_t(in.pf[1]);
        for (int i = 0; i < 8; i++) { cpu.d[i] = in.r[i]; if (i < 7) cpu.a[i] = in.r[8 + i]; }
        cpu.sr = uint16_t(in.r[17]);
        m68k_set_sps(cpu, in.r[15], in.r[16]);
        cpu.pc = pc0;
        cpu.stopped = cpu.halted = cpu.trace_pending = false;
        cpu.ipl = 0;
        const int cyc = m68k_step(cpu);

        uint32_t got[19];
        for (int i = 0; i < 8; i++) got[i] = cpu.d[i];
        for (int i = 0; i < 7; i++) got[8 + i] = cpu.a[i];
        got[15] = m68k_get_usp(cpu);
        got[16] = m68k_get_ssp(cpu);
        got[17] = cpu.sr;
        got[18] = cpu.stopped ? cpu.pc : cpu.pc + 4;    // the reference's PC is the next prefetch address
        bool ok = true;
        std::string why;
        char tmp[96];
        for (int i = 0; i < 19; i++)
            if (got[i] != out.r[i]) {
                ok = false;
                snprintf(tmp, sizeof tmp, " %s=%x(want %x)", names[i], got[i], out.r[i]);
                why += tmp;
            }
        for (auto& [a, w] : out.ram) {
            const uint32_t x = a & 0xFFFFFF;
            const uint32_t v = uint32_t(mem[x] << 8 | mem[(x + 1) & 0xFFFFFF]);
            if (v != w) {
                ok = false;
                snprintf(tmp, sizeof tmp, " [%x]=%04x(want %04x)", x, v, w);
                why += tmp;
            }
        }
        const bool cyc_ok = cyc == int(want_cyc);
        if (ok && !cyc_ok && !ae) {
            cyc_bad++;
            auto& d = cyc_diff[cyc - int(want_cyc)];
            if (getenv("M68K_CYC"))
                printf("  CYCLES %s: %d, want %u (sr %04x, Dx %08x, Dy %08x)\n", name.c_str(), cyc, want_cyc, in.r[17],
                       in.r[(in.pf[0] >> 9) & 7], in.r[in.pf[0] & 7]);
            if (!d.first++) { snprintf(tmp, sizeof tmp, "%s (%d, want %u)", name.c_str(), cyc, want_cyc); d.second = tmp; }
        }
        if (ok) (ae ? ae_pass : pass)++;
        else {
            (ae ? ae_fail : fail)++;
            if (shown < verbose && (!ae || getenv("M68K_AE"))) {
                printf("  FAIL %s (sr in %04x):%s\n", name.c_str(), in.r[17], why.c_str());
                shown++;
            }
        }
        for (auto& [a, w] : in.ram) { const uint32_t x = a & 0xFFFFFF; mem[x] = mem[(x + 1) & 0xFFFFFF] = 0; }
        for (auto& [a, w] : out.ram) { const uint32_t x = a & 0xFFFFFF; mem[x] = mem[(x + 1) & 0xFFFFFF] = 0; }
        for (int i = 0; i < 4; i++) mem[(pc0 + i) & 0xFFFFFF] = 0;
        for (uint32_t x : touched) mem[x] = 0;
        touched.clear();
    }
    printf("%-14s %5ld/%5ld%s", stem.c_str(), pass, pass + fail, fail ? "  <--" : "");
    if (ae_pass + ae_fail) printf("   address errors %ld/%ld", ae_pass, ae_pass + ae_fail);
    if (cyc_bad) {
        printf("   cycles off in %ld:", cyc_bad);
        for (auto& [d, v] : cyc_diff) printf(" %+d x%d [%s]", d, v.first, v.second.c_str());
    }
    printf("\n");
    total_pass += pass; total_fail += fail; total_ae_pass += ae_pass; total_ae_fail += ae_fail;
    total_cyc_bad += cyc_bad;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "m68ktest DIR [NAME...] [-v N]\n"); return 2; }
    const std::string dir = argv[1];
    int verbose = 0;
    std::vector<std::string> want;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v") && i + 1 < argc) verbose = atoi(argv[++i]);
        else want.push_back(argv[i]);
    }
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (!d) { fprintf(stderr, "cannot open %s\n", dir.c_str()); return 2; }
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 9 && n.compare(n.size() - 9, 9, ".json.bin") == 0) files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        const std::string stem = f.substr(0, f.size() - 9);
        if (!want.empty() && std::find(want.begin(), want.end(), stem) == want.end()) continue;
        run_file(dir + "/" + f, stem, verbose);
    }
    printf("\ntotal %ld/%ld passed (%ld with the cycles off); address-error tests %ld/%ld\n", total_pass,
           total_pass + total_fail, total_cyc_bad, total_ae_pass, total_ae_pass + total_ae_fail);
    return total_fail || total_cyc_bad ? 1 : 0;
}

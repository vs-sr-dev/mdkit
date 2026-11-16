// md: the Mega Drive runtime as an executable (headless for now).
//
//   md ROM.bin [--machine NTSC-U|NTSC-J|PAL] [--sram BYTES] [--pad6] [--frames N]
//              [--snap F1,F2,...] [--out DIR] [--monkey SEED] [--state] [--slot MAME_SLOT]
//              [--interp] [--lockstep] [--cover FILE]
//              [--wav FILE] [--vgm FILE] [--press "FRAME+LEN:button;..."] [--tape FILE] [--inputlog FILE]
//              [--pcs F] [--trace F] [--iolog F] [--ram F]     (diagnostics of frame F)
//
// The ROM is the list's image (cart.py --collect writes it so).  --monkey drives pad 1
// with a scripted, seeded hand: Start now and then, the buttons and the directions at random.
// --tape plays a scripted session (md_tape.h) and the run ends with it; --inputlog FILE writes
// the pad and the tape's RAM writes frame by frame.  Built with -DMD_TAINT
// (build/md_taint.exe), --cover FILE also writes FILE.taint (md_taint.h).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <set>
#include <vector>
#include <chrono>
#include <map>
#include <algorithm>
#include "md.h"
#include "md_native.h"
#include "md_tape.h"

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
    if (!m.sram.empty()) h = fnv(m.sram.data(), m.sram.size() * 2, h);
    h = fnv(m.vdp.vsram, sizeof m.vdp.vsram, h); h = fnv(m.vdp.reg, sizeof m.vdp.reg, h);
    h = fnv(m.vdp.out, sizeof m.vdp.out, h);
    h = fnv(&m.t, sizeof m.t, h);
    return h;
}

// --iolog: the 68000's I/O accesses of a frame, with the PC and the line
static bool g_iolog = false;
static void logio(MD& m, const char* k, uint32_t a, uint32_t v) {
    if (g_iolog) printf("  %06X L%3d %s %06X %X\n", m.cpu.ipc, m.line, k, a & 0xFFFFFF, v);
}
static uint8_t log_r8(void* p, uint32_t a) { MD& m = *static_cast<MD*>(p); uint8_t v = m.io_r8(a); logio(m, "r8 ", a, v); return v; }
static uint16_t log_r16(void* p, uint32_t a) { MD& m = *static_cast<MD*>(p); uint16_t v = m.io_r16(a); logio(m, "r16", a, v); return v; }
static void log_w8(void* p, uint32_t a, uint8_t v) { MD& m = *static_cast<MD*>(p); logio(m, "w8 ", a, v); m.io_w8(a, v); }
static void log_w16(void* p, uint32_t a, uint16_t v) { MD& m = *static_cast<MD*>(p); logio(m, "w16", a, v); m.io_w16(a, v); }

// --press "FRAME+LEN:button;...": pad 1 held so, as the MAME script takes it (frames from 0)
struct Press { int from, to; uint16_t mask; };
static std::vector<Press> parse_presses(const std::string& v) {
    static const char* names[12] = {"up", "down", "left", "right", "a", "b", "c", "start", "x", "y", "z", "mode"};
    std::vector<Press> out;
    size_t p = 0;
    while (p < v.size()) {
        size_t q = v.find(';', p);
        if (q == std::string::npos) q = v.size();
        const std::string item = v.substr(p, q - p);
        const size_t plus = item.find('+'), colon = item.find(':');
        if (plus != std::string::npos && colon != std::string::npos) {
            Press pr{std::stoi(item.substr(0, plus)), 0, 0};
            pr.to = pr.from + std::stoi(item.substr(plus + 1, colon - plus - 1));
            const std::string n = item.substr(colon + 1);
            for (int i = 0; i < 12; i++) if (n == names[i]) pr.mask = uint16_t(1 << i);
            out.push_back(pr);
        }
        p = q + 1;
    }
    return out;
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
    std::vector<Press> presses;
    bool interp = false, lockstep = false, steplog = false, survey = false;
    std::vector<std::pair<uint32_t, uint64_t>> log_m, log_ref;
    std::string cover_path, tape_path, inputlog_path;
    int pcs_frame = -1, iolog_frame = -1, pcs_n = 24, trace_frame = -1, trace_n = 100000, traced = 0, ram_frame = -1;
    std::set<int> snaps;
    std::string out = ".", slot = "rom", wav, vgm;
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
        else if (a == "--pcs") pcs_frame = std::stoi(val());
        else if (a == "--pcs-n") pcs_n = std::stoi(val());
        else if (a == "--wav") wav = val();
        else if (a == "--interp") interp = true;
        else if (a == "--lockstep") lockstep = true;
        else if (a == "--survey") survey = true;
        else if (a == "--steplog") steplog = true;
        else if (a == "--cover") cover_path = val();
        else if (a == "--vgm") vgm = val();
        else if (a == "--press") presses = parse_presses(val());
        else if (a == "--tape") tape_path = val();
        else if (a == "--inputlog") inputlog_path = val();
        else if (a == "--ram") ram_frame = std::stoi(val());
        else if (a == "--trace") trace_frame = std::stoi(val());
        else if (a == "--trace-n") trace_n = std::stoi(val());
        else if (a == "--iolog") iolog_frame = std::stoi(val());
        else if (a == "--slot") slot = val();
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    const std::vector<uint8_t> image = load(rom);
    if (image.size() < 0x200) { fprintf(stderr, "cannot read %s\n", rom); return 1; }
    static MD m, ref;           // ref: the interpreter's machine, for --lockstep
    CartHW probe;
    if (!cart_setup(probe, slot)) fprintf(stderr, "slot %s not covered: run as rom\n", slot.c_str());
    m.init(image, mach, sram, slot);
    m.pad[0].six = pad6;
    if (lockstep) { ref.init(image, mach, sram, slot); ref.pad[0].six = pad6; }
    if (lockstep && steplog) { m.steplog = &log_m; ref.steplog = &log_ref; }
    std::vector<uint8_t> cov;
    std::map<uint32_t, uint32_t> site_map;
    std::vector<uint16_t> zcov;
    if (!cover_path.empty()) { cov.assign(1 << 23, 0); m.cover = &cov; m.sites = &site_map; interp = true; zcov.assign(0x2000, 0); m.zcover = &zcov; }
    if (md_native_n == 0) interp = true;
#ifdef MD_TAINT
    if (!cover_path.empty()) taint_attach(&m.cpu, uint32_t(image.size()));
#endif
    Tape tape;
    tape.out = out;
    std::vector<std::string> written;       // this frame's tape writes, for --inputlog
    tape.on_write = [&](uint32_t a, int size, uint32_t v) {
        if (lockstep) {     // the lockstep's interpreter sees the same session
            uint32_t x = v;
            for (int k = size - 1; k >= 0; k--) { ref.ram[(a + k) & 0xFFFF] = uint8_t(x); x >>= 8; }
        }
        char buf[64];
        snprintf(buf, sizeof buf, "poke %06X %c %X", a, size == 1 ? 'b' : size == 2 ? 'w' : 'l', v);
        written.push_back(buf);
    };
    if (!tape_path.empty()) {
        if (!tape.load(tape_path)) { fprintf(stderr, "cannot read %s\n", tape_path.c_str()); return 1; }
        frames = 1 << 30;
    }
    m.sound.open(wav, vgm, m.pal ? 53203424.0 : 53693175.0);
    if (iolog_frame >= 0) {
        m.cpu.io_r8 = log_r8; m.cpu.io_r16 = log_r16; m.cpu.io_w8 = log_w8; m.cpu.io_w16 = log_w16;
    }
    // --inputlog FILE: what drove the machine, frame by frame: "FRAME PAD" (pad 1, bits 0-11
    // U D L R A B C Start X Y Z Mode), then "FRAME poke ADDR b|w|l VALUE" for each write the tape
    // made to the work RAM before that frame
    FILE* inlog = inputlog_path.empty() ? nullptr : fopen(inputlog_path.c_str(), "wb");
    if (inlog) fprintf(inlog, "# mdkit input log: FRAME PAD, FRAME poke ADDR SIZE VALUE\n");
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; f++) {
        if (!tape_path.empty()) {
            uint16_t b = 0;
            if (!tape.frame(m, f, b)) { frames = f; break; }
            m.pad[0].buttons = b;
        }
        if (use_monkey) m.pad[0].buttons = md_monkey(uint64_t(f), seed);
        if (!presses.empty()) {
            uint16_t b = 0;
            for (const Press& p : presses) if (f >= p.from && f < p.to) b |= p.mask;
            m.pad[0].buttons = b;
        }
        if (inlog) {
            fprintf(inlog, "%d %03X\n", f, m.pad[0].buttons);
            for (const std::string& w : written) fprintf(inlog, "%d %s\n", f, w.c_str());
        }
        written.clear();
        if (f == iolog_frame) { g_iolog = true; printf("-- frame %d I/O\n", f); }
        if (f == pcs_frame || f == trace_frame) {
            // the frame's instructions, counted by address
            std::map<uint32_t, uint64_t> hist;
            m.frame_done = false;
            while (!m.frame_done) {
                if (m.t >= m.next) { m.events(); continue; }
                hist[m.cpu.pc]++;
                if (f == trace_frame && traced++ < trace_n)
                    printf("  L%3d %06X %04X SR %04X D0 %08X D1 %08X A0 %08X A7 %08X\n", m.line, m.cpu.pc & 0xFFFFFF,
                           bus_r16(m.cpu, m.cpu.pc), m.cpu.sr, m.cpu.d[0], m.cpu.d[1], m.cpu.a[0], m.cpu.a[7]);
                const int c = m68k_step(m.cpu);
                m.t += uint64_t(c) * CPU_T;
                m.cpu.cyc = 0;
            }
            std::vector<std::pair<uint64_t, uint32_t>> v;
            for (auto& [pc, n] : hist) v.push_back({n, pc});
            std::sort(v.rbegin(), v.rend());
            printf("-- frame %d PCs (%zu distinct)\n", f, v.size());
            for (size_t i = 0; i < v.size() && i < size_t(pcs_n); i++) printf("  %06X %llu\n", v[i].second, (unsigned long long)v[i].first);
        } else if (interp) { log_m.clear(); log_ref.clear(); m.run_frame(); }
        else { log_m.clear(); log_ref.clear(); m.run_frame_native(); }
        g_iolog = false;
        if (lockstep) {
            ref.pad[0].buttons = m.pad[0].buttons;
            ref.run_frame();
            if (md_hash(m) != md_hash(ref)) {
                if (steplog) {
                    size_t k = 0;
                    while (k < log_m.size() && k < log_ref.size() && (log_m[k].first & 0xFFFFFF) == (log_ref[k].first & 0xFFFFFF) &&
                           log_m[k].second == log_ref[k].second) k++;
                    printf("steplog: the first difference at instruction %zu of the frame%s", k, "\n");
                    for (size_t j = k >= 6 ? k - 6 : 0; j < k + 3; j++) {
                        if (j < log_m.size()) printf("  native %s %06X t %llu", (log_m[j].first & 0x80000000u) ? "(interp)" : "        ",
                                                     log_m[j].first & 0xFFFFFF, (unsigned long long)log_m[j].second);
                        if (j < log_ref.size()) printf("   |  interp %06X t %llu", log_ref[j].first, (unsigned long long)log_ref[j].second);
                        printf("%s", "\n");
                    }
                }
                printf("lockstep: frame %d differs:", f);
                if (m.t != ref.t) printf(" t %llu/%llu", (unsigned long long)m.t, (unsigned long long)ref.t);
                if (std::memcmp(m.cpu.d, ref.cpu.d, sizeof m.cpu.d) || std::memcmp(m.cpu.a, ref.cpu.a, sizeof m.cpu.a) ||
                    m.cpu.pc != ref.cpu.pc || m.cpu.sr != ref.cpu.sr) printf(" CPU (PC %06X/%06X)", m.cpu.pc, ref.cpu.pc);
                if (std::memcmp(m.ram, ref.ram, sizeof m.ram)) printf(" RAM");
                if (std::memcmp(m.zram, ref.zram, sizeof m.zram)) printf(" Z80-RAM");
                if (std::memcmp(m.vdp.vram, ref.vdp.vram, sizeof m.vdp.vram)) printf(" VRAM");
                if (std::memcmp(m.vdp.cram, ref.vdp.cram, sizeof m.vdp.cram)) printf(" CRAM");
                if (std::memcmp(m.vdp.out, ref.vdp.out, sizeof m.vdp.out)) printf(" picture");
                printf("\n");
                return 1;
            }
        }
        if (f == ram_frame) {       // the work RAM, the Z80's and VRAM, raw
            FILE* o = fopen((out + "/ram.bin").c_str(), "wb");
            if (o) { fwrite(m.ram, 1, sizeof m.ram, o); fclose(o); }
            o = fopen((out + "/zram.bin").c_str(), "wb");
            if (o) { fwrite(m.zram, 1, sizeof m.zram, o); fclose(o); }
            o = fopen((out + "/vram.bin").c_str(), "wb");
            if (o) { fwrite(m.vdp.vram, 1, sizeof m.vdp.vram, o); fclose(o); }
            o = fopen((out + "/cram.bin").c_str(), "wb");
            if (o) { fwrite(m.vdp.cram, 2, 64, o); fclose(o); }
            o = fopen((out + "/vsram.bin").c_str(), "wb");
            if (o) { fwrite(m.vdp.vsram, 2, 40, o); fclose(o); }
            o = fopen((out + "/vdpregs.bin").c_str(), "wb");
            if (o) { fwrite(m.vdp.reg, 1, 32, o); fclose(o); }
            o = fopen((out + "/satcache.bin").c_str(), "wb");
            if (o) { fwrite(m.vdp.sat_cache, 2, 0x200, o); fclose(o); }
        }
        if (snaps.count(f)) {
            char name[64];
            snprintf(name, sizeof name, "/f%05d.ppm", f);
            md_write_ppm(out + name, m);
        }
    }
    if (inlog) fclose(inlog);
    m.sound.close(m.t);
    if (!cover_path.empty()) {
        FILE* o = fopen(cover_path.c_str(), "wb");
        if (o) {
            for (uint32_t i = 0; i < cov.size(); i++) if (cov[i]) fprintf(o, "%06X%s", i * 2, "\n");
            fclose(o);
        }
        // the access sites, beside it: "PC MASK" (two bits a class, md.h's IoClass: read, write)
        o = fopen((cover_path + ".sites").c_str(), "wb");
        if (o) {
            for (auto& [pc, mask] : site_map) fprintf(o, "%06X %08X%s", pc, mask, "\n");
            fclose(o);
        }
        // the Z80's code, beside it: "ADDR BYTE", each Z80 RAM byte run as code and its value then
        o = fopen((cover_path + ".z80").c_str(), "wb");
        if (o) {
            for (uint32_t i = 0; i < zcov.size(); i++) if (zcov[i]) fprintf(o, "%04X %02X%s", i, zcov[i] & 0xFF, "\n");
            fclose(o);
        }
#ifdef MD_TAINT
        taint_save((cover_path + ".taint").c_str());
#endif
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (lockstep) printf("lockstep: %d frames identical%s", frames, "\n");
    if (!tape_path.empty()) printf("tape: %d frames, %d timeouts%s", frames, tape.timeouts, "\n");
    if (!interp) printf("native %llu, interpreted %llu instructions%s", (unsigned long long)m.n_native,
                        (unsigned long long)m.n_interp, "\n");
    printf("%d frames in %.2f s (%.0f fps)  hash %016llx\n", frames, secs, frames / secs, (unsigned long long)md_hash(m));
    if (survey) {               // one line of key=value: tools/survey.py reads it
        const Survey& v = m.sv;
        printf("survey frames=%u h40=%u h32=%u v30=%u interlace=%u shadow=%u window=%u display_off=%u "
               "hs_full=%u hs_cell=%u hs_line=%u vs_column=%u spr_line_max=%u spr_ovf_lines=%u "
               "dma_words=%llu dma_frame_max=%u hint_frames=%u hint_frame_max=%u "
               "cram_active=%u vsram_active=%u reg_active=%u status_reads=%u hv_reads=%u "
               "z80_uploads=%u z80_hash=%016llx ym27=%02X ym_writes=%u ym_writes_68k=%u keyons=%u dac_writes=%u "
               "psg_writes=%u psg_writes_68k=%u%s",
               v.frames, v.h40, v.h32, v.v30, v.interlace, v.shadow, v.window, v.display_off,
               v.hs_mode[0], v.hs_mode[2], v.hs_mode[3], v.vs_column, v.spr_line_max, v.spr_ovf_lines,
               (unsigned long long)v.dma_words, v.dma_frame_max, v.hint_frames, v.hint_frame_max,
               v.cram_active, v.vsram_active, v.reg_active, v.status_reads, v.hv_reads,
               v.z80_uploads, (unsigned long long)v.z80_hash, v.ym27, m.ym.writes, m.ym.writes_68k, m.ym.keyons,
               m.ym.dac_writes, m.ym.psg_writes, m.ym.psg_writes_68k, "\n");
    }
    if (state) {
        printf("PC %06X SR %04X  D", m.cpu.pc, m.cpu.sr);
        for (int i = 0; i < 8; i++) printf(" %08X", m.cpu.d[i]);
        printf("\n  A");
        for (int i = 0; i < 8; i++) printf(" %08X", m.cpu.a[i]);
        printf("\n  VDP regs");
        for (int i = 0; i < 24; i++) printf(" %02X", m.vdp.reg[i]);
        printf("\n  ints: VINT %u HINT %u  Z80 PC %04X %s%s  YM writes %u\n", m.vints, m.hints, m.z80.PC,
               m.z80_reset ? "reset " : "", m.z80_busreq ? "busreq" : "", m.ym.writes);
        printf("  sound: YM writes %u (68000 %u) key-ons %u DAC %u  PSG writes %u (68000 %u)\n",
               m.ym.writes, m.ym.writes_68k, m.ym.keyons, m.ym.dac_writes, m.ym.psg_writes, m.ym.psg_writes_68k);
    }
    return 0;
}

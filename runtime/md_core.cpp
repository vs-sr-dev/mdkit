// The Mega Drive's bus, I/O, Z80 side, YM2612 timers, pads and the line events (see md.h).
#include "md.h"
#include "md_native.h"

// ---- the 68000's view of the machine ----

// the hardware an address of the 68000's I/O belongs to (the tracer's access sites)
int io_class(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < 0x400000) return IOC_CART;
    if (a < 0xA00000) return IOC_OTHER;
    if (a < 0xA04000) return IOC_Z80RAM;
    if (a < 0xA06000) return IOC_YM;
    if (a < 0xA10000) return IOC_Z80RAM;
    if (a < 0xA10002) return IOC_VERSION;
    if (a < 0xA10020) return IOC_PAD;
    if ((a & 0xFFFF00) == 0xA11100 || (a & 0xFFFF00) == 0xA11200) return IOC_Z80BUS;
    if ((a & 0xFFFF00) == 0xA13000) return IOC_MAPPER;
    if ((a & 0xFFF000) == 0xA14000) return IOC_TMSS;
    if (a >= 0xC00000 && a < 0xE00000) {
        const uint32_t r = a & 0x1F;
        return r < 4 ? IOC_VDPDATA : r < 8 ? IOC_VDPCTRL : r < 0x10 ? IOC_HV : r < 0x18 ? IOC_PSG : IOC_OTHER;
    }
    return IOC_OTHER;
}
static inline void site(MD& m, uint32_t a, bool w) {
    if (m.sites) (*m.sites)[m.cpu.ipc & 0xFFFFFF] |= 1u << (io_class(a) * 2 + (w ? 1 : 0));
}
static uint8_t cb_r8(void* p, uint32_t a) { MD& m = *static_cast<MD*>(p); site(m, a, false); return m.io_r8(a); }
static uint16_t cb_r16(void* p, uint32_t a) { MD& m = *static_cast<MD*>(p); site(m, a, false); return m.io_r16(a); }
static void cb_w8(void* p, uint32_t a, uint8_t v) { MD& m = *static_cast<MD*>(p); site(m, a, true); m.io_w8(a, v); }
static void cb_w16(void* p, uint32_t a, uint16_t v) { MD& m = *static_cast<MD*>(p); site(m, a, true); m.io_w16(a, v); }
static int cb_iack(void* p, int) {
    static_cast<MD*>(p)->irq_ack();
    return -1;                  // the autovector
}

void MD::init(const std::vector<uint8_t>& rom_image, Machine mach, uint32_t sram_bytes, const std::string& slot) {
    machine = mach;
    pal = mach == M_PAL;
    lines = pal ? 313 : 262;
    // the ROM padded to a power of two, the rest a mirror of what precedes it
    size_t n = 0x10000;
    while (n < rom_image.size()) n <<= 1;
    rom.assign(n, 0xFF);
    std::memcpy(rom.data(), rom_image.data(), rom_image.size());
    for (size_t i = rom_image.size(); i < n && !rom_image.empty(); i++) rom[i] = rom[i % rom_image.size()];
    // the SRAM as MAME has it: 16-bit words from $200000 ($300000 on HardBall '95), in view
    // from the start on a cart of 2 MB or less, and always for FRAM
    sram.assign((sram_bytes + 1) / 2, 0xFFFF);
    sram_start = slot == "rom_hardbl95" ? 0x300000 : 0x200000;
    sram_end = sram_start + (sram_bytes ? sram_bytes - 1 : 0);
    sram_fram = slot == "rom_fram";
    rom_bytes = uint32_t(rom_image.size());
    if (!cart_setup(cart, slot)) cart_setup(cart, "rom");
    zglue.md = this;
    z80.bus = &zglue;
    cpu.ctx = this;
    cpu.io_r8 = cb_r8; cpu.io_r16 = cb_r16; cpu.io_w8 = cb_w8; cpu.io_w16 = cb_w16;
    cpu.iack = cb_iack;
    cpu.tas_writes = false;     // the Mega Drive's bus does not let TAS write back
    reset();
}

void MD::reset() {
    std::memset(ram, 0, sizeof ram);
    std::memset(zram, 0, sizeof zram);
    std::memset(&vdp, 0, sizeof vdp);
    std::memset(&ym, 0, sizeof ym);
    std::memset(&psg, 0, sizeof psg);
    std::memset(&sv, 0, sizeof sv);
    sv_hints0 = sv_hints_on = 0;
    std::memset(pad, 0, sizeof pad);
    vdp.width = 320; vdp.height = 224; vdp.out_w = 320; vdp.out_h = 224;
    vdp.hint_counter = 0;
    vdp.vint_line = vdp.hint_line = NEVER;
    irq_next = NEVER;
    // the 68000's pages: the cartridge's 4 MB, the work RAM's 64 KB at $E00000-$FFFFFF
    for (int p = 0; p < 256; p++) { cpu.rpage[p] = cpu.wpage[p] = nullptr; cpu.pmask[p] = 0; }
    const uint32_t rom_pages = uint32_t(rom.size() >> 16);
    for (uint32_t p = 0; p < 0x40; p++) {
        cpu.rpage[p] = rom.data() + (uint64_t(p % rom_pages) << 16);
        cpu.pmask[p] = 0xFFFF;
    }
    if (!sram.empty())          // the SRAM's pages answer through I/O
        for (uint32_t p = sram_start >> 16; p <= (sram_end >> 16) && p < 0x40; p++) cpu.rpage[p] = nullptr;
    sram_on = sram_fram || rom_bytes <= sram_start;
    sram_ro = false;
    cpu.map(0xE0, 0x20, ram, 0xFFFF, true);
    cart_map();
    t = 0; frame = 0; line = 0; line_t = 0; line_drawn = line_hblank = false;
    z80.reset();
    zt = 0; z80_busreq = false; z80_reset = true; zbank = 0;
    pad[0].data = pad[1].data = 0x7F;
    pad[0].six = pad[1].six = false;
    m68k_reset(cpu);
    t = uint64_t(cpu.cyc) * CPU_T;
    cpu.cyc = 0;
    next = 0;
}

// ---- interrupts ----

void MD::update_irq() {
    int level = 0;
    irq_next = NEVER;
    auto see = [&](uint64_t line, int lv) {
        if (line == NEVER) return;
        if (line + IRQ_SAMPLE_T <= t) { if (lv > level) level = lv; }
        else if (line + IRQ_SAMPLE_T < irq_next) irq_next = line + IRQ_SAMPLE_T;
    };
    see(vdp.vint_line, 6);
    see(vdp.hint_line, 4);
    m68k_set_irq(cpu, level);
    if (irq_next < next) next = irq_next;
}

// the 68000 acknowledges an interrupt: the VDP clears the one it is raising (MAME's irq_ack)
void MD::irq_ack() {
    if (vdp.vint_pending && (vdp.reg[1] & 0x20)) { vdp.vint_pending = false; vdp.vint_line = NEVER; }
    else if (vdp.hint_pending && (vdp.reg[0] & 0x10)) { vdp.hint_pending = false; vdp.hint_line = NEVER; }
    update_irq();
}

void MD::irq_reg_written(int r) {
    const uint64_t n = now();
    const bool pending = r == 0 ? vdp.hint_pending : vdp.vint_pending;
    const bool on = r == 0 ? (vdp.reg[0] & 0x10) : (vdp.reg[1] & 0x20);
    uint64_t& line = r == 0 ? vdp.hint_line : vdp.vint_line;
    if (pending && on) { if (line == NEVER || line > n) line = n + IRQ_ENABLE_T; }
    else line = NEVER;
    update_irq();
}

// ---- the line events ----

void MD::events() {
    for (;;) {
        if (!line_drawn) {
            if (t < line_t) break;
            line_drawn = true;
            const int act = vdp_active_lines();
            if (line == 0) {
                vdp.vblank = false;
                vdp.width = vdp_h40() ? 320 : 256;
                vdp.height = act;
                // the survey: the frame's modes, its DMA and HINTs
                Survey& v = sv;
                const uint8_t* r = vdp.reg;
                v.frames++;
                if (r[12] & 1) v.h40++; else v.h32++;
                if (r[1] & 8) v.v30++;
                if (r[12] & 6) v.interlace++;
                if (r[12] & 8) v.shadow++;
                if ((r[17] & 0x9F) || (r[18] & 0x9F)) v.window++;
                if (!(r[1] & 0x40)) v.display_off++;
                if (r[11] & 4) v.vs_column++;
                v.hs_mode[r[11] & 3]++;
                v.dma_words += dma_words;
                if (dma_words > v.dma_frame_max) v.dma_frame_max = uint32_t(dma_words);
                dma_words = 0;
                const uint32_t h = sv_hints_on - sv_hints0;
                if (h > v.hint_frame_max) v.hint_frame_max = h;
                if (h) v.hint_frames++;
                sv_hints0 = sv_hints_on;
            }
            if (line == act) {
                vdp.vblank = true;
                vdp.vint_pending = true;
                vdp.vint_line = (vdp.reg[1] & 0x20) ? line_t + IRQ_RISE_T : NEVER;
                vints++;
                z80_sync();
                z80.irq = true;
            } else if (line == act + 1) {
                z80_sync();
                z80.irq = false;
            }
            if (line < act && line < MAX_H) vdp_render_line(line);
            continue;
        }
        if (!line_hblank) {
            if (t < line_t + HINT_T) break;
            line_hblank = true;
            // the V counter steps here, and the HINT counter with it, for the line V enters:
            // it counts down on lines 0-224 and is reloaded on the others (MAME's), so the
            // handler of a HINT reads the line it was raised for
            const int vnext = line + 1 == lines ? 0 : line + 1;
            if (vnext <= 224) {
                if (--vdp.hint_counter < 0) {
                    vdp.hint_counter = vdp.reg[10];
                    vdp.hint_pending = true;
                    vdp.hint_line = (vdp.reg[0] & 0x10) ? line_t + HINT_T + IRQ_RISE_T : NEVER;
                    hints++;
                    if (vdp.reg[0] & 0x10) sv_hints_on++;
                }
            } else vdp.hint_counter = vdp.reg[10];
            continue;
        }
        if (t < line_t + LINE_T) break;
        line_t += LINE_T;
        line_drawn = line_hblank = false;
        if (++line == lines) {
            line = 0;
            vdp_end_frame();
            frame++;
            frame_done = true;
        }
    }
    next = !line_drawn ? line_t : !line_hblank ? line_t + HINT_T : line_t + LINE_T;
    update_irq();
}

void MD::run_frame() {
    frame_done = false;
    while (!frame_done) {
        if (t >= next) { events(); continue; }
        if (cover) (*cover)[(cpu.pc & 0xFFFFFF) >> 1] = 1;     // 24 bits: jsr $c500.w runs $FFFFC500, that is $FFC500
        if (steplog) steplog->push_back({cpu.pc, t});
        TAINT(taint_begin(cpu);)
        const bool stop = cpu.stopped;
        const int c = m68k_step(cpu);
        TAINT(taint_end(cpu);)
        t += uint64_t(c) * CPU_T;
        cpu.cyc = 0;
        if (stop) n_stop++; else n_interp++;
    }
}

bool md_poll(MD& m, uint32_t a) {
    if (md_leave(m)) { m.cpu.pc = a; return true; }
    if (m.steplog) m.steplog->push_back({a, m.t});
    return false;
}

void md_op(MD& m, uint32_t a, uint16_t op) {
    M68K& c = m.cpu;
    c.ipc = a; c.ir = op; c.pc = a + 2; c.cyc = 4;
    m68k_op[op](c);
    if (c.pc & 1) address_error(c, c.pc, true, true, prog_fc(c));
    m.t += uint64_t(c.cyc) * CPU_T; c.cyc = 0; m.n_native++;
}

// the function of the recompiled code holding pc, or null
static NativeFn native_find(uint32_t pc) {
    int lo = 0, hi = md_native_n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (pc < md_native[mid].lo) hi = mid;
        else if (pc >= md_native[mid].hi) lo = mid + 1;
        else return md_native[mid].fn;
    }
    return nullptr;
}

void MD::run_frame_native() {
    frame_done = false;
    while (!frame_done) {
        if (t >= next) { events(); continue; }
        if (!md_leave(*this)) {
            if (NativeFn f = native_find(cpu.pc)) {
                const uint64_t before = n_native;
                f(*this, cpu.pc);
                if (n_native != before || cpu.halted) continue;
            }
        }
        if (steplog) steplog->push_back({cpu.pc | 0x80000000u, t});
        const bool stop = cpu.stopped;
        const int c = m68k_step(cpu);
        t += uint64_t(c) * CPU_T;
        cpu.cyc = 0;
        if (stop) n_stop++; else n_interp++;
    }
}

// ---- the Z80 ----

void MD::z80_sync() {
    const uint64_t target = now();
    if (z80_reset || z80_busreq) { if (zt < target) zt = target; return; }
    z80_running = true;
    if (!zcover) while (zt < target) zt += uint64_t(z80.step()) * Z80_T;
    else while (zt < target) {
        // the code the Z80 runs from its RAM: an instruction's bytes when it falls through, its first byte when it jumps
        const u16 pc = z80.PC;
        zt += uint64_t(z80.step()) * Z80_T;
        if (pc >= 0x4000) continue;
        const int n = (z80.PC - pc) & 0xFFFF;
        for (int k = 0; k < (n >= 1 && n <= 4 ? n : 1); k++) {
            const u16 a = (pc + k) & 0x1FFF;
            if (!(*zcover)[a]) (*zcover)[a] = uint16_t(0x100 | zram[a]);
        }
    }
    z80_running = false;
}

u8 Z80Glue::rd(u16 a) {
    MD& m = *md;
    if (a < 0x4000) return m.zram[a & 0x1FFF];
    if (a < 0x6000) return m.ym_status();
    if (a < 0x7F00) return 0xFF;
    if (a < 0x8000) {
        if ((a & 0xFC) == 0x08) return uint8_t(m.vdp_hv() >> ((a & 1) ? 0 : 8));
        return 0xFF;
    }
    const uint32_t addr = (uint32_t(m.zbank) << 15) | (a & 0x7FFF);
    if (addr >= 0xA00000 && addr < 0xA10000) return 0xFF;      // the Z80 reaching its own bus: nothing answers
    return bus_r8(m.cpu, addr);
}

void Z80Glue::wr(u16 a, u8 v) {
    MD& m = *md;
    if (a < 0x4000) { m.zram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { m.ym_write(a & 3, v); return; }
    if (a < 0x6100) { m.zbank = uint16_t(((m.zbank >> 1) | ((v & 1) << 8)) & 0x1FF); return; }
    if (a < 0x7F00) return;
    if (a < 0x8000) {
        if ((a & 0xF8) == 0x10) m.psg_write(v);
        return;
    }
    const uint32_t addr = (uint32_t(m.zbank) << 15) | (a & 0x7FFF);
    if (addr >= 0xA00000 && addr < 0xA10000) return;
    if (addr >= 0xE00000) { m.ram[addr & 0xFFFF] = v; return; }
    if (addr >= 0xC00000 && addr < 0xE00000) { m.vdp_write(addr, uint16_t(v << 8 | v), true); return; }
}

uint8_t MD::z80_space_r8(uint32_t a) {
    z80_sync();
    const uint32_t o = a & 0xFFFF;
    if (o < 0x4000) return zram[o & 0x1FFF];
    if (o < 0x6000) return ym_status();
    if (o >= 0x7F00 && o < 0x8000 && (o & 0xFC) == 0x08) return uint8_t(vdp_hv() >> ((o & 1) ? 0 : 8));
    return 0xFF;
}

void MD::z80_space_w8(uint32_t a, uint8_t v) {
    z80_sync();
    const uint32_t o = a & 0xFFFF;
    if (o < 0x4000) zram[o & 0x1FFF] = v;
    else if (o < 0x6000) ym_write(o & 3, v);
    else if (o < 0x6100) zbank = uint16_t(((zbank >> 1) | ((v & 1) << 8)) & 0x1FF);
    else if (o >= 0x7F00 && o < 0x8000 && (o & 0xF8) == 0x10) psg_write(v);
}

// ---- the YM2612: registers and the two timers (the sound itself: md_sound) ----

static void ym_timers(MD& m) {
    YM& y = m.ym;
    const uint64_t now = m.bus_time();
    const uint8_t r27 = y.reg[0][0x27];
    const uint64_t pa = uint64_t(1024 - ((y.reg[0][0x24] << 2) | (y.reg[0][0x25] & 3))) * 1008;
    const uint64_t pb = uint64_t(256 - y.reg[0][0x26]) * 16 * 1008;
    while (y.ta_next && now >= y.ta_next) { if (r27 & 4) y.status |= 1; y.ta_next += pa; }
    while (y.tb_next && now >= y.tb_next) { if (r27 & 8) y.status |= 2; y.tb_next += pb; }
}

uint8_t MD::ym_status() {
    ym_timers(*this);
    return ym.status;
}

void MD::ym_write(int port, uint8_t v) {
    ym_timers(*this);
    if (sound.on()) sound.ym_write(bus_time(), port, v);
    const int part = port >> 1;
    if (!(port & 1)) { ym.addr[part] = v; return; }
    const uint8_t r = ym.addr[part];
    ym.writes++;
    if (!z80_running) ym.writes_68k++;
    if (part == 0 && r == 0x28 && (v & 0xF0)) ym.keyons++;
    if (part == 0 && r == 0x2A) ym.dac_writes++;
    if (part == 0 && r == 0x27) {
        sv.ym27 |= v;
        const uint8_t old = ym.reg[0][0x27];
        const uint64_t pa = uint64_t(1024 - ((ym.reg[0][0x24] << 2) | (ym.reg[0][0x25] & 3))) * 1008;
        const uint64_t pb = uint64_t(256 - ym.reg[0][0x26]) * 16 * 1008;
        if ((v & 1) && !(old & 1)) ym.ta_next = bus_time() + pa;
        if (!(v & 1)) ym.ta_next = 0;
        if ((v & 2) && !(old & 2)) ym.tb_next = bus_time() + pb;
        if (!(v & 2)) ym.tb_next = 0;
        if (v & 0x10) ym.status &= ~1;
        if (v & 0x20) ym.status &= ~2;
    }
    ym.reg[part][r] = v;
}

void MD::psg_write(uint8_t v) {
    if (sound.on()) sound.psg_write(bus_time(), v);
    ym.psg_writes++;
    if (!z80_running) ym.psg_writes_68k++;
    if (v & 0x80) {
        psg.latch = (v >> 4) & 7;
        psg.reg[psg.latch] = uint16_t((psg.reg[psg.latch] & 0x3F0) | (v & 0x0F));
    } else if (psg.latch & 1 || psg.latch == 6) {
        psg.reg[psg.latch] = v & 0x0F;
    } else {
        psg.reg[psg.latch] = uint16_t((psg.reg[psg.latch] & 0x0F) | ((v & 0x3F) << 4));
    }
}

// ---- the pads ----

enum { B_UP = 1, B_DOWN = 2, B_LEFT = 4, B_RIGHT = 8, B_A = 16, B_B = 32, B_C = 64, B_START = 128,
       B_X = 256, B_Y = 512, B_Z = 1024, B_MODE = 2048 };

uint8_t MD::pad_read(int n) {
    Pad& p = pad[n];
    if (p.phase && now() - p.th_at > 80000) p.phase = 0;      // about 1.5 ms without a TH edge
    const bool th = (p.ctrl & 0x40) ? (p.data & 0x40) : true;
    const uint16_t b = p.buttons;
    auto bit = [&](uint16_t m, int pos) { return (b & m) ? 0 : (1 << pos); };
    uint8_t v;
    if (th) {
        if (p.six && p.phase == 6) v = 0x40 | bit(B_C, 5) | bit(B_B, 4) | bit(B_MODE, 3) | bit(B_X, 2) | bit(B_Y, 1) | bit(B_Z, 0);
        else v = 0x40 | bit(B_C, 5) | bit(B_B, 4) | bit(B_RIGHT, 3) | bit(B_LEFT, 2) | bit(B_DOWN, 1) | bit(B_UP, 0);
    } else {
        if (p.six && p.phase == 5) v = bit(B_START, 5) | bit(B_A, 4);
        else if (p.six && p.phase == 7) v = bit(B_START, 5) | bit(B_A, 4) | 0x0F;
        else v = bit(B_START, 5) | bit(B_A, 4) | bit(B_DOWN, 1) | bit(B_UP, 0);
    }
    return uint8_t(((v & ~p.ctrl) | (p.data & p.ctrl)) & 0x7F) | (p.data & 0x80);
}

void MD::pad_write_data(int n, uint8_t v) {
    Pad& p = pad[n];
    const uint8_t old = p.data;
    p.data = v;
    if ((p.ctrl & 0x40) && ((old ^ v) & 0x40)) {
        if (p.phase && now() - p.th_at > 80000) p.phase = 0;
        p.phase = (p.phase + 1) & 7;
        p.th_at = now();
    }
}

// ---- the 68000's I/O ----

uint8_t MD::io_r8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < 0x400000) {
        uint16_t w;
        if (cart_r16(a, w)) return (a & 1) ? uint8_t(w) : uint8_t(w >> 8);
        // the SRAM, in view
        if (sram_on && !sram.empty() && a >= sram_start && a <= sram_end) {
            const uint16_t w = sram[(a - sram_start) >> 1];
            return (a & 1) ? uint8_t(w) : uint8_t(w >> 8);
        }
        return rom[a & (rom.size() - 1)];
    }
    if (a >= 0xA00000 && a < 0xA10000) return z80_space_r8(a);
    if (a >= 0xA10000 && a < 0xA10020) {
        switch (a & 0x1E) {
        case 0x00: return uint8_t((machine != M_NTSC_J ? 0x80 : 0) | (pal ? 0x40 : 0) | 0x20);
        case 0x02: return pad_read(0);
        case 0x04: return pad_read(1);
        case 0x06: return io_ext_data;
        case 0x08: return pad[0].ctrl;
        case 0x0A: return pad[1].ctrl;
        case 0x0C: return io_ext_ctrl;
        default: return 0;
        }
    }
    if ((a & 0xFFFF00) == 0xA11100) {
        // the bit over the open bus: the 68000's prefetched word (MAME: random; Double Dragon,
        // Shadow of the Beast, Super Off Road and Time Killers wait on the other bits)
        const uint8_t bit = (z80_busreq && !z80_reset) ? 0 : 1;
        return (a & 1) ? bit : uint8_t((open_bus() >> 8 & 0xFE) | bit);
    }
    if ((a & 0xFFFF00) == 0xA11200) return 0;
    if (a >= 0xC00000 && a < 0xE00000) {
        const uint16_t w = vdp_read(a & ~1u);
        return (a & 1) ? uint8_t(w) : uint8_t(w >> 8);
    }
    return 0xFF;
}

uint16_t MD::io_r16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a >= 0xC00000 && a < 0xE00000) return vdp_read(a);
    if (a >= 0xA00000 && a < 0xA10000) { const uint8_t b = z80_space_r8(a); return uint16_t(b << 8 | b); }
    if ((a & 0xFFFF00) == 0xA11100) return uint16_t((open_bus() & 0xFEFF) | (((z80_busreq && !z80_reset) ? 0 : 1) << 8));
    if (a < 0x400000) {
        uint16_t w;
        if (cart_r16(a, w)) return w;
    }
    if (a < 0x400000 || (a >= 0xA10000 && a < 0xA10020)) {
        const uint8_t hi = (a < 0x400000) ? io_r8(a) : io_r8(a + 1), lo = io_r8(a + 1);
        return uint16_t(hi << 8 | lo);
    }
    return 0xFFFF;
}

void MD::io_w8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a < 0x400000) {
        cart_w16(a, (a & 1) ? v : uint16_t(v << 8), (a & 1) ? 0x00FF : 0xFF00);
        // MAME's: the word written whole, a byte in its lane and the other lane zero
        if (sram_on && !sram_ro && !sram.empty() && a >= sram_start && a <= sram_end)
            sram[(a - sram_start) >> 1] = (a & 1) ? v : uint16_t(v << 8);
        return;
    }
    if (a < 0x800000) { cart_w16(a, (a & 1) ? v : uint16_t(v << 8), (a & 1) ? 0x00FF : 0xFF00); return; }
    if (a >= 0xA00000 && a < 0xA10000) { z80_space_w8(a, v); return; }
    if (a >= 0xA10000 && a < 0xA10020) {
        switch (a & 0x1E) {
        case 0x02: pad_write_data(0, v); break;
        case 0x04: pad_write_data(1, v); break;
        case 0x06: io_ext_data = v; break;
        case 0x08: pad[0].ctrl = v; break;
        case 0x0A: pad[1].ctrl = v; break;
        case 0x0C: io_ext_ctrl = v; break;
        }
        return;
    }
    if ((a & 0xFFFF00) == 0xA11100) {
        if (!(a & 1)) { z80_sync(); z80_busreq = v & 1; }
        return;
    }
    if ((a & 0xFFFF00) == 0xA11200) {
        if (!(a & 1)) {
            z80_sync();
            const bool hold = !(v & 1);
            if (z80_reset && !hold) {
                z80.reset(); zt = now();
                uint64_t hsh = 1469598103934665603ull;
                for (uint8_t b : zram) { hsh ^= b; hsh *= 1099511628211ull; }
                sv.z80_hash = hsh;
                sv.z80_uploads++;
            }
            z80_reset = hold;
        }
        return;
    }
    if ((a & 0xFFFFF0) == 0xA130F0) { if (a & 1) cart_a13_w(a, v); return; }
    if (a >= 0xC00000 && a < 0xE00000) { vdp_write(a, uint16_t(v << 8 | v), true); return; }
}

void MD::io_w16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xC00000 && a < 0xE00000) { vdp_write(a, v, false); return; }
    if (a >= 0xA00000 && a < 0xA10000) { z80_space_w8(a, uint8_t(v >> 8)); return; }
    if ((a & 0xFFFF00) == 0xA11100 || (a & 0xFFFF00) == 0xA11200) { io_w8(a, uint8_t(v >> 8)); return; }
    if (a >= 0xA10000 && a < 0xA10020) { io_w8(a + 1, uint8_t(v)); return; }
    if (a < 0x400000) {
        cart_w16(a, v, 0xFFFF);
        if (sram_on && !sram_ro && !sram.empty() && a >= sram_start && a <= sram_end)
            sram[(a - sram_start) >> 1] = v;
        return;
    }
    if ((a & 0xFFFFF0) == 0xA130F0) { cart_a13_w(a | 1, uint8_t(v)); return; }
    if (a >= 0x400000 && a < 0x800000) cart_w16(a, v, 0xFFFF);
}

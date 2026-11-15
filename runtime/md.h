// The Mega Drive / Genesis under recompiled code: the machine around the 68000.
//
// Time is counted in ticks of the master clock (53.693175 MHz NTSC, 53.203424 PAL):
// a 68000 cycle is 7 ticks, a Z80 T-state 15, a line 3420, a frame 262 lines (NTSC)
// or 313 (PAL).  A line's ticks start where its active display does: 2560 ticks of
// pixels (320 at MCLK/8 in H40, 256 at MCLK/10 in H32), then the horizontal blanking.
//
// The VDP works a line at a time, at fixed points of the line: the line is drawn at
// its tick 0 (with the registers, VRAM, CRAM and VSRAM of that moment), the HINT
// counter steps and HINT is raised at the line's end (HINT_T), VINT and
// the vertical blanking start at tick 0 of line 224 (240 in V30).  The Z80, the YM2612's
// timers and the pads are brought up to the present whenever the 68000 touches them,
// and at every event.
//
// The rule that holds the recompiled code and the interpreter together (as in gbkit):
// both make the same accesses in the same order with the same charges, and both look
// at the machine's events at every instruction boundary at or past md.next.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include "m68k.h"
#include "z80.h"
#include "md_cart.h"
#include "md_sound.h"

enum {
    LINE_T = 3420,              // ticks per line
    ACTIVE_T = 2560,            // the active display's ticks
    HBLANK_T = 2640,            // where the HBlank flag rises and V steps (about H counter $A5 in H40)
#ifndef HINT_AT
#define HINT_AT 3420
#endif
    // where the HINT counter steps and HINT is raised: at the line's end, the phase of VINT
    // (MAME raises both in its scanline callback; held to MAME, more of the library agrees
    // than with HINT at the blanking's start)
    HINT_T = HINT_AT,
    CPU_T = 7, Z80_T = 15,
    IRQ_RISE_T = 128, IRQ_ENABLE_T = 64, IRQ_SAMPLE_T = 4 * 7,
    MAX_W = 320, MAX_H = 240,
};

enum Machine { M_NTSC_U, M_NTSC_J, M_PAL };
constexpr uint64_t NEVER = ~0ull;

// the hardware the 68000's I/O reaches, by class (the tracer's access sites: two bits each,
// read and write); io_class() in md_core.cpp
enum IoClass { IOC_CART, IOC_Z80RAM, IOC_YM, IOC_VERSION, IOC_PAD, IOC_Z80BUS, IOC_MAPPER, IOC_TMSS,
               IOC_VDPDATA, IOC_VDPCTRL, IOC_HV, IOC_PSG, IOC_OTHER, IOC_N };
int io_class(uint32_t a);

struct VDP {
    uint8_t vram[0x10000];
    uint16_t cram[64], vsram[40];
    uint16_t sat_cache[0x200];  // the VDP's own copy of the sprite table's first words (y, size, link)
    uint8_t reg[32];
    uint16_t code, addr;        // the access mode and address of the data port
    bool pending;               // the control port has the first word of a command
    uint16_t cmd1;
    bool fill_pending;          // a VRAM fill waits for its data word
    bool vint_pending, hint_pending, vblank;
    // the interrupt lines to the 68000: the tick each rose (NEVER: low).  MAME's VDP raises
    // a line 128 ticks after the interrupt becomes pending (64 after an enable bit is set
    // over a pending one) and drops it at once; the 68000 samples it at the start of an
    // instruction's last prefetch, so a line is taken at a boundary 4 cycles past its rise
    uint64_t vint_line, hint_line;
    int hint_counter;
    bool sprite_ovf, sprite_col;
    bool odd_frame;
    // the frame being drawn
    uint32_t fb[MAX_W * MAX_H]; // xRGB
    int width, height;          // of the frame (320 or 256; 224 or 240)
    // ... and the last whole frame
    uint32_t out[MAX_W * MAX_H];
    int out_w, out_h;
};

// what the library does with the machine (the survey's columns, for the Jaguar side): counts only
struct Survey {
    uint32_t frames, h40, h32, v30, interlace, shadow, window, display_off, vs_column;
    uint32_t hs_mode[4];                 // full, (invalid), per cell, per line
    uint32_t spr_line_max, spr_ovf_lines;
    uint64_t dma_words;
    uint32_t dma_frame_max, hint_frame_max, hint_frames;
    uint32_t cram_active, vsram_active, reg_active;     // writes while the display draws
    uint32_t status_reads, hv_reads;
    uint32_t z80_uploads;                // the Z80 let out of reset
    uint64_t z80_hash;                   // its RAM (the sound driver) when last let out
    uint8_t ym27;                        // YM2612 register $27's bits ever set: timers, CSM, channel 3 mode
};

struct Pad {
    uint16_t buttons;           // 1 = pressed: bits 0-11 U D L R A B C S X Y Z M
    bool six;                   // a six-button pad
    uint8_t data, ctrl;         // the port's data and direction registers ($A10003 / $A10009)
    int phase;                  // the six-button pad's TH counter
    uint64_t th_at;             // the tick of the last TH change (the counter resets 1.5 ms later)
};

struct YM {
    uint8_t reg[2][256];
    uint8_t addr[2];
    uint64_t ta_next, tb_next;  // the ticks of the timers' next overflows (0: stopped)
    uint8_t status;             // the overflow flags as last updated
    uint32_t writes;            // data writes (for the survey)
    uint32_t writes_68k, keyons, dac_writes, psg_writes, psg_writes_68k;   // ... by whom and what
};

struct PSG {
    uint8_t latch;
    uint16_t reg[8];
};

struct MD;
struct Z80Glue : Z80Bus {
    MD* md;
    u8 rd(u16 a) override;
    void wr(u16 a, u8 v) override;
    u8 in(u16) override { return 0xFF; }
    void out(u16, u8) override {}
};

struct MD {
    M68K cpu;
    Machine machine = M_NTSC_U;
    bool pal = false;
    int lines = 262;            // per frame

    // ---- time ----
    uint64_t t = 0;             // ticks
    uint64_t next = 0;          // the next tick something happens
    uint64_t frame = 0;         // frames begun
    int line = 0;               // the line being run (0 = the first active one)
    uint64_t line_t = 0;        // the tick the line began
    bool line_drawn = false, line_hblank = false;
    bool frame_done = false;    // a frame ended at the last event
    uint64_t irq_next = NEVER;  // the tick an interrupt line becomes visible to the 68000

    // ---- memory ----
    std::vector<uint8_t> rom;   // the cartridge, as the list has it
    uint8_t ram[0x10000];
    uint8_t zram[0x2000];
    std::vector<uint16_t> sram; // the save memory (SRAM or FRAM), in 16-bit words
    bool sram_on = false, sram_ro = false;     // $A130F1: in view, read-only
    bool sram_fram = false;
    uint32_t sram_start = 0x200000, sram_end = 0x20FFFF;
    uint32_t rom_bytes = 0;     // the image's own size

    // ---- the Z80 side ----
    Z80 z80;
    Z80Glue zglue;
    uint64_t zt = 0;            // ticks the Z80 has run to
    bool z80_busreq = false, z80_reset = true;   // $A11100, $A11200 (reset held at power-on)
    uint16_t zbank = 0;         // the 68000 window's bank (9 bits, shifted in a bit at a time)
    bool z80_irq_line = false;
    uint64_t z80_irq_end = 0;

    YM ym;
    PSG psg;
    Sound sound;                // the WAV and VGM outputs, when asked for
    bool z80_running = false;   // inside z80_sync: the Z80's accesses are at zt
    VDP vdp;
    Pad pad[4];                 // 3 and 4: the J-Cart's
    CartHW cart;
    uint8_t io_ext_data = 0, io_ext_ctrl = 0;

    // ---- counters (for the survey) ----
    Survey sv;
    uint32_t sv_hints0 = 0, sv_hints_on = 0;   // HINTs raised while enabled
    uint64_t dma_words = 0;     // 68000 to VDP DMA, words, this frame
    uint32_t hints = 0, vints = 0;

    void init(const std::vector<uint8_t>& rom_image, Machine m, uint32_t sram_bytes, const std::string& slot = "rom");
    void reset();
    // run until the end of the current frame (interpreted)
    void run_frame();
    // ... the recompiled code where there is some (md_native.h), the interpreter elsewhere
    void run_frame_native();
    uint64_t n_native = 0, n_interp = 0;        // instructions run each way
    std::vector<uint8_t>* cover = nullptr;
    std::map<uint32_t, uint32_t>* sites = nullptr;  // the tracer's: instruction -> the I/O classes it reached
    std::vector<std::pair<uint32_t, uint64_t>>* steplog = nullptr;  // --steplog: (pc, t) of each instruction      // the tracer's: the instruction starts run, by pc / 2
    // the events due at t: lines, interrupts; sets next
    void events();
    void update_irq();          // the 68000's level from the lines, as seen at the boundary t
    void irq_ack();
    void irq_reg_written(int r);  // VDP register 0 or 1 written: an enable over a pending interrupt

    // ---- the cartridge's hardware (md_cart.cpp) ----
    void cart_map();
    void ssf2_bank(int slot, int bank);
    bool cart_r16(uint32_t a, uint16_t& v);
    void cart_w16(uint32_t a, uint16_t data, uint16_t mask);
    void cart_a13_w(uint32_t a, uint8_t v);

    // the I/O side of the 68000's bus
    uint8_t io_r8(uint32_t a);
    uint16_t io_r16(uint32_t a);
    void io_w8(uint32_t a, uint8_t v);
    void io_w16(uint32_t a, uint16_t v);

    // the Z80 brought up to the present
    void z80_sync();
    uint8_t z80_space_r8(uint32_t a);       // the Z80's bus as the 68000 reaches it ($A00000-$A0FFFF)
    void z80_space_w8(uint32_t a, uint8_t v);

    uint8_t ym_status();
    void ym_write(int port, uint8_t v);
    void psg_write(uint8_t v);

    uint8_t pad_read(int n);
    void pad_write_data(int n, uint8_t v);

    // ---- the VDP (md_vdp.cpp) ----
    uint16_t vdp_read(uint32_t a);
    void vdp_write(uint32_t a, uint16_t v, bool byte);
    void vdp_ctrl_write(uint16_t v);
    void vdp_data_write(uint16_t v);
    uint16_t vdp_status();
    uint16_t vdp_hv();
    void vdp_dma();
    void vdp_render_line(int y);
    void vdp_end_frame();
    int vdp_active_lines() const { return (vdp.reg[1] & 8) && pal ? 240 : 224; }
    bool vdp_h40() const { return vdp.reg[12] & 1; }
    // what the 68000's data bus holds where nothing drives it: the word it prefetched
    uint16_t open_bus() { const uint8_t* p = cpu.rpage[(cpu.pc >> 16) & 0xFF]; return p ? bus_r16(cpu, cpu.pc & ~1u) : 0xFFFF; }
    // the time of an access to the sound chips: the Z80's own while it runs
    uint64_t bus_time() const { return z80_running ? zt : now(); }
    uint64_t now() const { return t + uint64_t(cpu.cyc) * CPU_T; }
};

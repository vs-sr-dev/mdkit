// The Mega Drive / Genesis under recompiled code: the machine around the 68000.
//
// Time is counted in ticks of the master clock (53.693175 MHz NTSC, 53.203424 PAL):
// a 68000 cycle is 7 ticks, a Z80 T-state 15, a line 3420, a frame 262 lines (NTSC)
// or 313 (PAL).  A line's ticks start where its active display does: 2560 ticks of
// pixels (320 at MCLK/8 in H40, 256 at MCLK/10 in H32), then the horizontal blanking.
//
// The VDP works a line at a time, at fixed points of the line: the line is drawn at
// its tick 0 (with the registers, VRAM, CRAM and VSRAM of that moment), the HINT
// counter steps and HINT is raised at the start of the blanking (HBLANK_T), VINT and
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
#include "m68k.h"
#include "z80.h"

enum {
    LINE_T = 3420,              // ticks per line
    ACTIVE_T = 2560,            // the active display's ticks
    HBLANK_T = 2640,            // where HINT comes and the HBlank flag rises (about H counter $A5 in H40)
    CPU_T = 7, Z80_T = 15,
    MAX_W = 320, MAX_H = 240,
};

enum Machine { M_NTSC_U, M_NTSC_J, M_PAL };

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
    int hint_counter;
    bool sprite_ovf, sprite_col;
    bool odd_frame;
    uint64_t dma_end;           // fill and copy run while the 68000 goes on: busy until here
    // the frame being drawn
    uint32_t fb[MAX_W * MAX_H]; // xRGB
    int width, height;          // of the frame (320 or 256; 224 or 240)
    // ... and the last whole frame
    uint32_t out[MAX_W * MAX_H];
    int out_w, out_h;
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

    // ---- memory ----
    std::vector<uint8_t> rom;   // the cartridge, as the list has it
    uint8_t ram[0x10000];
    uint8_t zram[0x2000];
    std::vector<uint8_t> sram;
    bool sram_on = false;       // $A130F1
    uint32_t sram_start = 0x200000, sram_end = 0x20FFFF;
    bool sram_odd = true;       // only the odd bytes (8-bit SRAM on the low lane)

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
    VDP vdp;
    Pad pad[2];
    uint8_t io_ext_data = 0, io_ext_ctrl = 0;

    // ---- counters (for the survey) ----
    uint64_t dma_words = 0;     // 68000 to VDP DMA, words, this frame
    uint32_t hints = 0, vints = 0;

    void init(const std::vector<uint8_t>& rom_image, Machine m, uint32_t sram_bytes);
    void reset();
    // run until the end of the current frame (interpreted)
    void run_frame();
    // the events due at t: lines, interrupts; sets next
    void events();
    void update_irq();

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
    uint64_t now() const { return t + uint64_t(cpu.cyc) * CPU_T; }
};

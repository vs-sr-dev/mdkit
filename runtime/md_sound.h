// The sound the machine makes: the YM2612 (ymfm, BSD-3, as a library) and the VDP's
// SN76489 PSG (after MAME's sn76496, the Sega variant), brought up to the present at
// every write and at every frame's end.  Output only, outside the state the lockstep
// compares (the YM2612's registers and timers live in MD::ym).
//
// Two products, both optional:
//   a WAV at the YM2612's own rate (MCLK / 1008, about 53267 Hz NTSC), stereo, mixed as
//   MAME mixes them (the YM2612 at 1/2, the PSG at 1/4);
//   a VGM (1.50) log of every YM2612 and PSG write at its time, the oracle any later
//   synthesizer (the Jaguar's DSP) is held to.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct MD;

struct SnPsg {
    uint16_t reg[8] = {0, 0xF, 0, 0xF, 0, 0xF, 0, 0xF};
    int last = 3;
    int count[4] = {}, period[4] = {}, output[4] = {};
    int volume[4] = {};
    uint32_t rng = 0x8000;
    int vol_table[16] = {};
    void init();
    void write(uint8_t v);
    void step();
    int out() const;            // negated sum, as MAME's segapsg
};

struct Sound {
    struct Impl;
    std::unique_ptr<Impl> p;
    Sound();
    ~Sound();
    bool on() const;
    // open the outputs (either path may be empty); mclk: the machine's master clock
    void open(const std::string& wav, const std::string& vgm, double mclk);
    void sync(uint64_t tick);                    // generate up to this master tick
    void ym_write(uint64_t tick, int port, uint8_t v);
    void psg_write(uint64_t tick, uint8_t v);
    void close(uint64_t tick);
    // for the survey: the YM2612's key-ons, DAC samples written, PSG writes so far
    uint64_t keyons = 0, dac_writes = 0, psg_writes = 0;
};

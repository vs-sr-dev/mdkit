// The YM2612 and the PSG made audible (see md_sound.h).
#include "md_sound.h"
#include <cmath>
#include <cstring>
#include "ymfm/ymfm_opn.h"

// ---- the PSG: MAME's sn76496 as segapsg configures it ----
// (feedback $8000, white noise taps bits 0 and 3, a step every 16 PSG clocks, period 0
// counted as is, output negated)

void SnPsg::init() {
    for (int i = 0; i < 4; i++) { reg[i * 2] = 0; reg[i * 2 + 1] = 0xF; count[i] = 0; output[i] = 0; period[i] = 0; }
    last = 3;
    rng = 0x8000;
    output[3] = rng & 1;
    double o = 0x7FFF / 4.0;
    for (int i = 0; i < 15; i++) { vol_table[i] = int(o > 0x7FFF / 4 ? 0x7FFF / 4 : o); o /= 1.258925412; }
    vol_table[15] = 0;
    for (int i = 0; i < 4; i++) volume[i] = vol_table[reg[i * 2 + 1]];
}

void SnPsg::write(uint8_t data) {
    int r;
    if (data & 0x80) {
        r = (data & 0x70) >> 4;
        last = r;
        reg[r] = uint16_t((reg[r] & 0x3F0) | (data & 0x0F));
    } else r = last;
    const int c = r >> 1;
    switch (r) {
    case 0: case 2: case 4:
        if (!(data & 0x80)) reg[r] = uint16_t((reg[r] & 0x0F) | ((data & 0x3F) << 4));
        period[c] = reg[r];
        if (r == 4 && (reg[6] & 3) == 3) period[3] = period[2] << 1;
        break;
    case 1: case 3: case 5: case 7:
        volume[c] = vol_table[data & 0x0F];
        if (!(data & 0x80)) reg[r] = uint16_t((reg[r] & 0x3F0) | (data & 0x0F));
        break;
    case 6: {
        if (!(data & 0x80)) reg[r] = uint16_t((reg[r] & 0x3F0) | (data & 0x0F));
        const int n = reg[6];
        period[3] = (n & 3) == 3 ? period[2] << 1 : 1 << (5 + (n & 3));
        rng = 0x8000;
        break;
    }
    }
}

void SnPsg::step() {
    for (int i = 0; i < 3; i++)
        if (--count[i] <= 0) { output[i] ^= 1; count[i] = period[i]; }
    if (--count[3] <= 0) {
        const bool tap = ((rng & 1) != 0) != (((rng & 8) != 0) && (reg[6] & 4));
        rng >>= 1;
        if (tap) rng |= 0x8000;
        output[3] = rng & 1;
        count[3] = period[3];
    }
}

int SnPsg::out() const {
    int o = 0;
    for (int i = 0; i < 4; i++) if (output[i]) o += volume[i];
    return -o;
}

// ---- the mixer and the files ----

namespace {
struct Intf : ymfm::ymfm_interface {};

enum { YM_TICKS = 1008, PSG_TICKS = 240, VGM_RATE = 44100 };

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (8 * i)); }
}

struct Sound::Impl {
    Intf intf;
    ymfm::ym2612 ym{intf};
    SnPsg psg;
    double mclk = 53693175.0;
    bool wav_on = false, vgm_on = false;
    std::string wav_path, vgm_path;
    uint64_t t = 0;             // master ticks generated to
    uint64_t psg_t = 0;         // the PSG's next step
    int64_t psg_acc = 0; int psg_n = 0;
    std::vector<int16_t> pcm;   // stereo frames
    std::vector<uint8_t> vgm;
    uint64_t vgm_samples = 0;   // 44100 Hz samples written as waits
    uint8_t ym_addr[2] = {};

    void run_to(uint64_t tick) {
        // YM samples end at multiples of YM_TICKS; the PSG steps every PSG_TICKS between
        while ((t / YM_TICKS + 1) * YM_TICKS <= tick) {
            const uint64_t end = (t / YM_TICKS + 1) * YM_TICKS;
            while (psg_t < end) { psg.step(); psg_acc += psg.out(); psg_n++; psg_t += PSG_TICKS; }
            ymfm::ym2612::output_data o;
            ym.generate(&o, 1);
            const int p = psg_n ? int(psg_acc / psg_n) : psg.out();
            psg_acc = 0; psg_n = 0;
            if (wav_on) {
                auto clip = [](int v) { return int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v); };
                pcm.push_back(clip(o.data[0] / 2 + p / 4));
                pcm.push_back(clip(o.data[1] / 2 + p / 4));
            }
            t = end;
        }
    }
    void vgm_wait(uint64_t tick) {
        if (!vgm_on) return;
        const uint64_t target = uint64_t(double(tick) * double(VGM_RATE) / mclk);
        while (vgm_samples < target) {
            const uint64_t n = target - vgm_samples > 0xFFFF ? 0xFFFF : target - vgm_samples;
            vgm.push_back(0x61); vgm.push_back(uint8_t(n)); vgm.push_back(uint8_t(n >> 8));
            vgm_samples += n;
        }
    }
};

Sound::Sound() = default;
Sound::~Sound() = default;
bool Sound::on() const { return p != nullptr; }

void Sound::open(const std::string& wav, const std::string& vgm, double mclk) {
    if (wav.empty() && vgm.empty()) return;
    p = std::make_unique<Impl>();
    p->mclk = mclk;
    p->wav_path = wav; p->wav_on = !wav.empty();
    p->vgm_path = vgm; p->vgm_on = !vgm.empty();
    p->ym.reset();
    p->psg.init();
    if (p->vgm_on) p->vgm.assign(0x100, 0);
}

void Sound::sync(uint64_t tick) { if (p) p->run_to(tick); }

void Sound::ym_write(uint64_t tick, int port, uint8_t v) {
    if (!p) return;
    p->run_to(tick);
    const int part = (port >> 1) & 1;
    if (!(port & 1)) { p->ym_addr[part] = v; }
    else {
        const uint8_t a = p->ym_addr[part];
        if (part == 0 && a == 0x28 && (v & 0xF0)) keyons++;
        if (part == 0 && a == 0x2A) dac_writes++;
        if (p->vgm_on) {
            p->vgm_wait(tick);
            p->vgm.push_back(part ? 0x53 : 0x52); p->vgm.push_back(a); p->vgm.push_back(v);
        }
    }
    p->ym.write(uint32_t(port & 3), v);
}

void Sound::psg_write(uint64_t tick, uint8_t v) {
    if (!p) return;
    p->run_to(tick);
    psg_writes++;
    if (p->vgm_on) { p->vgm_wait(tick); p->vgm.push_back(0x50); p->vgm.push_back(v); }
    p->psg.write(v);
}

void Sound::close(uint64_t tick) {
    if (!p) return;
    p->run_to(tick);
    if (p->wav_on) {
        FILE* f = fopen(p->wav_path.c_str(), "wb");
        if (f) {
            const uint32_t rate = uint32_t(std::lround(p->mclk / double(YM_TICKS)));
            const uint32_t bytes = uint32_t(p->pcm.size() * 2);
            std::vector<uint8_t> h(44, 0);
            std::memcpy(&h[0], "RIFF", 4); put32(h, 4, 36 + bytes); std::memcpy(&h[8], "WAVEfmt ", 8);
            put32(h, 16, 16); h[20] = 1; h[22] = 2; put32(h, 24, rate); put32(h, 28, rate * 4); h[32] = 4; h[34] = 16;
            std::memcpy(&h[36], "data", 4); put32(h, 40, bytes);
            fwrite(h.data(), 1, h.size(), f);
            fwrite(p->pcm.data(), 2, p->pcm.size(), f);
            fclose(f);
        }
    }
    if (p->vgm_on) {
        p->vgm_wait(tick);
        p->vgm.push_back(0x66);
        std::vector<uint8_t>& b = p->vgm;
        std::memcpy(&b[0], "Vgm ", 4);
        put32(b, 0x04, uint32_t(b.size() - 4));
        put32(b, 0x08, 0x150);
        put32(b, 0x0C, uint32_t(std::lround(p->mclk / 15)));        // SN76489
        put32(b, 0x18, uint32_t(p->vgm_samples));
        put32(b, 0x24, p->mclk < 53500000 ? 50 : 60);
        b[0x28] = 0x09; b[0x29] = 0x00; b[0x2A] = 16;                // the Sega PSG's noise
        put32(b, 0x2C, uint32_t(std::lround(p->mclk / 7)));         // YM2612
        put32(b, 0x34, 0x100 - 0x34);
        FILE* f = fopen(p->vgm_path.c_str(), "wb");
        if (f) { fwrite(b.data(), 1, b.size(), f); fclose(f); }
    }
    p.reset();
}

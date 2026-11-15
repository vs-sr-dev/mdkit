// vgmplay: render a VGM log md.exe wrote back to a WAV through the same YM2612 and PSG,
// to hold the log to the sound it was taken from (tools: compare the two WAVs).
//
//   vgmplay IN.vgm OUT.wav
#include <cstdio>
#include <cstdint>
#include <vector>
#include "../runtime/md_sound.h"

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "vgmplay IN.vgm OUT.wav\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<uint8_t> b;
    for (int c; (c = fgetc(f)) != EOF;) b.push_back(uint8_t(c));
    fclose(f);
    auto le32 = [&](size_t a) { return uint32_t(b[a] | b[a + 1] << 8 | b[a + 2] << 16 | uint32_t(b[a + 3]) << 24); };
    const double mclk = double(le32(0x2C)) * 7;
    size_t p = 0x34 + le32(0x34);
    Sound s;
    s.open(argv[2], "", mclk);
    uint64_t samples = 0;
    auto tick = [&]() { return uint64_t(double(samples) * mclk / 44100.0); };
    while (p < b.size()) {
        const uint8_t op = b[p];
        if (op == 0x52 || op == 0x53) {
            const int part = op & 1;
            s.ym_write(tick(), part * 2, b[p + 1]);
            s.ym_write(tick(), part * 2 + 1, b[p + 2]);
            p += 3;
        } else if (op == 0x50) { s.psg_write(tick(), b[p + 1]); p += 2; }
        else if (op == 0x61) { samples += b[p + 1] | b[p + 2] << 8; p += 3; }
        else if (op == 0x62) { samples += 735; p++; }
        else if (op == 0x63) { samples += 882; p++; }
        else if (op >= 0x70 && op <= 0x7F) { samples += (op & 15) + 1; p++; }
        else break;     // 0x66: the end
    }
    s.close(tick());
    return 0;
}

// The VDP (315-5313) as md.h describes it: its ports, registers and DMA after MAME's
// driver, a line drawn at a time.
#include "md.h"

static inline uint16_t vram16(const VDP& v, uint32_t a) { a &= 0xFFFE; return uint16_t(v.vram[a] << 8 | v.vram[a + 1]); }


// ---- the ports ----

uint16_t MD::vdp_read(uint32_t a) {
    switch (a & 0x1C) {
    case 0x00: {                // data
        vdp.pending = false;
        uint16_t r = 0;
        switch (vdp.code & 0xF) {
        case 0x0: r = vram16(vdp, vdp.addr); break;
        case 0x4: r = vdp.vsram[((vdp.addr & 0x7E) >> 1) % 40]; break;
        case 0x8: r = vdp.cram[(vdp.addr & 0x7E) >> 1]; break;
        default: return 0;
        }
        vdp.addr = uint16_t(vdp.addr + vdp.reg[15]);
        return r;
    }
    case 0x04: sv.status_reads++; return vdp_status();
    case 0x08: case 0x0C: sv.hv_reads++; return vdp_hv();
    default: return 0xFFFF;
    }
}

void MD::vdp_write(uint32_t a, uint16_t v, bool byte) {
    switch (a & 0x1C) {
    case 0x00: vdp_data_write(v); break;
    case 0x04: vdp_ctrl_write(v); break;
    case 0x10: case 0x14:
        if (!byte || (a & 1)) { z80_sync(); psg_write(uint8_t(v)); }
        break;
    default: break;
    }
}

uint16_t MD::vdp_status() {
    const uint64_t n = now();
    const uint64_t pos = n >= line_t ? (n - line_t) % LINE_T : 0;
    vdp.pending = false;
    const bool vb = vdp.vblank || !(vdp.reg[1] & 0x40);
    const uint16_t s = 0x3400 | 0x200
        | (vdp.vint_pending ? 0x80 : 0) | (vdp.sprite_ovf ? 0x40 : 0) | (vdp.sprite_col ? 0x20 : 0)
        | (vdp.odd_frame ? 0x10 : 0) | (vb ? 8 : 0) | (pos >= HBLANK_T ? 4 : 0)
        | (pal ? 1 : 0);
    vdp.sprite_ovf = vdp.sprite_col = false;
    return s;
}

static const int vjump_ntsc224[2] = {0xEA, 0xE5}, vjump_ntsc240[2] = {0xFF, 0x100};
static const int vjump_pal224[2] = {0x102, 0x1CA}, vjump_pal240[2] = {0x10A, 0x1D2};

uint16_t MD::vdp_hv() {
    const uint64_t n = now();
    const uint64_t pos = n >= line_t ? (n - line_t) % LINE_T : 0;
    // H: 211 steps a line in H40 ($00-$B6, then $E4-$FF), 171 in H32 ($00-$93, $E9-$FF)
    int h;
    if (vdp_h40()) { const int s = int(pos * 211 / LINE_T); h = s <= 0xB6 ? s : s - 0xB7 + 0xE4; }
    else { const int s = int(pos * 171 / LINE_T); h = s <= 0x93 ? s : s - 0x94 + 0xE9; }
    // V: the line, a step on past the blanking's start, folded as the counter runs (MAME's tables)
    int l = line + int((n >= line_t ? (n - line_t) : 0) / LINE_T) + (pos >= HBLANK_T ? 1 : 0);
    l %= lines;
    const int* j = pal ? ((vdp.reg[1] & 8) ? vjump_pal240 : vjump_pal224) : ((vdp.reg[1] & 8) ? vjump_ntsc240 : vjump_ntsc224);
    int vc = l <= j[0] ? l : l - j[0] - 1 + j[1];
    return uint16_t(((vc & 0xFF) << 8) | (h & 0xFF));
}

void MD::vdp_ctrl_write(uint16_t v) {
    vdp.fill_pending = false;
    if (vdp.pending) {
        vdp.pending = false;
        vdp.code = uint16_t((vdp.cmd1 >> 14) | ((v & 0xF0) >> 2));
        vdp.addr = uint16_t((vdp.cmd1 & 0x3FFF) | ((v & 3) << 14));
        if (vdp.code & 0x20) vdp_dma();
        return;
    }
    if ((v & 0xC000) == 0x8000) {
        const int r = (v >> 8) & 0x1F;
        if (!(vdp.reg[1] & 4) && r > 10) return;      // mode 4: only registers 0-10
        vdp.reg[r] = uint8_t(v);
        if (!vdp.vblank && (vdp.reg[1] & 0x40)) sv.reg_active++;
        if (r == 0 || r == 1) irq_reg_written(r);
        vdp.code = 0;
        vdp.addr = 0;
        return;
    }
    vdp.pending = true;
    vdp.cmd1 = v;
    vdp.code = uint16_t((vdp.code & 0x3C) | (v >> 14));
    vdp.addr = uint16_t((vdp.addr & 0xC000) | (v & 0x3FFF));
}

static void sat_cache_write(VDP& v, uint16_t addr, uint16_t data, bool h40) {
    const uint16_t base = h40 ? uint16_t((v.reg[5] & 0x7E) << 9) : uint16_t((v.reg[5] & 0x7F) << 9);
    const int size = h40 ? 0x400 : 0x200;
    if (addr >= base && addr < base + size) v.sat_cache[(addr & (size - 1)) >> 1] = data;
}

static void vram_write_word(MD& m, uint16_t data) {
    VDP& v = m.vdp;
    uint16_t a = v.addr;
    if (a & 1) data = uint16_t(data << 8 | data >> 8);
    v.vram[a & 0xFFFE] = uint8_t(data >> 8);
    v.vram[(a & 0xFFFE) + 1] = uint8_t(data);
    sat_cache_write(v, uint16_t(a & 0xFFFE), data, m.vdp_h40());
}

void MD::vdp_data_write(uint16_t data) {
    vdp.pending = false;
    if (vdp.fill_pending) {
        // VRAM fill (MAME's): the first byte where the address is, then the data's high byte over the length
        vdp.fill_pending = false;
        const uint16_t a0 = vdp.addr;
        vdp.vram[a0] = uint8_t(data);
        const int len = vdp.reg[19] | (vdp.reg[20] << 8);
        for (int i = 0; i <= len; i++) {
            vdp.vram[vdp.addr ^ 1] = uint8_t(data >> 8);
            vdp.addr = uint16_t(vdp.addr + vdp.reg[15]);
        }
        vdp.reg[19] = vdp.reg[20] = 0;
        return;
    }
    switch (vdp.code & 0xF) {
    case 0x1: vram_write_word(*this, data); break;
    case 0x3: vdp.cram[(vdp.addr & 0x7E) >> 1] = data & 0xEEE; if (!vdp.vblank && (vdp.reg[1] & 0x40)) sv.cram_active++; break;
    case 0x5: if (((vdp.addr & 0x7E) >> 1) < 40) vdp.vsram[(vdp.addr & 0x7E) >> 1] = data & 0x7FF;
              if (!vdp.vblank && (vdp.reg[1] & 0x40)) sv.vsram_active++;
              break;
    default: return;
    }
    vdp.addr = uint16_t(vdp.addr + vdp.reg[15]);
}

void MD::vdp_dma() {
    if (!(vdp.reg[1] & 0x10)) return;
    const int type = vdp.reg[23] >> 6;
    uint32_t len = vdp.reg[19] | (vdp.reg[20] << 8);
    if (type <= 1) {
        // 68000 memory to VRAM, CRAM or VSRAM, at once, as MAME does it: the length in
        // bytes in 16 bits (0 is $FFFF), the source on past $FFFFFF to the RAM, the
        // 68000 paused length / 3.5 ns for VRAM (MAME's stand-in for the real timing),
        // not at all for CRAM and VSRAM; a VSRAM transfer stops past its end
        uint16_t bytes = uint16_t(len << 1);
        if (!bytes) bytes = 0xFFFF;
        const uint32_t n = bytes >> 1;
        uint32_t src = (vdp.reg[21] | (vdp.reg[22] << 8) | ((vdp.reg[23] & 0x7F) << 16)) << 1;
        const int what = vdp.code & 0xF;
        if (what != 1 && what != 3 && what != 5) return;
        for (uint32_t i = 0; i < n; i++) {
            if (what == 5 && vdp.addr >= 0x80) return;
            TAINT(taint_dma(cpu, src);)
            const uint16_t w = bus_r16(cpu, src);
            if (what == 1) vram_write_word(*this, w);
            else if (what == 3) { vdp.cram[(vdp.addr & 0x7E) >> 1] = w & 0xEEE; if (!vdp.vblank && (vdp.reg[1] & 0x40)) sv.cram_active++; }
            else if (((vdp.addr & 0x7E) >> 1) < 40) vdp.vsram[(vdp.addr & 0x7E) >> 1] = w & 0x7FF;
            vdp.addr = uint16_t(vdp.addr + vdp.reg[15]);
            src += 2;
            if (src > 0xFFFFFF) src = what == 1 ? 0xE00000 : 0xFE0000;
        }
        dma_words += n;
        vdp.reg[19] = vdp.reg[20] = 0;
        vdp.reg[21] = uint8_t(src >> 1); vdp.reg[22] = uint8_t(src >> 9); vdp.reg[23] = uint8_t(src >> 17);
        if (what == 1) {
            const double mclk = pal ? 53203424.0 : 53693175.0;
            const uint64_t ticks = uint64_t(double(bytes) * 1000.0 / 3500.0 * mclk / 1e9);
            cpu.cyc += int((ticks + CPU_T - 1) / CPU_T);
        }
    } else if (type == 2) {
        vdp.fill_pending = true;        // the data port's next word starts it
    } else {
        // VRAM copy: byte by byte, the source in registers 21-22 (MAME's: a length of 0
        // copies nothing, and the registers are left as they were)
        uint16_t src = uint16_t(vdp.reg[21] | (vdp.reg[22] << 8));
        for (uint32_t i = 0; i < len; i++) {
            vdp.vram[vdp.addr] = vdp.vram[src];
            src++;
            vdp.addr = uint16_t(vdp.addr + vdp.reg[15]);
        }
    }
}

// ---- drawing ----

static inline int pattern_pixel(const VDP& v, int tile, int row, int col) {
    const uint8_t b = v.vram[((tile << 5) + (row << 2) + (col >> 1)) & 0xFFFF];
    return (col & 1) ? (b & 15) : (b >> 4);
}

// one scroll plane's line: colour (0 = transparent) and priority per pixel
static void plane_line(const VDP& v, int which, int y, int w, uint8_t* col, uint8_t* pri) {
    static const int sizes[4] = {32, 64, 32, 128};
    const int hs = sizes[v.reg[16] & 3], vs = sizes[(v.reg[16] >> 4) & 3];
    const uint32_t base = which == 0 ? uint32_t(v.reg[2] & 0x38) << 10 : uint32_t(v.reg[4] & 7) << 13;
    const uint32_t hsb = uint32_t(v.reg[13] & 0x3F) << 10;
    const int mode = v.reg[11] & 3;
    const int hl = mode == 0 ? 0 : mode == 2 ? (y & ~7) : mode == 3 ? y : (y & 7);
    const int hscroll = vram16(v, hsb + hl * 4 + which * 2) & 0x3FF;
    const bool vcol = v.reg[11] & 4;
    for (int x = 0; x < w; x++) {
        const int vsc = vcol ? v.vsram[((x >> 4) * 2 + which) % 40] : v.vsram[which];
        const int px = (x - hscroll) & (hs * 8 - 1);
        const int py = (y + vsc) & (vs * 8 - 1);
        const uint16_t e = vram16(v, base + (((py >> 3) * hs + (px >> 3)) << 1));
        int row = py & 7, c = px & 7;
        if (e & 0x1000) row = 7 - row;
        if (e & 0x0800) c = 7 - c;
        const int p = pattern_pixel(v, e & 0x7FF, row, c);
        col[x] = p ? uint8_t(((e >> 9) & 0x30) | p) : 0;
        pri[x] = uint8_t(e >> 15);
    }
}

// the window plane where it covers the line, in place of plane A
static void window_line(const VDP& v, int y, int w, bool h40, uint8_t* col, uint8_t* pri) {
    const int wv = v.reg[18] & 0x1F, wh = v.reg[17] & 0x1F;
    const bool down = v.reg[18] & 0x80, right = v.reg[17] & 0x80;
    const bool vin = down ? y >= wv * 8 : y < wv * 8;
    int x0 = 0, x1 = 0;
    if (vin) { x0 = 0; x1 = w; }
    else if (right) { x0 = wh * 16; x1 = w; }
    else { x0 = 0; x1 = wh * 16 < w ? wh * 16 : w; }
    if (x0 >= x1) return;
    const uint32_t base = h40 ? uint32_t(v.reg[3] & 0x3C) << 10 : uint32_t(v.reg[3] & 0x3E) << 10;
    const int cells = h40 ? 64 : 32;
    for (int x = x0; x < x1; x++) {
        const uint16_t e = vram16(v, base + (((y >> 3) * cells + (x >> 3)) << 1));
        int row = y & 7, c = x & 7;
        if (e & 0x1000) row = 7 - row;
        if (e & 0x0800) c = 7 - c;
        const int p = pattern_pixel(v, e & 0x7FF, row, c);
        col[x] = p ? uint8_t(((e >> 9) & 0x30) | p) : 0;
        pri[x] = uint8_t(e >> 15);
    }
}

static int sprite_line(VDP& v, int y, int w, bool h40, uint8_t* col, uint8_t* pri) {
    const int max_spr = h40 ? 80 : 64, max_line = h40 ? 20 : 16, max_px = h40 ? 320 : 256;
    const uint32_t sat = h40 ? uint32_t(v.reg[5] & 0x7E) << 9 : uint32_t(v.reg[5] & 0x7F) << 9;
    int list[20], n = 0, idx = 0;
    for (int i = 0; i < max_spr; i++) {
        const uint16_t w0 = v.sat_cache[(idx * 4) & 0x1FF], w1 = v.sat_cache[(idx * 4 + 1) & 0x1FF];
        const int sy = (w0 & 0x1FF) - 128, vsz = ((w1 >> 8) & 3) + 1;
        if (y >= sy && y < sy + vsz * 8) {
            if (n < max_line) list[n++] = idx;
            else { v.sprite_ovf = true; n++; break; }
        }
        idx = w1 & 0x7F;
        if (!idx || idx >= max_spr) break;
    }
    int budget = max_px;
    bool masked = false, seen_x = false;
    for (int k = 0; k < n && k < max_line && budget > 0; k++) {
        const int s = list[k];
        const uint16_t w1 = v.sat_cache[(s * 4 + 1) & 0x1FF];
        const int sy = (v.sat_cache[(s * 4) & 0x1FF] & 0x1FF) - 128;
        const uint16_t w2 = vram16(v, sat + s * 8 + 4), w3 = vram16(v, sat + s * 8 + 6);
        const int hsz = ((w1 >> 10) & 3) + 1, vsz = ((w1 >> 8) & 3) + 1;
        const int rawx = w3 & 0x1FF, sx = rawx - 128;
        if (rawx == 0) { if (seen_x) masked = true; }
        else seen_x = true;
        int row = y - sy;
        if (w2 & 0x1000) row = vsz * 8 - 1 - row;
        for (int c = 0; c < hsz * 8; c++) {
            if (--budget < 0) { v.sprite_ovf = true; break; }
            if (masked) continue;
            const int x = sx + c;
            if (x < 0 || x >= w) continue;
            const int cc = (w2 & 0x0800) ? hsz * 8 - 1 - c : c;
            const int tile = (w2 & 0x7FF) + (cc >> 3) * vsz + (row >> 3);
            const int p = pattern_pixel(v, tile, row & 7, cc & 7);
            if (!p) continue;
            if (col[x]) { v.sprite_col = true; continue; }
            col[x] = uint8_t(((w2 >> 9) & 0x30) | p);
            pri[x] = uint8_t(w2 >> 15);
        }
    }
    return n;
}

// a CRAM colour (or the backdrop) at an intensity: 0 shadow, 1 normal, 2 highlight
static inline uint32_t rgb(uint16_t c, int level) {
    auto ch = [&](int v3) {
        const int v = level == 1 ? v3 * 2 : level == 0 ? v3 : 7 + v3;
        return uint32_t(v * 255 / 14);
    };
    return ch((c >> 1) & 7) << 16 | ch((c >> 5) & 7) << 8 | ch((c >> 9) & 7);
}

void MD::vdp_render_line(int y) {
    VDP& v = vdp;
    const bool h40 = vdp_h40();
    const int w = h40 ? 320 : 256;
    uint32_t* out = v.fb + y * MAX_W;
    const uint16_t bg = v.cram[v.reg[7] & 0x3F];
    if (!(v.reg[1] & 0x40)) {
        for (int x = 0; x < w; x++) out[x] = rgb(bg, 1);
        return;
    }
    uint8_t ac[MAX_W] = {}, ap[MAX_W] = {}, bc[MAX_W] = {}, bp[MAX_W] = {}, sc[MAX_W] = {}, sp[MAX_W] = {};
    plane_line(v, 1, y, w, bc, bp);
    plane_line(v, 0, y, w, ac, ap);
    window_line(v, y, w, h40, ac, ap);
    const int ns = sprite_line(v, y, w, h40, sc, sp);
    if (ns > int(sv.spr_line_max)) sv.spr_line_max = uint32_t(ns);
    if (ns > (h40 ? 20 : 16)) sv.spr_ovf_lines++;
    const bool sh = v.reg[12] & 8;
    for (int x = 0; x < w; x++) {
        int c, level = 1;
        const bool s = sc[x], a = ac[x], b = bc[x];
        int layer;              // 0 sprite, 1 A, 2 B, 3 backdrop
        if (s && sp[x]) layer = 0;
        else if (a && ap[x]) layer = 1;
        else if (b && bp[x]) layer = 2;
        else if (s) layer = 0;
        else if (a) layer = 1;
        else if (b) layer = 2;
        else layer = 3;
        if (!sh) {
            c = layer == 0 ? sc[x] : layer == 1 ? ac[x] : layer == 2 ? bc[x] : (v.reg[7] & 0x3F);
            out[x] = rgb(v.cram[c], 1);
            continue;
        }
        // shadow/highlight: the planes are in shadow unless one has its priority bit
        const bool shadow = !(ap[x] || bp[x]);
        level = shadow ? 0 : 1;
        if (layer == 0 && (sc[x] == 0x3E || sc[x] == 0x3F)) {
            // the operators: the pixel under the sprite, highlighted (14) or shadowed (15)
            const int under = (a && ap[x]) ? 1 : (b && bp[x]) ? 2 : a ? 1 : b ? 2 : 3;
            c = under == 1 ? ac[x] : under == 2 ? bc[x] : (v.reg[7] & 0x3F);
            level = sc[x] == 0x3E ? (shadow ? 1 : 2) : 0;
        } else if (layer == 0) {
            c = sc[x];
            if (sp[x] || (sc[x] & 0xF) == 14) level = 1;
        } else c = layer == 1 ? ac[x] : layer == 2 ? bc[x] : (v.reg[7] & 0x3F);
        out[x] = rgb(v.cram[c], level);
    }
}

void MD::vdp_end_frame() {
    vdp.out_w = vdp.width;
    vdp.out_h = vdp.height;
    for (int y = 0; y < vdp.out_h; y++) std::memcpy(vdp.out + y * MAX_W, vdp.fb + y * MAX_W, sizeof(uint32_t) * vdp.out_w);
    if (vdp.reg[12] & 2) vdp.odd_frame = !vdp.odd_frame;
}

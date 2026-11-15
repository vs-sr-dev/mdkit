// The cartridge's hardware past ROM and SRAM (see md_cart.h), after MAME's
// bus/megadrive/eeprom.cpp, jcart.cpp, rom.cpp and machine/i2cmem.cpp.
#include "md.h"

// ---- the I2C EEPROM (MAME's i2cmem) ----

enum { I_IDLE, I_DEVSEL, I_ADDRESSHIGH, I_ADDRESSLOW, I_DATAIN, I_READSELACK, I_DATAOUT, I_RESET };

void I2CMem::init(int size, int wpage, bool xicor_select) {
    data.assign(size_t(size), 0xFF);
    write_page = wpage;
    read_page = 0;
    xicor = xicor_select;
    scl = sdaw = 0; sdar = 1;
    state = I_IDLE; bits = shift = devsel = addresshigh = byteaddr = 0;
    page_offset = page_written = 0;
}

int I2CMem::read_sda() const {
    return state == I_IDLE ? (sdaw & 1) : (sdar & 1);
}

void I2CMem::write_sda(int v) {
    v &= 1;
    if (sdaw == v) return;
    sdaw = v;
    if (!(scl && sdar)) return;
    if (sdaw) {                 // stop: the page buffer goes to the array
        if (page_written > 0) {
            const int mask = int(data.size()) - 1;
            const int base = byteaddr & mask;
            const int root = base & ~(write_page - 1);
            for (int i = 0; i < page_written; i++)
                data[size_t(root | ((base + i) & (write_page - 1)))] = page[i];
            page_written = 0;
        }
        state = I_IDLE;
    } else {                    // start
        state = I_DEVSEL;
        bits = 0;
    }
}

void I2CMem::write_scl(int v) {
    if (scl == v) return;
    scl = v;
    const int mask = int(data.size()) - 1;
    switch (state) {
    case I_DEVSEL: case I_ADDRESSHIGH: case I_ADDRESSLOW: case I_DATAIN:
        if (bits < 8) {
            if (scl) { shift = ((shift << 1) | sdaw) & 0xFF; bits++; }
        } else if (scl) {
            bits++;
        } else if (bits == 8) {
            switch (state) {
            case I_DEVSEL:
                devsel = shift;
                if (devsel == 0 && !xicor) state = I_RESET;
                else if (!xicor && (devsel & (0xFE & ~(skip_addresshigh() ? mask >> 7 : 0))) !=
                                   (0xA0 & (0xFE & ~(skip_addresshigh() ? mask >> 7 : 0)))) state = I_IDLE;
                else if (!(devsel & 1)) {
                    if (xicor) { byteaddr = (devsel & 0xFE) >> 1; page_offset = page_written = 0; state = I_DATAIN; }
                    else state = skip_addresshigh() ? I_ADDRESSLOW : I_ADDRESSHIGH;
                } else {
                    if (xicor) byteaddr = (devsel & 0xFE) >> 1;
                    state = I_READSELACK;
                }
                break;
            case I_ADDRESSHIGH:
                addresshigh = shift;
                state = I_ADDRESSLOW;
                break;
            case I_ADDRESSLOW:
                byteaddr = shift | (skip_addresshigh() ? ((devsel & 0xFE) << 7) & mask : addresshigh << 8);
                page_offset = page_written = 0;
                state = I_DATAIN;
                break;
            case I_DATAIN:
                if (write_page > 0) {
                    page[page_offset] = uint8_t(shift);
                    if (++page_offset == write_page) page_offset = 0;
                    if (++page_written > write_page) page_written = write_page;
                } else {
                    data[size_t(byteaddr & mask)] = uint8_t(shift);
                    byteaddr++;
                }
                break;
            }
            if (state != I_IDLE) sdar = 0;          // acknowledge
        } else {
            bits = 0;
            sdar = 1;
        }
        break;
    case I_READSELACK:
        bits = 0;
        state = I_DATAOUT;
        break;
    case I_DATAOUT:
        if (bits < 8) {
            if (scl) bits++;
            else {
                if (bits == 0) {
                    shift = data[size_t(byteaddr & mask)];
                    const int rp = read_page ? read_page : 0;
                    byteaddr = rp ? ((byteaddr & ~(rp - 1)) | ((byteaddr + 1) & (rp - 1))) : byteaddr + 1;
                }
                sdar = (shift >> 7) & 1;
                shift = (shift << 1) & 0xFF;
            }
        } else if (scl) {
            if (sdaw) state = I_IDLE;               // no acknowledge from the master
            bits = 0;
        } else sdar = 1;
        break;
    case I_RESET:
        if (scl) {
            if (bits > 8) { state = I_IDLE; sdar = 1; }
            bits++;
        }
        break;
    }
}

// ---- NBA Jam's EEPROM (MAME's md_eeprom_nbajam_alt) ----

enum { N_IDLE, N_WAIT_STOP, N_DEVSEL, N_GET_WORD_ADDR, N_WRITE_DATA, N_READ_DATA };

void I2CNbaJam::init() {
    std::memset(mem, 0xFF, sizeof mem);
    cnt = rw = word_address = devsel = byte = 0;
    sda = prev_sda = scl = prev_scl = 1;
    cur = N_IDLE;
}

void I2CNbaJam::update() {
    auto idle_devsel = [&]() {
        if (scl) {
            if (prev_sda && !sda) { cnt = 0; cur = N_DEVSEL; }
            else if (!prev_sda && sda) cur = N_IDLE;
        }
    };
    const bool rise = !prev_scl && scl, fall = prev_scl && !scl;
    switch (cur) {
    case N_IDLE: idle_devsel(); break;
    case N_WAIT_STOP: if (!prev_sda && sda && scl) cur = N_IDLE; break;
    case N_DEVSEL:
        idle_devsel();
        if (rise) {
            if (cnt > 4 && cnt < 8) devsel = ((devsel << 1) | sda) & 0xFF;
            else if (cnt == 8) rw = sda;
        }
        if (fall) {
            if (cnt < 9) cnt++;
            else { cnt = 1; cur = rw ? N_READ_DATA : N_GET_WORD_ADDR; }
        }
        break;
    case N_READ_DATA:
        idle_devsel();
        if (fall) cnt = cnt < 9 ? cnt + 1 : 1;
        if (rise && cnt == 9 && sda) cur = N_WAIT_STOP;
        break;
    case N_GET_WORD_ADDR:
        idle_devsel();
        if (fall) {
            if (cnt < 9) cnt++;
            else { cnt = 1; cur = N_WRITE_DATA; byte = 0; word_address &= 0xFF; }
        }
        if (rise && cnt < 9) word_address = ((word_address << 1) | sda) & 0xFF;
        break;
    case N_WRITE_DATA:
        idle_devsel();
        if (fall) cnt = cnt < 9 ? cnt + 1 : 1;
        if (rise) {
            if (cnt < 9) byte = ((byte << 1) | sda) & 0xFF;
            else {
                mem[(devsel * 0x100 + word_address) & 0x7FF] = uint8_t(byte);
                byte = 0;
                word_address = (word_address & ~3) | ((word_address + 1) & 3);
            }
        }
        break;
    }
    prev_scl = scl;
    prev_sda = sda;
}

int I2CNbaJam::out() {
    int res = sda;
    switch (cur) {
    case N_READ_DATA:
        if (cnt < 9) {
            res = (mem[(devsel * 0x100 + word_address) & 0x7FF] >> (8 - cnt)) & 1;
            if (cnt == 8) word_address = (word_address + 1) & 0xFF;
        }
        break;
    case N_DEVSEL: case N_GET_WORD_ADDR: case N_WRITE_DATA:
        if (cnt == 9) res = 0;
        break;
    }
    return res;
}

// ---- the slots ----

bool cart_setup(CartHW& c, const std::string& slot) {
    c = CartHW();
    c.slot = slot;
    auto eeprom = [&](int size, int wpage, bool xicor) { c.kind = CART_EEPROM; c.eeprom.init(size, wpage, xicor); };
    if (slot == "rom" || slot == "rom_sram" || slot == "rom_sramsafe" || slot == "rom_fram" || slot == "rom_hardbl95") return true;
    if (slot == "rom_eeprom") { eeprom(0x80, 8, false); return true; }
    if (slot == "rom_eeprom_mode1") { eeprom(0x80, 8, true); return true; }
    if (slot == "rom_nbajam") { eeprom(0x100, 8, false); c.read_form = 1; return true; }
    if (slot == "rom_nbajamte" || slot == "rom_cslam" || slot == "rom_nflqb96") {
        if (slot == "rom_nbajamte") eeprom(0x200, 16, false);
        else if (slot == "rom_cslam") eeprom(0x2000, 64, false);
        else eeprom(0x800, 16, false);
        c.read_form = 1; c.split = true;
        return true;
    }
    if (slot == "rom_nhlpa") { eeprom(0x80, 8, false); c.scl_bit = 6; c.sda_bit = 7; c.read_form = 2; return true; }
    if (slot == "rom_blara95" || slot == "rom_blara96") {
        eeprom(0x2000, 64, false);
        c.rd_addr = c.wr_addr = 0x380000; c.scl_bit = 9; c.sda_bit = 8; c.read_form = 2;
        return true;
    }
    if (slot == "rom_codemast" || slot == "rom_mm96") {
        if (slot == "rom_codemast") eeprom(0x400, 16, false); else eeprom(0x800, 16, false);
        c.rd_addr = 0x380000; c.wr_addr = 0x300000; c.scl_bit = 9; c.sda_bit = 8; c.read_form = 2;
        c.jcart = true;
        return true;
    }
    if (slot == "rom_jcart" || slot == "rom_jcart_sampras") { c.kind = CART_JCART; c.jcart = true; return true; }
    if (slot == "rom_nbajam_alt") { c.kind = CART_NBAJAM_ALT; c.nba.init(); return true; }
    if (slot == "rom_ssf2") { c.kind = CART_SSF2; return true; }
    if (slot == "rom_realtec") { c.kind = CART_REALTEC; return true; }
    return false;
}

// the pages of $000000-$3FFFFF the cart answers in itself go through I/O
void MD::cart_map() {
    auto io_page = [&](uint32_t a) { cpu.rpage[(a >> 16) & 0xFF] = nullptr; };
    if (cart.kind == CART_EEPROM || cart.kind == CART_NBAJAM_ALT) { io_page(cart.rd_addr); io_page(cart.wr_addr); }
    if (cart.jcart) io_page(0x38FFFE);
    if (cart.kind == CART_SSF2) for (int i = 0; i < 8; i++) ssf2_bank(i, cart.bank[i]);
    if (cart.kind == CART_REALTEC) for (int p = 0; p < 0x40; p++) cpu.rpage[p] = nullptr;   // all read through the mapper
}

void MD::ssf2_bank(int slot, int bank) {
    cart.bank[slot] = uint8_t(bank);
    const uint32_t rom_pages = uint32_t(rom.size() >> 16);
    for (int p = 0; p < 8; p++)
        cpu.rpage[slot * 8 + p] = rom.data() + (uint64_t((bank * 8 + p) % rom_pages) << 16);
}

static uint8_t jcart_port(const MD& m, int n) {
    const uint16_t b = m.pad[2 + n].buttons;
    const bool th = m.cart.jcart_th;
    auto bit = [&](uint16_t mask, int pos) { return (b & mask) ? 0 : (1 << pos); };
    const uint8_t in = th ? uint8_t(bit(64, 5) | bit(32, 4) | bit(8, 3) | bit(4, 2) | bit(2, 1) | bit(1, 0))
                          : uint8_t(bit(128, 5) | bit(16, 4) | bit(2, 1) | bit(1, 0));
    return uint8_t((in & 0x3F) | (0x40 & m.cart.jcart_th) | 0x80);
}

// a read of the word at a in the cart's space: true if the cart answers it itself
bool MD::cart_r16(uint32_t a, uint16_t& v) {
    a &= 0xFFFFFE;
    if (cart.kind == CART_REALTEC) {
        // MAME's: two banks of the same size at the bottom, else the image's last 8 KB of its first 512
        const uint32_t m = uint32_t(rom_bytes ? rom_bytes : rom.size());
        uint32_t o = a < uint32_t(cart.rt_size) * 0x40000 ? a + uint32_t(cart.rt_bank) * 0x20000 : (a & 0x1FFE) + 0x7E000;
        o %= m;
        v = uint16_t(rom[o] << 8 | rom[o + 1]);
        return a < 0x400000;
    }
    if (cart.kind == CART_EEPROM && a == cart.rd_addr) {
        const uint16_t s = uint16_t(cart.eeprom.read_sda());
        v = cart.read_form == 0 ? uint16_t(s << 8 | s) : cart.read_form == 1 ? s : uint16_t((s & 1) << 7);
        return true;
    }
    if (cart.kind == CART_NBAJAM_ALT && a == 0x200000) { v = uint16_t(cart.nba.out() << 1); return true; }
    if (cart.jcart && a == 0x38FFFE) { v = uint16_t(jcart_port(*this, 0) | jcart_port(*this, 1) << 8); return true; }
    return false;
}

// a write in the cart's space, as MAME's 16-bit handlers see it (mask: the lanes written)
void MD::cart_w16(uint32_t a, uint16_t data, uint16_t mask) {
    a &= 0xFFFFFE;
    if (cart.kind == CART_EEPROM && a == cart.wr_addr) {
        I2CMem& e = cart.eeprom;
        if (cart.split) {
            if (mask & 0xFF00) e.write_scl((data >> 8) & 1);
            if (mask & 0x00FF) e.write_sda(data & 1);
        } else {
            e.write_scl((data >> cart.scl_bit) & 1);
            e.write_sda((data >> cart.sda_bit) & 1);
        }
        return;
    }
    if (cart.kind == CART_NBAJAM_ALT && a == 0x200000) {
        cart.nba.sda = data & 1;
        cart.nba.scl = (data >> 1) & 1;
        cart.nba.update();
        return;
    }
    if (cart.jcart && a == 0x38FFFE) { cart.jcart_th = uint8_t((data & 1) << 6); return; }
    if (cart.kind == CART_REALTEC) {
        if (a == 0x400000) cart.rt_bank = (cart.rt_bank & 7) | ((data >> 9) & 7) << 3;
        if (a == 0x402000) { cart.rt_bank = 0; cart.rt_size = (data >> 8) & 0x1F; }
        if (a == 0x404000) cart.rt_bank = (cart.rt_bank & 0xF8) | ((data >> 8) & 3);
    }
}

// $A130F0-$A130FF
void MD::cart_a13_w(uint32_t a, uint8_t v) {
    const int reg = int((a & 0xF) >> 1);
    if (cart.kind == CART_SSF2) {
        if (reg) ssf2_bank(reg, v & 0xF);       // bank 0 is fixed
        return;
    }
    if (reg == 0) { sram_on = v & 1; sram_ro = !sram_fram && (v & 2); }
}

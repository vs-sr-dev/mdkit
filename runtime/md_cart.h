// The cartridge's own hardware past ROM and SRAM, as MAME's cart types (the software
// list's "slot") describe it: serial EEPROMs on I2C wired to the bus in each maker's way,
// the J-Cart's two extra pad ports, and Super Street Fighter II's bank registers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// An I2C serial EEPROM as MAME's i2cmem device runs it: the 24Cxx family (a device
// select byte, one or two address bytes, page writes) and Xicor's X24C01 (the address
// in the select byte itself).
struct I2CMem {
    std::vector<uint8_t> data;
    int write_page = 8, read_page = 0;
    bool xicor = false;
    int scl = 0, sdaw = 0, sdar = 1;
    int state = 0, bits = 0, shift = 0, devsel = 0, addresshigh = 0, byteaddr = 0;
    int page_offset = 0, page_written = 0;
    uint8_t page[128] = {};

    void init(int size, int wpage, bool xicor_select);
    void write_sda(int v);
    void write_scl(int v);
    int read_sda() const;
    bool skip_addresshigh() const { return data.size() <= 0x800; }
};

// MAME's custom I2C state machine for NBA Jam (rom_nbajam_alt): the X24C02 as the game
// drives it, word address in a second byte, the device bits after a 4-bit type.
struct I2CNbaJam {
    uint8_t mem[0x800];
    int cnt = 0, rw = 0, word_address = 0, devsel = 0, byte = 0;
    int sda = 1, prev_sda = 1, scl = 1, prev_scl = 1, cur = 0;
    void init();
    void update();
    int out();
};

enum CartKind { CART_ROM, CART_EEPROM, CART_NBAJAM_ALT, CART_JCART, CART_SSF2, CART_REALTEC };

struct CartHW {
    CartKind kind = CART_ROM;
    std::string slot = "rom";
    // an EEPROM's wiring: the word it answers at, the bits of the written word that are
    // SCL and SDA, and where SDA shows in the read word
    uint32_t rd_addr = 0x200000, wr_addr = 0x200000;
    int scl_bit = 1, sda_bit = 0;
    bool split = false;         // SCL in the high byte's write, SDA in the low byte's (nbajamte, cslam, nflqb96)
    int read_form = 0;          // 0: (sda << 8) | sda; 1: sda; 2: sda << 7
    bool jcart = false;         // the J-Cart's ports at $38FFFE
    uint8_t jcart_th = 0x40;
    I2CMem eeprom;
    I2CNbaJam nba;
    uint8_t bank[8] = {0, 1, 2, 3, 4, 5, 6, 7};    // SSF2: the 512 KB banks of $000000-$3FFFFF
    int rt_bank = 0, rt_size = 0;                  // Realtec: the bank (128 KB units) and the window's size
};

// the kind and wiring for a MAME slot name; false for a slot the runtime does not cover
bool cart_setup(CartHW& c, const std::string& slot);

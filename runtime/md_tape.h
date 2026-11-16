// Scripted sessions for the machine (md.exe --tape FILE), as a piece of the runtime another
// harness can drive a machine with: load a tape, then call frame() before each frame and hold
// pad 1 so.  Every write the tape makes to the work RAM (poke, pokeat, keep) goes to the
// machine and to on_write, so that a second machine -- the lockstep's interpreter, or one
// whose RAM sits elsewhere -- can be given the same.
//
// --tape FILE: a scripted session, a command a line ('#' starts a comment):
//   hold BUTTONS N         pad 1 held so N frames (BUTTONS: up+a+start..., or none)
//   wait N                 N frames, nothing held
//   tap BUTTONS [N]        held N frames (3 if not given), then released as long
//   until ADDR SIZE OP VALUE MAX [BUTTONS]
//                          frames (BUTTONS held) until the work RAM's b/w/l at ADDR is OP
//                          (== != < >= &, & meaning any bit of VALUE set) VALUE; at most MAX
//   poke ADDR SIZE VALUE   write the work RAM now; keep ADDR SIZE VALUE: at every frame from
//                          now on; unkeep ADDR
//   pokeat BASE STRIDE COUNT KEY BYTE OFFSET SIZE VALUE
//                          in a table of COUNT entries of STRIDE bytes from BASE, the entries
//                          whose byte at KEY is BYTE get VALUE at OFFSET (an object's field)
//   monkey SEED N          the random hand for N frames
//   snap NAME              the picture so far, as OUT/NAME.ppm; echo TEXT: the frame and TEXT
// ADDR and VALUE are hex, N, MAX and SEED decimal.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct MD;

struct TapeOp { std::vector<std::string> w; int line; };

struct Tape {
    std::vector<TapeOp> ops;
    size_t i = 0;
    long left = -1;
    int timeouts = 0;                                   // until commands that ran out of frames
    struct Keep { uint32_t addr; int size; uint32_t value; };
    std::vector<Keep> keeps;
    std::string out = ".";                              // where snap writes
    std::function<void(uint32_t addr, int size, uint32_t value)> on_write;   // each write to the work RAM

    bool load(const std::string& path);
    // the tape's commands for frame f, done on m; the pad to hold; false when the tape is over
    bool frame(MD& m, int f, uint16_t& pad);
};

// the random hand (--monkey SEED): a new hand every 6 frames, Start now and then
uint16_t md_monkey(uint64_t frame, uint32_t seed);
// the picture as a binary PPM
void md_write_ppm(const std::string& path, const MD& m);

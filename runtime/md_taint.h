// Where the 68000's pointers come from (built with -DMD_TAINT: build.sh makes build/md_taint.exe).
//
// Every word and long the CPU reads from the ROM -- data, or the extension words of the
// instruction itself (immediates, absolute addresses, displacements), or MOVEQ's byte --
// is an origin: (its ROM address, its size, the PC that read it).  A tag rides with each
// value as it moves: one per register, one per word of work RAM.  What an instruction
// writes takes the tag of the input it is made from: the input equal to it (a move), equal
// in its low word (a word move, sign-extended or not), or else the nearest within 32 KB in
// all 32 bits (a pointer plus an offset); a register that stays a work RAM address in the
// same 64 KB keeps its own (adda.l d0,a0; moveq #-1,d3 / move.b (a4,d0.w),d3), and so does
// the stack pointer.  The inputs are the instruction's reads and extension words and the
// registers its opcode names.  An origin is marked when its value
//   - lands in an address register (TO_AN),
//   - is the base (or the index) of an address in work RAM, $E00000-$FFFFFF (RAM_BASE),
//   - is compared (CMPA, CMP.L, CMPI.L) with another work RAM address (RAM_CMP),
//   - is the base of an address anywhere else: ROM, I/O (OTHER_BASE).
// A work RAM address formed from a register no origin reaches is counted by its PC.
//
// The rule is a heuristic on values, not the instructions' dataflow: good for pointers and
// pointers plus offsets, which is what a relocation needs to prove.
#pragma once
#include <cstdint>

struct M68K;

enum : uint32_t { TAINT_TO_AN = 1, TAINT_RAM_BASE = 2, TAINT_RAM_CMP = 4, TAINT_OTHER_BASE = 8 };

#ifdef MD_TAINT
void taint_read(M68K& c, uint32_t a, int size, uint32_t v);     // a data read (word, long)
void taint_write(M68K& c, uint32_t a, int size, uint32_t v);    // a data write (word, long)
void taint_fetch(M68K& c, uint32_t a, uint16_t v);              // a word of the instruction stream
void taint_base(M68K& c, int reg, uint32_t value);              // register 0-15 (8+: An) formed an address
#define TAINT(x) x
#else
#define TAINT(x)
#endif

void taint_attach(M68K* c, uint32_t rom_size);  // start following this CPU
void taint_begin(M68K& c);                      // around each step
void taint_end(M68K& c);
bool taint_save(const char* path);              // the marked origins and the unattributed uses

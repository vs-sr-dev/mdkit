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
// pointers plus offsets, which is what a relocation needs to prove.  (Byte accesses take no
// part in it: they are seen only by the probes below.)
//
// The same build probes what a port that moves the stack, the header or the code has to keep:
//   - each read of the header's first long, $100-$103 (by the CPU, any size, or by DMA);
//   - each read of the work RAM in the 1024 bytes under the stack pointer -- the CPU's
//     against A7 before the instruction (a pop reads at it, not under it), the DMA's against
//     A7 then -- with the shallowest and deepest distance seen; and each read at or above A7
//     of a byte that has been under A7 since it was last written (a stale local): written in
//     those 1024 bytes under it (against A7 after the instruction, so a push is not), or left
//     under it by a return (A7 back up by up to 1024 bytes and not loaded: RTS, UNLK, RTE, a
//     pop).  The reads of CLR, Scc and MOVE from SR, which the 68000 makes and drops, are not
//     reads here;
//   - each instruction that loads A7 itself (MOVEA, LEA not off A7, EXG, MOVEM naming it):
//     the stack switches, with the values A7 took;
//   - code in the work RAM: each byte written there keeps the ROM byte it copies (a read of
//     the same size and value in the same instruction) and the PC that wrote it; each
//     instruction run from the work RAM, its encodings (opcode and extension words: more than
//     one is code rewritten) and where its bytes came from;
//   - an address register's work RAM address leaving the address registers: to a data
//     register (MOVE.L An,Dn, EXG Dx,Ay), to memory (a long equal to it, anywhere; MOVEM's
//     slots by their place in its list), compared (CMPA's destination, CMPA's and CMP.L's
//     source when it is An itself: not an An that only addresses an operand), or added to a
//     data register (ADD.L/SUB.L An,Dn).  The value that left is followed while it moves
//     whole -- a data register, a long of the work RAM -- to where it comes back into an
//     address register, or is an index of a work RAM address;
//   - each instruction that writes A0-A6, known by its encoding rather than by the value
//     (a MOVEM that puts back the value a register already has writes it): MOVEA, LEA, EXG,
//     MOVEM, UNLK, LINK, MOVE USP, and ADDA/SUBA when the value is not the register's own
//     plus an offset.  A register's value is credited to the instruction that last wrote it,
//     and to the sites of the register a copy (MOVEA An, LEA off another An, EXG) or an ADDA
//     reads; each work RAM address the register forms is a use of each of them.  Offsets
//     and increments of a register into itself (LEA d(An),An, ADDQ, (An)+) are not writes.
//     For the Jaguar side's swap (bits 16-23 $FF<->$1F, $FE<->$1E, any top byte) each site
//     also counts the writes whose value the swap changes, its value's exits (as above) when
//     the swap changes it -- a work RAM address or not --, and the times its value was an
//     operand of ADDA.L/SUBA.L/CMPA.L An,Am, by whether the other was a work RAM address;
//   - the targets of JMP and JSR through a register or (d8,PC,Xn), with the times each.
#pragma once
#include <cstdint>

struct M68K;

enum : uint32_t { TAINT_TO_AN = 1, TAINT_RAM_BASE = 2, TAINT_RAM_CMP = 4, TAINT_OTHER_BASE = 8 };

#ifdef MD_TAINT
void taint_read(M68K& c, uint32_t a, int size, uint32_t v);     // a data read (byte, word, long)
void taint_write(M68K& c, uint32_t a, int size, uint32_t v);    // a data write (byte, word, long)
void taint_dma(M68K& c, uint32_t a);                            // a word the VDP's DMA reads from the 68000's side
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

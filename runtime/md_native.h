// The contract between the recompiled code and the runtime (see md.h).
//
// The generated C++ is a set of functions, each covering a range of ROM addresses; a
// function starts at any instruction of its range (a switch over them) and runs until
// the machine needs the runtime: an event is due (t >= next), an interrupt is to be
// taken, the CPU stops or traces, or control leaves the instructions it holds.  It then
// returns with the PC at the next instruction to run, and the runtime (MD::run_frame_native)
// handles the event or runs that one instruction in the interpreter.
//
// Every instruction does what m68k_step does for it: the boundary's poll, the opcode's
// fetch charged (4 cycles), the instruction's handler (m68k_op, the interpreter's own,
// so the accesses and their charges are the same), the jump to an odd address faulting,
// and its cycles added to the clock -- so that the two run the same machine, tick for
// tick, and the lockstep holds them to it.
#pragma once
#include "md.h"

typedef void (*NativeFn)(MD&, uint32_t);
struct NativeRange { uint32_t lo, hi; NativeFn fn; };
extern const NativeRange md_native[];
extern const int md_native_n;
extern const char* md_native_rom;   // the ROM's CRC32 it was built from, as 8 hex digits

// the boundary: anything that is not "run the instruction at A" goes back to the runtime
inline bool md_leave(const MD& m) {
    const M68K& c = m.cpu;
    return m.t >= m.next || c.ipl > ((c.sr >> 8) & 7) || (c.ipl == 7 && c.nmi_pending) ||
           c.stopped || c.halted || (c.sr & SR_T) || c.trace_pending;
}

#define MD_POLL(A) if (md_leave(m)) { c.pc = (A); return; } if (m.steplog) m.steplog->push_back({(A), m.t});
// one instruction at A of opcode OP, through the interpreter's handler
#define MD_OP(A, OP) do { c.ipc = (A); c.ir = (OP); c.pc = (A) + 2; c.cyc = 4; m68k_op[OP](c); \
    if (c.pc & 1) address_error(c, c.pc, true, true, prog_fc(c)); \
    m.t += uint64_t(c.cyc) * CPU_T; c.cyc = 0; m.n_native++; } while (0)
// the end of a function: an address error taken as the interpreter takes it
#define MD_FAULT catch (const AddressError& err) { m68k_fault(c, err); m.t += uint64_t(c.cyc) * CPU_T; c.cyc = 0; }

# mdkit

Sega Mega Drive / Genesis (1988-1998) reverse engineering and, in time,
static recompilation of the whole library: the cartridge and what MAME's
software list says of it, the 68000 (as MAME's microcoded core runs it),
and a runtime that replaces the console -- the VDP, the Z80 side, the
YM2612's timers, the pads -- under the game's code.

The sibling of [gbkit](https://github.com/vs-sr-dev/gbkit),
[wskit](https://github.com/vs-sr-dev/wskit),
[tg16kit](https://github.com/vs-sr-dev/tg16kit),
[7800kit](https://github.com/vs-sr-dev/7800kit) and
[watarakit](https://github.com/vs-sr-dev/watarakit). It is also meant as
the front end of a future Mega Drive to Atari Jaguar translator: the same
68000, and little else in common.

Status: all 1349 cartridges of the library (below) recompiled to C++,
built, and identical to the interpreter in lockstep for 6000 frames (the
whole machine compared at every frame); 97.5% of the instructions the
library runs are native (median per cart 99.98%; what is left is code in
RAM). The interpreter is held to MAME (a build with only the Mega Drive
drivers) frame by frame: with no input, 1260 of the 1349 pictures are
identical to MAME's at frame 300, 1190 at frame 1500.

## Tools

```sh
python -m mdkit.cart "mdkit/Mega Drive/*/*" --summary         # the library: tiers, machines, sizes, carts
python -m mdkit.cart --collect SETDIR "mdkit/Mega Drive"      # the listed dumps of a set, one each
python -m mdkit.m68k ROM.bin --entry -n 40                    # a listing from the reset vector
python tools/m68kcheck.py --unidasm PATH/unidasm.exe          # the decoder against MAME's, all 65536 opcodes
python tools/boot.py "Mega Drive/core" --out build/boot       # every cart in the interpreter, mosaics
```

Building (msys2 ucrt64 g++):

```sh
./build.sh                                                    # build/md.exe and the runtime's objects (build/obj)
g++ -std=c++20 -O2 -o build/m68ktest.exe tests/m68ktest.cpp runtime/m68k.cpp
build/m68ktest.exe PATH/SingleStepTests/m68000/v1             # the 68000 against SingleStepTests
build/md.exe ROM.bin --machine NTSC-U --frames 600 --snap 300 --out DIR --monkey 1
build/md.exe ROM.bin --frames 1800 --wav out.wav --vgm out.vgm   # the sound, and the log of every YM2612/PSG write
build/md.exe ROM.bin --press "700+6:start;1000+60:right"       # a scripted pad instead of the monkey
build/md.exe ROM.bin --pcs F | --trace F | --iolog F | --ram F  # one frame's PCs, instructions, I/O, memory
```

## The recompiler

```sh
build/md.exe ROM.bin --frames 6000 --monkey 1 --cover trace.cov            # the tracer: every instruction run
python -m mdkit.recomp ROM.bin --cover trace.cov --out build/recomp/GAME --build
build/recomp/GAME/md.exe ROM.bin --frames 6000 --monkey 2 --lockstep      # native and interpreter, every frame
python -m mdkit.recomp.all "Mega Drive/core" --jobs 4                     # every cart: trace, recompile, build, lockstep
python tools/disasm_oracle.py ROM.bin PATH/s1disasm --cover trace.cov    # discovery against a disassembly's data
python tools/mamecmp.py "Mega Drive/core" --frames 300,1500,3000         # the interpreter against MAME, same pad
python tools/survey.py "Mega Drive/core" --exe build/md.exe              # the library: VDP, raster, sound, code columns
```

- `recomp/discover.py` -- code from the tracer's coverage and a walk on
  from it (fall-through, branch, BSR, DBcc, JMP/JSR to fixed addresses).
  A walk into data costs only C++: every instruction runs with the
  interpreter's own handler or as its exact C++ copy, so a word wrongly
  called code would, if run, do what the interpreter does. Against
  s1disasm (whose binary data files cover 65% of Sonic 1's ROM) no
  instruction discovered starts in data.
- `recomp/emit.py` -- the ROM's instructions as C++ functions over address
  ranges (`runtime/md_native.h`): a switch to any instruction, at each the
  boundary's poll (events due, interrupts, STOP, trace), the instruction,
  its cycles; Bcc, BRA, BSR, DBcc and MOVEQ written out in C++ with their
  charges, the others through the interpreter's handler; a branch within a
  function a `goto`, elsewhere a return to the runtime.
- Code in RAM, SSF2's banked ROM past its first 512 KB and Realtec's
  mapped ROM stay interpreted for now.

## Discovery as JSON (for the Jaguar side)

```sh
python -m mdkit.recomp.export ROM.bin --cover trace.cov --out GAME.json
```

`recomp/export.py` writes what discovery and the tracer know of a cart:
its code (the instructions, how each was found, the runs), the **access
sites** -- every instruction the trace saw reach the hardware, with the
addressing mode of each operand and the classes it read and wrote (VDP
data, control, HV counter, PSG, Z80 RAM, YM2612, the Z80's bus, pads,
mapper, TMSS) -- the absolute operands by the area they name (work RAM,
VDP, Z80 side, I/O, ROM), the jumps through registers and PC-indexed
tables, and the special instructions (TAS, MOVEP, STOP, TRAP, line A/F).
On the Jaguar the RAM sits at `$000000` and the ROM at `$800000`, and
there is no VDP or YM2612: these are the places a translation has to
rewrite. Sonic 1, for one, reaches the VDP mostly through an address
register loaded once (`(An)`), and names the work RAM with `abs.w` 1055
times.

## The library

`cart.py` recognises a dump by CRC32 against MAME's software list
(`hash/megadriv.xml`, CC0), also where a set's dump is not the list's
image: shorter than the chip MAME lists (the rest reads `$FF`), an
overdump, in the SMD format, or a cartridge MAME lists chip by chip. It
writes each recognised dump as the list's image, one per software, in
tiers:

- `core` -- the period's commercial releases (to 1998): the library;
- `proto` -- prototypes, demos, Sega Channel and Game Toshokan, counted apart;
- `unl` -- the period's unlicensed and pirate carts, counted apart;
- `out` -- homebrew and later releases, cheat devices, hacks and translations: not counted.

The set this was written with (from the Internet Archive, not
distributed) holds 1349 of the list's 1474 core software, 901 of its 921
families; the 20 families missing are obscure (two Codemasters 2-in-1s,
Tec Toy's Brazilian releases, Realtec's, Wonder MIDI...).

## The machine

`runtime/`: the 68000 (`m68k.*`), the Z80 (`z80.*`), the bus, I/O, pads
and line events (`md_core.cpp`), the VDP drawn a line at a time
(`md_vdp.cpp`), the cartridges' hardware as MAME's slots describe it
(`md_cart.cpp`: SRAM and FRAM, the I2C EEPROMs in each board's wiring, NBA
Jam's, the J-Cart's ports, Super Street Fighter II's banks, Realtec's
mapper), the sound (`md_sound.cpp`: the YM2612 through ymfm, the PSG, a
WAV and a VGM log of every write at its time), the recompiled code's
contract (`md_native.h`) and the executable (`md_main.cpp`).

## The survey (from the library)

`tools/survey.py` runs every cart 6000 frames with the monkey's hand and
counts what it does with the machine (modes held over 60 frames):

- VDP: per-line horizontal scroll 484 carts, per-cell 242, 2-cell column
  vertical scroll 80; shadow/highlight 188; window 595; H32 throughout 160;
  V30 23; interlace 2; HINT 326; CRAM rewritten mid-picture 86, VSRAM 151;
  sprites over a line's limit 124; the most DMA a frame, median 8 KB.
- Sound: the YM2612 written by the Z80 alone in 1099 carts, by the 68000
  alone in 28, by both in 216; the DAC in 926; timer A 676, timer B 525,
  channel 3's special mode 220, CSM 4; the PSG 1338. The Z80's RAM when the
  68000 lets it run: 818 distinct images, the largest group 137 carts
  (Aladdin, Aero the Acro-Bat, Real Monsters...: GEMS by the look of it).
- Code: the work RAM named by `abs.l` above all (median 1403 operands a
  cart), `abs.w` in 777 carts (Sonic 1: 1055); the hardware reached
  through an address register (`(An)`, `(An)+`, `d16(An)`: 274,000 operands
  of the access sites) about as often as by `abs.l` (155,000); TRAP in 204
  carts, MOVEP 156, STOP 127, TAS 14.

## Not covered (yet)

- SVP (Virtua Racing); the Sonic & Knuckles lock-on; interlace mode 2;
  the VDP's FIFO and access slots; light guns, mouse, multitap; the
  YM2612's CSM mode in the sound (its timers run for the game, not for
  ymfm); the protections and mappers of the unlicensed carts.

## Licence

MIT, see [LICENSE](LICENSE); [THIRD_PARTY.md](THIRD_PARTY.md) lists the
sources it was written from.

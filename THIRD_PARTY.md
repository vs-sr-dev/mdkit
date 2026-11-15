# Sources

mdkit is MIT-licensed (`LICENSE`). It contains one piece of third-party
data: MAME's software list for the Mega Drive / Genesis cartridges
(`hash/megadriv.xml`, CC0-1.0), which `cart.py` uses to name each dump and
to know its board (SRAM, EEPROM, mappers), its region and whether MAME
runs it.

The hardware the runtime models follows MAME's drivers and devices, read
as documentation (BSD-3-Clause): `src/mame/sega/megadriv.cpp`,
`mdconsole.cpp` (the memory map, the I/O, the Z80's bus),
`src/devices/video/315_5313.cpp` (the VDP: its ports, registers, DMA,
status, the V counter's tables), `src/devices/bus/megadrive/` (the
cartridges). The code is written independently: the VDP's timing in
particular (a line of 3420 master-clock ticks, drawn at the start of its
active display, HINT and the HBlank flag at tick 2640) is mdkit's own, made
deterministic for the recompiler's lockstep.

- `m68k.py`'s instruction forms (the name, mask, match and addressing modes
  of each) are those of MAME's 68000 disassembler (`m68kdasm.cpp`, Karl
  Stenerud's Musashi), restricted to the 68000; `tools/m68kcheck.py` holds
  the decoder to `unidasm`.
- The 68000 interpreter's semantics come from cdiPC's core
  (same author, MIT); its cycles are
  the ones MAME's microcoded 68000 takes, as SingleStepTests/m68000 records
  them (tests generated with that core; used locally by
  `tests/m68ktest.cpp`, not distributed). The division's cycles follow
  Jorge Cwik's analysis of the 68000's microcode.
- The Z80 (`runtime/z80.*`) is Pocketshift's (same author, MIT), which
  passes zexdoc and zexall.
- The DMAs are MAME's: at once, the 68000 paused length / 3.5 ns for a
  transfer to VRAM and not at all to CRAM or VSRAM, no DMA ever reported
  in progress. Real hardware is slower (Charles MacDonald's figures: 18
  words a line in H40's active display, 205 in blanking); held to MAME,
  the library's pictures agree with MAME's frame for frame
  (`tools/mamecmp.py`), which they did not with the real rates.
- `runtime/ymfm/` is Aaron Giles's ymfm (BSD-3-Clause, its `LICENSE`
  alongside), copied unchanged from MAME's `3rdparty/ymfm`: the YM2612's
  sound. The PSG (`md_sound.cpp`) follows MAME's `sn76496.cpp` as its
  `segapsg` configures it; the mix is MAME's (YM2612 1/2, PSG 1/4).
- The cartridges' hardware (`md_cart.cpp`): the I2C EEPROM follows MAME's
  `machine/i2cmem.cpp` (the 24Cxx family and Xicor's X24C01), and its
  wiring per board `bus/megadrive/eeprom.cpp` (including MAME's own state
  machine for NBA Jam) and `jcart.cpp`; the SSF2 banks `rom.cpp`.
- The interrupts' timing is MAME's: a line raised 128 ticks after its
  interrupt is pending, 64 after an enable bit set over a pending one,
  dropped at once; the 68000 samples it at the start of an instruction's
  last prefetch (MAME's microcode), here 4 cycles before its end.

Where mdkit departs from MAME on purpose:

- MAME returns random bits around the Z80 bus status bit (`$A11100`);
  mdkit returns the 68000's prefetched word there, as the bus does
  (Double Dragon, Shadow of the Beast, Super Off Road and Time Killers
  wait on those bits), so that a run is deterministic.
- The HINT counter steps, and HINT is raised, at the end of each line, for
  the line V enters: the phase of MAME's scanline callback, where VINT comes
  too; a HINT handler reads the line its interrupt was raised for (Lemmings 2
  relies on it).

Local references, not distributed: `build/mame/mdref.exe` (a MAME built
with only the Mega Drive drivers, `tools/mamecmp.py`'s oracle) and the
Sonic Retro disassemblies (`tools/disasm_oracle.py` reads their binary
data files, nothing of them is run).

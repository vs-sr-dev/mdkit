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
- The DMA's rates per line are Charles MacDonald's figures, as MAME's
  VDP quotes them.

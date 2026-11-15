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

Status: work in progress. The interpreter runs every cartridge of the
library (below) 6000 frames without a crash, and nearly all of them to
their titles, menus or play; the tracer and the recompiler are next.

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
g++ -std=c++20 -O2 -o build/md.exe runtime/md_main.cpp runtime/md_core.cpp runtime/md_vdp.cpp runtime/m68k.cpp runtime/z80.cpp
g++ -std=c++20 -O2 -o build/m68ktest.exe tests/m68ktest.cpp runtime/m68k.cpp
build/m68ktest.exe PATH/SingleStepTests/m68000/v1             # the 68000 against SingleStepTests
build/md.exe ROM.bin --machine NTSC-U --frames 600 --snap 300 --out DIR --monkey 1
```

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

## Not covered (yet)

- Sound output (the YM2612's and the PSG's registers are kept, and the
  YM2612's timers run); EEPROM saves; the SSF2 mapper; SVP (Virtua
  Racing); the Sonic & Knuckles lock-on; interlace mode 2; the VDP's FIFO
  and access slots; light guns, mouse, multitap.

## Licence

MIT, see [LICENSE](LICENSE); [THIRD_PARTY.md](THIRD_PARTY.md) lists the
sources it was written from.

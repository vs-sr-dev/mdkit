"""Sonic the Hedgehog (megadriv_sonic, revision 0): the tapes (md.exe --tape) that take the
tracer where the random hand does not -- every zone and act, the bosses, the special stage,
the sound test, game over and continue, the debug mode, the ending and the credits.

Each tape starts from power on: the title, the level select code (Up, Down, Left, Right) and
then the debug code (the same with C pressed twice), A held with Start for the level select.
The RAM addresses are the game's (s1disasm's _Variables.asm); pokes stand only for what a
long play would bring (rings, lives, the camera's way to a boss), never for the code's path.

    python tools/tapes_sonic1.py OUTDIR       # OUTDIR/NAME.tape, one a session
"""
import os
import random
import sys

GAMEMODE, ZONE, LIVES, AIR, CONTINUES, RINGS, TIME = 0xFFF600, 0xFFFE10, 0xFFFE12, 0xFFFE14, 0xFFFE18, 0xFFFE20, 0xFFFE22
LEVSELITEM, DEBUGMODE, CAMX, CAMY = 0xFFFF82, 0xFFFFFA, 0xFFF700, 0xFFF704
CREDITSCHEAT = 0xFFFFE3
PLAYER_X, PLAYER_Y = 0xFFD008, 0xFFD00C
EMERALDS, LASTSPECIAL = 0xFFFE57, 0xFFFE16
TITLE, LEVEL, SPECIAL, CONTINUE = 0x04, 0x0C, 0x10, 0x14

# the level select's rows (revision 0's order)
ROWS = ['ghz1', 'ghz2', 'ghz3', 'lz1', 'lz2', 'lz3', 'mz1', 'mz2', 'mz3', 'slz1', 'slz2', 'slz3',
        'syz1', 'syz2', 'syz3', 'sbz1', 'sbz2', 'sbz3', 'fz', 'special', 'sound']
# the bosses (_Constants.asm): the act, the height to fly the debug cursor at, the camera's X to stop at
BOSSES = {'ghz3': (0x380, 0x2960, 0x3D), 'lz3': (0x640, 0x1CA0, 0x77), 'mz3': (0x280, 0x1800 - 0x10, 0x73),
          'slz3': (0x280, 0x2000, 0x7A), 'syz3': (0x540, 0x2C00, 0x75), 'sbz2': (0x4E0, 0x2050, None)}
OBJECTS, OBJECT_SIZE, OBJECT_COUNT, OB_BOSSHITS = 0xFFD000, 0x40, 0x80, 0x21


def title(debug=True):
    t = [f'until {GAMEMODE:06X} b == {TITLE:02X} 1000', 'wait 200   # past "Sonic Team presents"']
    t += ['tap up', 'tap down', 'tap left', 'tap right', 'wait 4   # the level select code']
    if debug:
        t += ['tap left   # a wrong press starts the code over', 'tap c', 'tap c',
              'tap up', 'tap down', 'tap left', 'tap right', 'wait 4   # again with C twice: debug mode']
    return t


def level_select(row, hold_a=True):
    """From the title: into the level select, down to the row, and in (A held: debug mode on)."""
    t = ['hold a 4', 'hold a+start 3', 'wait 40']
    t += ['tap down'] * ROWS.index(row)
    t += ['wait 10', 'hold a+start 3' if hold_a else 'hold start 3']
    if hold_a:
        t += ['hold a 200   # held while the level loads: debug mode']
    return t


def keep_alive():
    return [f'keep {RINGS:06X} w 0032   # 50 rings: hits cost rings, not lives',
            f'keep {LIVES:06X} b 03', f'keep {AIR:06X} w 001E   # air under water']


def runner(frames, seed, right=True):
    """Running on with jumps and some backing off."""
    rnd = random.Random(seed)
    t, n = [], 0
    fwd, back = ('right', 'left') if right else ('left', 'right')
    while n < frames:
        k = rnd.random()
        if k < 0.55:
            d = rnd.randint(20, 70); t.append(f'hold {fwd} {d}')
        elif k < 0.8:
            d = rnd.randint(8, 20); t.append(f'hold {fwd}+a {d}')
        elif k < 0.9:
            d = rnd.randint(10, 30); t.append(f'hold {back} {d}')
        elif k < 0.95:
            d = rnd.randint(6, 16); t.append(f'hold {back}+a {d}')
        else:
            d = rnd.randint(10, 40); t.append(f'hold down {d}   # a spin dash of sorts'); t.append('hold down+a 4')
        n += d
    return t


def explore(frames, seed):
    """Across the act: a stretch run, then a hop on with the debug cursor (B, right, B)."""
    t, n, k = [], 0, 0
    while n < frames:
        t += runner(300, seed * 100 + k) + ['tap b', 'hold right 100', 'tap b']
        n += 420
        k += 1
    return t


def fighter(frames, seed, boss=None):
    """A boss: jump after jump, now this way, now that.  Its hits are brought down to one now and
    then (the long fight a player would have), so that the first one lands its defeat."""
    rnd = random.Random(seed)
    t, n, last = [], 0, 0
    while n < frames:
        if boss is not None and n - last > 400:
            t.append(f'pokeat {OBJECTS:06X} {OBJECT_SIZE:X} {OBJECT_COUNT:X} 0 {boss:02X} {OB_BOSSHITS:X} b 01   # one hit left')
            last = n
        side = rnd.choice(['left', 'right', 'none'])
        for _ in range(rnd.randint(1, 4)):
            pre = rnd.randint(2, 10)
            t += [f'hold {side} {pre}', f'hold {side}+a {rnd.randint(14, 30)}'.replace('none+a', 'a')]
            n += pre + 22
    return t


def debug_objects(n=24):
    """Debug mode: B to the cursor, every item placed (A the next, C to place), B back."""
    t = ['tap b   # debug mode: the cursor']
    for _ in range(n):
        t += ['tap a', 'tap c', 'hold right 8']
    t += ['tap b', 'wait 60']
    return t


def fly_to(y, camx, limit=1500):
    """The debug cursor across the level at height y until the camera reaches camx."""
    return ['tap b', f'keep {PLAYER_Y:06X} w {y:04X}', f'until {CAMX:06X} w >= {camx:04X} {limit} right',
            f'unkeep {PLAYER_Y:06X}', 'wait 30', 'tap b   # Sonic again', 'wait 30']


def tapes():
    out = {}
    for i, row in enumerate(ROWS[:19]):
        t = title() + level_select(row) + [f'until {GAMEMODE:06X} b == {LEVEL:02X} 600'] + keep_alive()
        t += [f'echo {row}', 'wait 60'] + debug_objects() + explore(5000, i) + runner(900, 100 + i)
        out['act_' + row] = t
    for row, (y, camx, boss) in BOSSES.items():
        t = title() + level_select(row) + [f'until {GAMEMODE:06X} b == {LEVEL:02X} 600'] + keep_alive()
        t += ['wait 60'] + fly_to(y, camx) + [f'echo boss {row}'] + fighter(5000, 200 + ROWS.index(row), boss)
        t += runner(1500, 300)    # on past the boss: the capsule, the signpost, the next act
        out['boss_' + row] = t
    # the special stage, from the level select (no rings kept: it ends on its own or by the exit)
    t = title() + level_select('special', hold_a=False) + [f'until {GAMEMODE:06X} b == {SPECIAL:02X} 600', 'echo special']
    rnd = random.Random(7)
    for _ in range(300):
        t.append(f"hold {rnd.choice(['left', 'right', 'left+a', 'right+a', 'a', 'none'])} {rnd.randint(5, 30)}")
    out['special'] = t
    # every special stage: the stage number poked before the level select takes it
    for s in range(6):
        t = title(debug=False) + [f'poke {LASTSPECIAL:06X} b {s:02X}'] + level_select('special', hold_a=False)
        t += [f'until {GAMEMODE:06X} b == {SPECIAL:02X} 600', f'echo special {s + 1}']
        rnd = random.Random(10 + s)
        for _ in range(250):
            t.append(f"hold {rnd.choice(['left', 'right', 'left+a', 'right+a', 'a', 'none'])} {rnd.randint(5, 30)}")
        out[f'special_{s + 1}'] = t
    # the sound test: every song (5 s each) and every sound effect
    t = title(debug=False) + ['hold a 4', 'hold a+start 3', 'wait 40', 'tap up   # the sound test row']
    for i in range(1, 0x50):
        t.append('tap right 2')
        if i <= 0x13:
            t += ['tap c 2', 'wait 300']
        elif i >= 0x20:
            t += ['tap c 2', 'wait 40']
    out['sound_test'] = t
    # game over and continue: one life, a continue, the clock at 9:59
    t = title(debug=False) + level_select('ghz1', hold_a=False) + [f'until {GAMEMODE:06X} b == {LEVEL:02X} 600']
    t += ['wait 60', f'poke {LIVES:06X} b 01', f'poke {CONTINUES:06X} b 01', f'poke {TIME:06X} l 00093B00   # 9:59',
          f'until {GAMEMODE:06X} b == {CONTINUE:02X} 1200', 'echo continue', 'wait 120', 'tap start',
          f'until {GAMEMODE:06X} b == {LEVEL:02X} 1200', 'echo back in', 'wait 60', f'poke {LIVES:06X} b 01',
          f'poke {TIME:06X} l 00093B00', f'until {GAMEMODE:06X} b < 08 1800', 'echo game over', 'wait 600']
    out['gameover'] = t
    # the ending (good and bad) and the credits: the Japanese credits cheat's sounds $9F and $9E
    for name, sound, emeralds in (('ending_good', 0x1F, 6), ('ending_bad', 0x1F, 0), ('credits', 0x1E, 0)):
        t = title(debug=False) + [f'poke {CREDITSCHEAT:06X} b 01', f'poke {EMERALDS:06X} b {emeralds:02X}',
                                  'hold a 4', 'hold a+start 3', 'wait 40', 'tap up']
        t += ['tap right 2'] * sound + ['tap c 2', f'echo {name}', 'wait 12000']
        out[name] = t
    return out


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    out_dir = argv[0]
    os.makedirs(out_dir, exist_ok=True)
    for name, lines in tapes().items():
        with open(os.path.join(out_dir, name + '.tape'), 'w') as f:
            f.write(f'# Sonic the Hedgehog (rev 0): {name} (tools/tapes_sonic1.py)\n')
            f.write('\n'.join(lines) + '\n')
    print(f'{len(tapes())} tapes in {out_dir}')


if __name__ == '__main__':
    sys.exit(main())

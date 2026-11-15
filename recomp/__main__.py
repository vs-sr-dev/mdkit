"""Recompile a cartridge: discovery from its traces, C++, and (with --build) an md.exe.

    python -m mdkit.recomp ROM --cover FILE [--cover FILE...] --out DIR [--build] [--comments]

The executable links the generated code with the runtime's objects (build.sh makes
them in build/obj); run it with --lockstep to hold it to the interpreter.
"""
import argparse
import concurrent.futures
import glob
import os
import subprocess
import sys
import zlib

from .discover import discover
from .emit import emit

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GXX = os.environ.get('MDKIT_GXX', r'C:\msys64\ucrt64\bin\g++.exe')
RUNTIME_OBJS = ['md_core', 'md_vdp', 'md_cart', 'md_sound', 'm68k', 'z80', 'md_main',
                'ymfm_opn', 'ymfm_adpcm', 'ymfm_ssg']


def stale_runtime():
    """The runtime sources newer than the objects the build links (build.sh makes them): the
    generated code is compiled against the headers of now, the objects against those of then."""
    objs = [os.path.join(HERE, 'build', 'obj', n + '.o') for n in RUNTIME_OBJS]
    if not all(os.path.exists(o) for o in objs):
        return ['(missing objects)']
    oldest = min(os.path.getmtime(o) for o in objs)
    src = glob.glob(os.path.join(HERE, 'runtime', '*.h')) + glob.glob(os.path.join(HERE, 'runtime', '*.cpp'))
    return [os.path.basename(p) for p in src if os.path.getmtime(p) > oldest]


def build(paths, out_dir, opt='-O1'):
    stale = stale_runtime()
    if stale:
        raise RuntimeError(f'the runtime objects are older than {stale}: run build.sh first')
    env = dict(os.environ, PATH=os.path.dirname(GXX) + os.pathsep + os.environ.get('PATH', ''))
    inc = os.path.join(HERE, 'runtime')

    def cc(p):
        o = os.path.splitext(p)[0] + '.o'
        r = subprocess.run([GXX, '-std=c++20', opt, '-I', inc, '-c', '-o', o, p], capture_output=True, text=True, env=env)
        if r.returncode:
            raise RuntimeError(r.stderr[-4000:])
        return o
    with concurrent.futures.ThreadPoolExecutor(len(paths)) as ex:
        objs = list(ex.map(cc, paths))
    objs += [os.path.join(HERE, 'build', 'obj', n + '.o') for n in RUNTIME_OBJS]
    exe = os.path.join(out_dir, 'md.exe')
    r = subprocess.run([GXX, '-o', exe] + objs, capture_output=True, text=True, env=env)
    if r.returncode:
        raise RuntimeError(r.stderr[-4000:])
    return exe


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--cover', action='append', default=[])
    ap.add_argument('--out', required=True)
    ap.add_argument('--build', action='store_true')
    ap.add_argument('--comments', action='store_true')
    ap.add_argument('--no-walk', action='store_true')
    a = ap.parse_args(argv)
    data = open(a.rom, 'rb').read()
    covers = [p for c in a.cover for p in glob.glob(c)]
    prog = discover(data, covers, not a.no_walk)
    for p in glob.glob(os.path.join(a.out, 'r*.cpp')):
        os.remove(p)
    paths = emit(prog, a.out, zlib.crc32(data), a.comments)
    print(f'{len(prog.code)} instructions in {len(paths) - 1} files')
    if a.build:
        print(build(paths, a.out))


if __name__ == '__main__':
    sys.exit(main())

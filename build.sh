#!/bin/sh
# build/md.exe (interpreter only) and the objects the recompiled builds link with;
# build/md_taint.exe: the same with the pointers' provenance (-DMD_TAINT, runtime/md_taint.h)
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/ucrt64/bin:$PATH
mkdir -p build/obj build/obj_taint
for f in md_core md_vdp md_cart md_sound m68k z80 none md_main md_tape; do
  g++ -std=c++20 -O2 -c -o build/obj/$f.o runtime/$f.cpp &
  g++ -std=c++20 -O2 -DMD_TAINT -c -o build/obj_taint/$f.o runtime/$f.cpp &
done
g++ -std=c++20 -O2 -DMD_TAINT -c -o build/obj_taint/md_taint.o runtime/md_taint.cpp &
for f in ymfm_opn ymfm_adpcm ymfm_ssg; do
  g++ -std=c++20 -O2 -c -o build/obj/$f.o runtime/ymfm/$f.cpp &
done
wait
g++ -o build/md.exe build/obj/*.o
g++ -o build/md_taint.exe build/obj_taint/*.o build/obj/ymfm_*.o

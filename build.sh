#!/bin/sh
# build/md.exe (interpreter only) and the objects the recompiled builds link with
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/ucrt64/bin:$PATH
mkdir -p build/obj
for f in md_core md_vdp md_cart md_sound m68k z80 none md_main; do
  g++ -std=c++20 -O2 -c -o build/obj/$f.o runtime/$f.cpp &
done
for f in ymfm_opn ymfm_adpcm ymfm_ssg; do
  g++ -std=c++20 -O2 -c -o build/obj/$f.o runtime/ymfm/$f.cpp &
done
wait
g++ -o build/md.exe build/obj/*.o

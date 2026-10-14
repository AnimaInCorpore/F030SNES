#!/bin/bash
# build.sh <tag> : assemble the DSP programs from falcon/dsp/tc.asm (working tree) and link 4 TCTEST packs
# usage: build.sh TAG   -> build/bench/TAG/{tc.bin,tc7.bin,tc.lst,tc7.lst,T0..T3.TOS}
set -e
cd "$(dirname "$0")/../.."
TAG=$1; D=build/bench/$TAG; mkdir -p $D
export SDL_VIDEODRIVER=dummy
xvfb-run -a python3 -m tools.smkx.dsp_asm --boot --define TC7=0 falcon/dsp/tc.asm $D/tc.bin
xvfb-run -a python3 -m tools.smkx.dsp_asm --boot --define TC7=1 falcon/dsp/tc.asm $D/tc7.bin
[ -f build/bench/t_tctestp.o ] || ${M68KCC:-m68k-atari-mintelf-gcc} -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fomit-frame-pointer -nostdlib -Isrc -c -o build/bench/t_tctestp.o build/bench/tctestp.c
for k in ${PACKS:-0 1 2 3 4 5}; do
  mkdir -p $D/p$k; cp $D/tc.bin $D/tc7.bin $D/p$k/; ln -sf "$PWD/build/bench/pack$k.bin" $D/p$k/tc_tests.bin
  ../../../F030Arcade/third_party/vasm/vasmm68k_mot -Felf -m68030 -quiet -nowarn=62 -I$D/p$k -Ifalcon -o $D/p$k/tctest.o falcon/tctest.s
  ../../../F030Arcade/third_party/vlink/vlink -b ataritos -tos-flags 1 -o $D/T$k.TOS $D/p$k/tctest.o build/bench/t_tctestp.o 2>&1 | grep -v "^Warning 22" || true
done
ls -la $D/*.TOS

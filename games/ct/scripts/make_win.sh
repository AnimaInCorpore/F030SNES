#!/bin/sh
# Build on the Windows workstation: MSYS2's cross gcc and Python,
# F030Arcade's vasm/vlink (the Makefile's defaults, .exe resolved by the
# shell) and DOSBox Staging for the DSP kernels, e.g.
#
#     scripts/make_win.sh ctx CTX_LC=0 CTX_FRAMES=12000
#
# Git for Windows' make hands its recipes none of the shell's exported
# environment (TMP, DOSBOX, PYTHONHASHSEED all arrive empty), so everything
# goes as make variables, which it does export. The mingw64 cross gcc needs
# TMP/TEMP as Windows paths (an MSYS-style TMPDIR sends it to C:\WINDOWS);
# -pipe keeps it off temporary files anyway.
PATH="/c/msys64/ucrt64/bin:/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
export PATH
exec make "$@" \
  CROSS_CC="m68k-atari-mintelf-gcc -pipe" \
  DOSBOX="${DOSBOX:-C:/Users/sasch/AppData/Local/Programs/DOSBox Staging/dosbox.exe}" \
  TMP="${TMP:-C:/Users/sasch/AppData/Local/Temp}" TEMP="${TEMP:-C:/Users/sasch/AppData/Local/Temp}" \
  PYTHONHASHSEED=0 PYTHONUTF8=1

#!/usr/bin/env bash
# Consecutive real frames from the Falcon runtime in Hatari (Windows, MSYS2 shell).
#
#   scripts/capture_window.sh NAME FROM TO
#
# Reassembles falcon/smkx_rt.s with SMKX_SNAP_EVERY=1 SMKX_SNAP_FROM=FROM SMKX_FRAMES=TO, relinks it
# with the C objects of the last build/ctx build (no cross-GCC needed), stages it as AUTO/GAME.PRG
# in build/ctxw_NAME and runs it in F030Arcade's patched Hatari until the program terminates.
# The snapshots F*.BIN land in build/ctxw_NAME. Stops only the Hatari it started (own PID).
# Emulator frame numbers, not a Falcon's; record the Hatari identity beside any figure
# (first line of build/ctxw_NAME/hatari-log.txt).
set -eu
name=$1; from=$2; to=$3
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH
vasm=../../../F030Arcade/third_party/vasm/vasmm68k_mot.exe
vlink=../../../F030Arcade/third_party/vlink/vlink.exe
hatari=../../../F030Arcade/third_party/hatari/build-ucrt64/src/hatari.exe
tos=../../../F030Arcade/third_party/tos/tos404.img
c=build/ctx
d=build/ctxw_$name
for f in $hatari $tos $c/CTX.TOS $c/sources.txt; do [ -e "$f" ] || { echo "missing $f" >&2; exit 1; }; done
rm -rf "$d"; mkdir -p "$d/AUTO"
cp $c/ct.sfc "$d/"
# the switches of the last build, except the snapshot ones
$vasm -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$c -DSMKX_FRAMES=$to \
  -DSMKX_SNAP_EVERY=1 -DSMKX_SNAP_FROM=$from -DSMKX_M7_DSP=1 -DSMKX_PROFILE_FROM=0 -DSMKX_DIAG=1 \
  -DCT_LZ=1 -DCT_LZ_CHECK=0 -DSMKX_TC=0 -o "$d/rt.o" falcon/smkx_rt.s
$vlink -b ataritos -tos-flags 1 -M"$d/CTXW.map" -o "$d/AUTO/GAME.PRG" "$d/rt.o" \
  $(sed 's/\.s$/.o/;s|^|build/ctx/|' $c/sources.txt) $c/tables.o $c/input.o $c/snes_ppu.o \
  $c/smkx_video.o $c/palette.o $c/smkx_m7.o $c/m7_plan.o $c/spc700.o $c/smkx_apu.o $c/smkx_ppu.o \
  $c/ppu_dsp_plan.o 2>&1 | grep -v "Warning 64" || true
winroot=$(cygpath -m "$root")
nohup $hatari --machine falcon --dsp emu --memsize 14 --ttram 0 --tos "$(cygpath -m "$tos")" \
  --fast-boot true --cpu-exact true --compatible true --patch-tos true --monitor rgb --sound off \
  --natfeats true --confirm-quit off --alert-level fatal --debug-except none --disable-video on \
  --fast-forward true --trace event --trace-file "$winroot/$d/hatari-trace.log" \
  --log-file "$winroot/$d/hatari-log.txt" "$winroot/$d" > "$d/hatari-stderr.log" 2>&1 &
pid=$!
# stop exactly the Hatari started here: the one whose command line names this run's directory
stop() {
  powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='hatari.exe'\" | Where-Object { \$_.CommandLine -like '*$winroot/$d*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1 || true
}
trap stop EXIT
echo "hatari pid $pid, window $from..$to -> $d"
for _ in $(seq 1 1200); do
  grep -q "Program termination" "$d/hatari-trace.log" 2>/dev/null && break
  kill -0 $pid 2>/dev/null || break
  sleep 1
done
stop
echo "$(ls $d/F*.BIN 2>/dev/null | wc -l) snapshots in $d"

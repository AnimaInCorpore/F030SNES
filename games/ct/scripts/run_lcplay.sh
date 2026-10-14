#!/usr/bin/env bash
# Runs falcon/lcplay.s with a recorded stream in F030Arcade's patched Hatari (Windows, MSYS2).
#
#   scripts/run_lcplay.sh check [STREAM.lcp]   verify: every frame's page window summed and
#                                              compared with the stream's checksum (NatFeats line)
#   scripts/run_lcplay.sh view  [STREAM.lcp]   a window you can watch; loops until a key
#   scripts/run_lcplay.sh record [STREAM.lcp]  one pass with video on, Hatari records an AVI
#                                              (build/lcplay/run_record/out.avi, PNG frames) and quits
#                                              after VBLS vertical blanks (default 1600)
#
# Builds build/lcplay/LCPLAY{1,0}.TOS (vasm + vlink), stages AUTO/GAME.PRG in build/lcplay/run_<mode>
# and stops only the Hatari it started (the one naming that directory). Emulator, not a Falcon:
# the blitter and the Videl are Hatari's, the speed means nothing. Hatari identity: first line of
# hatari-log.txt in the run directory.
set -eu
mode=${1:-check}
stream=${2:-build/host/outdoor.lcp}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH
vasm=../../../F030Arcade/third_party/vasm/vasmm68k_mot.exe
vlink=../../../F030Arcade/third_party/vlink/vlink.exe
hatari=../../../F030Arcade/third_party/hatari/build-ucrt64/src/hatari.exe
tos=../../../F030Arcade/third_party/tos/tos404.img
for f in $hatari $tos $stream falcon/lcplay.s; do [ -e "$f" ] || { echo "missing $f" >&2; exit 1; }; done
b=build/lcplay
mkdir -p $b
cp "$stream" $b/outdoor.lcp
check=0; [ "$mode" = check ] && check=1; check=${CHK:-$check}
loop=1; [ "$mode" = record ] && loop=0
(cd $b && ../../$vasm -Felf -m68030 -quiet -nowarn=62 -nowarn=2058 -DCHECK=$check -DLOOP=$loop -DNOHS=${NOHS:-0} -DCOPYSCR=${COPYSCR:-0} -o lcplay$check$loop.o ../../falcon/lcplay.s &&
  ../../$vlink -b ataritos -tos-flags 1 -Mlcplay$check$loop.map -o LCPLAY$check$loop.TOS lcplay$check$loop.o 2>&1 | grep -v "Warning 64" || true)
d=$b/run_$mode
mkdir -p $d/AUTO
rm -f $d/hatari-trace.log $d/hatari-log.txt $d/hatari-stderr.log
cp $b/LCPLAY$check$loop.TOS $d/AUTO/GAME.PRG
winroot=$(cygpath -m "$root")
if [ "$mode" = check ]; then
  extra="--disable-video on --fast-forward true"
elif [ "$mode" = record ]; then
  extra="--fast-forward false --avirecord true --avi-vcodec png --avi-fps 50 --avi-file $winroot/$d/out.avi --run-vbls ${VBLS:-1600}"
  rm -f $d/out.avi
else
  extra="--fast-forward false"
fi
nohup $hatari --machine falcon --dsp emu --memsize 14 --ttram 0 --tos "$(cygpath -m "$tos")" \
  --fast-boot true --cpu-exact true --compatible true --patch-tos true --monitor rgb --sound off \
  --natfeats true --confirm-quit off --alert-level fatal --debug-except none $extra \
  --trace event --trace-file "$winroot/$d/hatari-trace.log" \
  --log-file "$winroot/$d/hatari-log.txt" "$winroot/$d" > "$d/hatari-stderr.log" 2>&1 &
echo "started Hatari ($mode)"
stop() {
  powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='hatari.exe'\" | Where-Object { \$_.CommandLine -like '*$winroot/$d*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1 || true
}
if [ "$mode" = record ]; then
  trap stop EXIT
  for _ in $(seq 1 150); do
    grep -q "Program termination" "$d/hatari-trace.log" 2>/dev/null && break
    sleep 1
  done
  sleep 2
  stop
  ls -la $d/out.avi 2>&1
fi
if [ "$mode" = check ]; then
  trap stop EXIT
  for _ in $(seq 1 600); do
    grep -q "LCPLAY: frames" "$d/hatari-stderr.log" 2>/dev/null && break
    grep -q "Program termination" "$d/hatari-trace.log" 2>/dev/null && break
    sleep 1
  done
  stop
  grep "LCPLAY" "$d/hatari-stderr.log" || { echo "no result"; tail -5 "$d/hatari-stderr.log"; exit 1; }
fi

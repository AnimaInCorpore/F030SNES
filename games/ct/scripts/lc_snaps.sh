#!/usr/bin/env bash
# Snapshots F*.BIN of a stretch of the running game in the patched Hatari (macOS/Linux), into a directory of their own.
#
#   scripts/lc_snaps.sh DIR FROM TO EVERY [extra make variables...]
#
# (LC_TOS=file runs a program built earlier instead.)
# Builds CTX.TOS with CTX_SNAP_FROM=FROM CTX_SNAP_EVERY=EVERY CTX_FRAMES=TO (EVERY=1: consecutive frames, the input of
# tools/lc/lcd_model.c; EVERY=0: none, the status lines at the end only), copies it into DIR and runs it there: Hatari's GEMDOS
# drive is the program's directory, so the snapshots land in DIR. Extra arguments go to make (CTX_LC=0 for the old renderer,
# ...). Uses ../../../F030Arcade's patched Hatari directly (it starts its own Hatari, waits for the program's termination and stops that
# one process: nothing is shared with other runs but the build directory, so do not run two builds at once); the log lines
# of the runtime (SMKX ...) are in DIR/run.log, Hatari's whole log in DIR/hatari.log. Emulator frame numbers, not a Falcon's.
set -eu
dir=$1; from=$2; to=$3; every=$4; shift 4
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
hatari=../../../F030Arcade/third_party/hatari/build/src/hatari
tos=../../../F030Arcade/third_party/tos/tos404.img
[ -x "$hatari" ] || { echo "patched F030Arcade Hatari missing: $hatari" >&2; exit 1; }
if [ -z "${LC_TOS:-}" ]; then
  make ctx CTX_SNAP_FROM="$from" CTX_SNAP_EVERY="$every" CTX_FRAMES="$to" "$@" >/dev/null
  tos_file=build/ctx/CTX.TOS
else
  tos_file=$LC_TOS                       # a program built earlier (LC_TOS=path): no build, FROM/TO/EVERY are its own
fi
mkdir -p "$dir"
rm -f "$dir"/F*.BIN "$dir"/SNAP.BIN "$dir"/trace.log "$dir"/hatari.log "$dir"/run.log
cp "$tos_file" "$dir/CTX.TOS"
"$hatari" --machine falcon --dsp emu --memsize 14 --ttram 0 --tos "$tos" --fast-boot true --cpu-exact true --compatible true \
  --patch-tos true --monitor rgb --sound off --natfeats true --confirm-quit off --alert-level fatal --fast-forward true \
  --trace event --trace-file "$dir/trace.log" "$dir/CTX.TOS" > "$dir/hatari.log" 2>&1 &
pid=$!
start=$(date +%s)
while kill -0 "$pid" 2>/dev/null; do
  grep -q "Program termination" "$dir/trace.log" 2>/dev/null && break
  [ $(( $(date +%s) - start )) -gt "${LC_TIMEOUT:-3000}" ] && { echo "timeout" >&2; break; }
  sleep 2
done
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
grep -a "SMKX" "$dir/hatari.log" > "$dir/run.log" || true
echo "$(ls "$dir"/F*.BIN 2>/dev/null | wc -l) snapshots in $dir ($(grep -a -m1 -o 'Hatari v[^,]*' "$dir/hatari.log"))"

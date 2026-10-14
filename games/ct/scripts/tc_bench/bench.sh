#!/bin/bash
# bench.sh TAG [profile]: run the 6 packs in parallel and summarise
cd "$(dirname "$0")/../.."
export SDL_VIDEODRIVER=dummy
TAG=$1; MODE=${2:-}
for k in 0 1 2 3 4 5; do
  xvfb-run -a python3 scripts/tc_bench/run.py $TAG $k $MODE > build/bench/$TAG/run$k.out 2>&1 &
done
wait
python3 scripts/tc_bench/cmp.py ${BASE:-base} $TAG

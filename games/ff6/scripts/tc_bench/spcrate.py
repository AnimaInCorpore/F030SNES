#!/usr/bin/env python3
"""68030 cycles per SPC700 instruction from a CPU profile (build/cpuprof/TAG_START.txt of cpuprof.py):
the cycles of spc_run and timers over the executions of the step counter's increment (`addq.l #1,...`
in spc_run, one per instruction). Emulated cycles in the patched Hatari.

    scripts/tc_bench/spcrate.py FILE [FILE...]
"""
import re, sys

for f in sys.argv[1:]:
    cur, cyc, steps = None, 0, 0
    for l in open(f, errors="replace"):
        m = re.match(r"^([A-Za-z_.$][\w.$]*):\s*$", l)
        if m:
            cur = m[1]
            continue
        m = re.match(r"^([0-9a-f]{8}) (.*?)([\d.]+)% \((\d+), (\d+), (\d+), (\d+)\)\s*$", l)
        if m and cur in ("spc_run", "timers", "step"):
            cyc += int(m[5])
            if cur == "spc_run" and re.search(r"addq\.l #\$01,\(", m[2]):
                steps = max(steps, int(m[4]))
    print(f"{f}: {cyc} cycles, {steps} steps" + (f", {cyc / steps:.1f} cycles a step" if steps else ""))

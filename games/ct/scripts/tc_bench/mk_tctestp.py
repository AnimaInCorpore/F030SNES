#!/usr/bin/env python3
"""Writes build/bench/tctestp.c: src/tctest.c without the banded re-run (the profile and
timing runs need one render per frame) and with an SSIOFF command at the end (the DSP profile's
end marker, scripts/tc_bench/prof1.py)."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
s = (root / "src/tctest.c").read_text()
i = s.index("        if (f % 3 == 0) {")
j = s.index('        say("TCTEST timing')
s = s[:i] + s[j:]
s = s.replace('    say("TCTEST frames $", tested);',
              '    put(9);                                 /* SSIOFF: the profile\'s end marker */\n    say("TCTEST frames $", tested);')
(root / "build/bench").mkdir(parents=True, exist_ok=True)
(root / "build/bench/tctestp.c").write_text(s)

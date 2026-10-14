#!/usr/bin/env python3
"""Profile the DSP kernel inside a game run with Hatari's DSP profiler.

    python3 scripts/dsp_profile.py --program build/ctx/CTX.TOS --tag field --vbl 8205-8655 \
        --lst build/ctx/ppu.lst

Hatari's debugger (`dp`, the DSP profiler) counts instruction cycles per DSP
address and reports them as hardware time (cycles / 32 MHz), whatever speed the
emulator runs at. This driver turns it on at the first VBL and dumps the
profile at the second, from `--parse` scripts and `:trace :once :file`
breakpoints (the Windows build has no FIFO; the debugger reads its own console
window, so the process is stopped by its PID once the dump is on disk). Symbols
come from the Motorola assembler listing of the kernel the scene runs (labels
on a line of their own take the next instruction's address). Output:
build/ctx/<tag>-dp.log (dp stats, symbols, cycles), <tag>-dp.prof (dp save),
<tag>-dsp.sym. Use the patched F030Arcade Hatari only (AGENTS.md).
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
TOS = ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"


def symbols(lst: Path, out: Path) -> list[tuple[int, str]]:
    """Code labels and their P addresses from the listing (lines skipped by a
    false `if` are not in the listing at all); a label on a line of its own
    takes the next instruction's address."""
    syms: list[tuple[int, str]] = []
    pending: list[str] = []
    with lst.open(errors="replace") as f:
        for line in f:
            m = re.match(r"^\s*\d+\s+P:([0-9A-F]{4})\s+[0-9A-F]{6}\s+(?:(\w+):)?", line)
            if m:
                addr = int(m.group(1), 16)
                for name in pending:
                    syms.append((addr, name))
                pending = []
                if m.group(2):
                    syms.append((addr, m.group(2)))
                continue
            m = re.match(r"^\s*\d+\s{10,}(\w+):\s*(;.*)?$", line)
            if m:
                pending.append(m.group(1))
    with out.open("w", newline="\n") as o:
        for addr, name in syms:
            o.write(f"0x{addr:04X} T {name}\n")
    return syms


def report(prof: Path, syms: list[tuple[int, str]], top: int) -> str:
    """Cycles per symbol range from `dp save` output (one line per address)."""
    starts = sorted(set(a for a, _ in syms))
    name = {}
    for a, n in syms:
        name.setdefault(a, n)           # the first label at an address names the range
    cyc: dict[int, int] = {}
    ins: dict[int, int] = {}
    total = 0
    for line in prof.read_text(errors="replace").splitlines():
        m = re.match(r"^p:([0-9a-f]+)\s.*\((\d+), (\d+), \d+\)$", line)
        if not m:
            continue
        addr, i, c = int(m.group(1), 16), int(m.group(2)), int(m.group(3))
        lo, hi = 0, len(starts)
        while lo < hi:                  # the last symbol at or before the address
            mid = (lo + hi) // 2
            if starts[mid] <= addr:
                lo = mid + 1
            else:
                hi = mid
        base = starts[lo - 1] if lo else -1
        cyc[base] = cyc.get(base, 0) + c
        ins[base] = ins.get(base, 0) + i
        total += c
    # Local labels (`_name`) belong to the global label before them: the
    # function view folds them in; the range view keeps them apart.
    func_of: dict[int, int] = {}
    cur = -1
    for a in starts:
        if not name[a].startswith("_"):
            cur = a
        func_of[a] = cur
    fcyc: dict[int, int] = {}
    fins: dict[int, int] = {}
    for base, c in cyc.items():
        fb = func_of.get(base, -1)
        fcyc[fb] = fcyc.get(fb, 0) + c
        fins[fb] = fins.get(fb, 0) + ins[base]
    out = [f"DSP cycles by function ({total} cycles, {total / 32e6:.3f} s at 32 MHz; "
           "'?' = before the first label: the interrupt vectors):",
           f"{'function':<14}{'addr':>7}{'cycles':>13}{'share':>8}{'instr':>12}"]
    for base, c in sorted(fcyc.items(), key=lambda kv: -kv[1])[:top]:
        out.append(f"{name.get(base, '?'):<14}{base:>7X}{c:>13}{100 * c / total:>7.2f}%{fins[base]:>12}")
    out.append("")
    out.append(f"{'range':<14}{'addr':>7}{'cycles':>13}{'share':>8}{'instr':>12}")
    for base, c in sorted(cyc.items(), key=lambda kv: -kv[1])[:top]:
        out.append(f"{name.get(base, '?'):<14}{base:>7X}{c:>13}{100 * c / total:>7.2f}%{ins[base]:>12}")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--program", type=Path, default=ROOT / "build/ctx/CTX.TOS")
    ap.add_argument("--tag", default="dp")
    ap.add_argument("--vbl", default="0-0", help="FROM-TO: profile between these Hatari VBL counts "
                    "(about 194 more than the game's vbl field in the status lines: the TOS boot)")
    ap.add_argument("--lst", type=Path, default=ROOT / "build/ctx/ppu.lst")
    ap.add_argument("--top", type=int, default=48)
    ap.add_argument("--timeout", type=float, default=1200.0)
    ap.add_argument("--report", action="store_true", help="only the report from an existing <tag>-dp.prof")
    args = ap.parse_args()
    out = ROOT / "build/ctx"
    tag = args.tag
    sym = out / f"{tag}-dsp.sym"
    syms = symbols(args.lst, sym)
    print(f"{len(syms)} symbols from {args.lst}")
    if args.report:
        print(report(out / f"{tag}-dp.prof", syms, args.top))
        return 0
    hatari = HATARI if HATARI.is_file() else HATARI.with_suffix(".exe")
    if not hatari.is_file():
        raise SystemExit(f"patched F030Arcade Hatari missing: {HATARI}")
    v0, v1 = (int(x) for x in args.vbl.split("-"))
    drive = out / f"{tag}-drive"
    (drive / "AUTO").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.program, drive / "AUTO" / "CTX.PRG")
    log, prof = out / f"{tag}-dp.log", out / f"{tag}-dp.prof"
    for f in (log, prof):
        if f.exists():
            f.unlink()
    on, end, start = out / f"{tag}-on.ini", out / f"{tag}-end.ini", out / f"{tag}-start.ini"
    on.write_text("dp on\nc\n", newline="\n")
    end.write_text(f"dp off\ndp stats\ndp symbols {args.top}\ndp cycles {args.top}\ndp save {prof.as_posix()}\n"
                   "logfile\nc\n", newline="\n")
    start.write_text(f"logfile {log.as_posix()}\ndspsymbols {sym.as_posix()}\n"
                     f"b VBL = {v0} :trace :once :file {on.as_posix()}\n"
                     f"b VBL = {v1} :trace :once :file {end.as_posix()}\nc\n", newline="\n")
    trace, err = out / f"{tag}-trace.log", out / f"{tag}-stderr.log"
    command = [
        str(hatari), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
        "--ttram", "0", "--tos", str(TOS), "--fast-boot", "true",
        "--cpu-exact", "true", "--compatible", "true", "--patch-tos", "true",
        "--monitor", "rgb", "--sound", "off", "--natfeats", "true",
        "--confirm-quit", "off", "--alert-level", "fatal", "--fast-forward", "true",
        "--debug-except", "none", "--parse", start.as_posix(),
        "--trace", "event", "--trace-file", str(trace), str(drive),
    ]
    with err.open("w") as e:
        proc = subprocess.Popen(command, cwd=ROOT, stdout=e, stderr=subprocess.STDOUT)
    started = time.monotonic()
    try:
        while time.monotonic() - started < args.timeout:
            if prof.exists() and prof.stat().st_size > 0 and log.exists():
                time.sleep(2.0)             # the log closes after the save
                break
            if proc.poll() is not None:
                break
            time.sleep(1.0)
        else:
            print("timeout", file=sys.stderr)
    finally:
        if proc.poll() is None:
            proc.terminate()                # this PID only
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    if prof.exists():
        print(report(prof, syms, args.top))
    return 0 if prof.exists() else 1


if __name__ == "__main__":
    sys.exit(main())

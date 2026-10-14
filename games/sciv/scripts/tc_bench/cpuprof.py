#!/usr/bin/env python3
"""CPU profile of the 68030 side of the true-colour renderer (planning, compose, upload).

    scripts/tc_bench/cpuprof.py TAG START[,START...] [COUNT]

TAG names a build: build/scivx/SCIVX_TAG.TOS and build/cpuprof/TAG.map (copies of SCIVX.TOS
and SCIVX.map of that build). Each START is the number of the smkx_ppu_plan call (about one per
picture) where the profile starts; it runs COUNT calls (default 60). The breakpoints are
absolute addresses from the link map (text base = TOS program text start), so each version
profiles the same pictures. Emulated time in the patched Hatari; write its identity beside
any figure. Prints ms of CPU per smkx_ppu_plan call by routine group.
"""
import collections, os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
TOS = ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"
BASE = 0x1EFE4                      # start of the program text as Hatari loads it
CPS = 16042.494                     # cycles per ms
PLAN = set("need_f need flush_chr layer_config block_bits need_maprow need_cell mark_f need_span_tc "
           "pend_flush pend_add need_span mark math_row update m7_word m7_entry close_span plan_row put "
           "rows_hit seg_pals put_seg relocate dsp_plan dsp_tc_runs replay hdma_events hdma_init hdma_read "
           "ppu_replay_changes windows_possible outside ppu_sub_cfg mark_spans ppu_win_spans win_spans sext13 "
           "clip13 plan_rows smkx_ppu_plan slots_needed hdma_line_counter".split())
COMPOSE = set("compose input smkx_render smkx_render_start".split())
UPLOAD = set("slots upload w24 smkx_ppu_send take_dirty pages_due pre_build slot_save slot_restore pre_discard".split())


def sym(mapfile, name):
    for l in open(mapfile, errors="replace"):
        m = re.match(r"\s*0x([0-9a-f]+) %s: " % re.escape(name), l)
        if m:
            return int(m[1], 16) + BASE
    raise SystemExit(f"{name} not in {mapfile}")


def run(tag, start, count):
    d = ROOT / "build/cpuprof"
    addr = sym(d / f"{tag}.map", os.environ.get("SYM", "smkx_ppu_plan"))
    prof = d / f"{tag}_{start}.txt"
    (d / f"{tag}_{start}.start").write_text("profile on\n")
    (d / f"{tag}_{start}.stop").write_text(f"profile save {prof}\nprofile off\nquit\n")
    (d / f"{tag}_{start}.parse").write_text(
        f"b pc = ${addr:x} :{start} :once :file {d}/{tag}_{start}.start\n"
        f"b pc = ${addr:x} :{start + count} :once :file {d}/{tag}_{start}.stop\n")
    if prof.exists():
        prof.unlink()
    cmd = [str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14", "--ttram", "0", "--tos", str(TOS),
           "--fast-boot", "true", "--cpu-exact", "true", "--compatible", "true", "--patch-tos", "true",
           "--monitor", "rgb", "--sound", "off", "--natfeats", "true", "--confirm-quit", "off",
           "--alert-level", "fatal", "--fast-forward", "true", "--parse", str(d / f"{tag}_{start}.parse"),
           str(ROOT / f"build/scivx/SCIVX_{tag}.TOS")]
    with open(d / f"{tag}_{start}.log", "w") as log:
        return subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT), prof


def summarise(prof, count):
    cur, fn = None, collections.Counter()
    for l in open(prof, errors="replace"):
        m = re.match(r"^([A-Za-z_.$][\w.$]*):\s*$", l)
        if m:
            cur = m[1]
            continue
        m = re.match(r"^([0-9a-f]{8}) .*?([\d.]+)% \((\d+), (\d+), (\d+), (\d+)\)\s*$", l)
        if m and cur:
            if int(m[1], 16) >= 0xe00000:
                continue                # TOS ROM (interrupts): not the renderer
            if cur == "compose" and int(m[3]) > 20000 * count:
                continue                # wait_dsp's spin loop (`while (smkx_inflight)`), not work
            fn[cur] += int(m[4])
    g = lambda S: sum(v for k, v in fn.items() if k in S) / CPS / count
    top = sorted(((v / CPS / count, k) for k, v in fn.items() if k in PLAN), reverse=True)[:6]
    if os.environ.get("FULL"):
        allf = sorted(((v / CPS / count, k) for k, v in fn.items()), reverse=True)[:16]
        print("   all: " + " ".join(f"{k} {v:.1f}" for v, k in allf))
    return g(PLAN), g(COMPOSE), g(UPLOAD), top


def main():
    tag, starts = sys.argv[1], [int(x) for x in sys.argv[2].split(",")]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    procs = [(s, *run(tag, s, count)) for s in starts]
    deadline = time.monotonic() + 1800
    for s, p, prof in procs:
        while not prof.exists() and p.poll() is None and time.monotonic() < deadline:
            time.sleep(2)
        if p.poll() is None:
            p.terminate()               # our own process (by pid), after `quit` had no time
        if not prof.exists():
            print(f"start {s}: no profile"); continue
        plan, comp, up, top = summarise(prof, count)
        print(f"{tag} from call {s}: plan {plan:.1f} ms/pic, compose {comp:.1f}, upload+send {up:.1f}   "
              + " ".join(f"{k} {v:.1f}" for v, k in top))


if __name__ == "__main__":
    main()

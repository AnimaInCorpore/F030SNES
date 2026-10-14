#!/usr/bin/env python3
"""The 68030 side's planning of stored real frames, the same frames in every build.

    scripts/tc_bench/planbench.py pack [N]      # build/planbench/planbench.bin from build/bench15/F*.BIN
    scripts/tc_bench/planbench.py build TAG [REV]  # build/planbench/PB_TAG.TOS from the current source (or the src/ of REV)
    scripts/tc_bench/planbench.py run TAG       # profile it in the patched Hatari; ms a picture by routine

PLANBENCH (src/planbench.c) parses N snapshots, plans each once as a warm-up, then plans them twice more
between two markers (bench_go, bench_done); the CPU profile of the second part is summed by
routine. The pages written since the previous frame are passed as the runtime's vdirty.
Emulated time in the patched Hatari (write its identity beside any figure); M68KCC names the
cross compiler.
"""
import collections, os, re, struct, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D = ROOT / "build/planbench"
HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
TOS = ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"
CPS = 16042.494
BASE = 0x1EFE4
CC = os.environ.get("M68KCC", "m68k-atari-mintelf-gcc")
VASM = ROOT.parents[2] / "F030Arcade/third_party/vasm/vasmm68k_mot"
VLINK = ROOT.parents[2] / "F030Arcade/third_party/vlink/vlink"
CFLAGS = ("-m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fomit-frame-pointer "
          "-nostdlib -Wall -Isrc -DSMKX_TC=1").split()
SRC = ["planbench.c", "snes_ppu.c", "ppu_dsp_plan.c", "m7_plan.c"]
PLAN = set("need_f need flush_chr layer_config block_bits need_maprow need_span_tc pend_flush pend_add need_span "
           "mark math_row update m7_word m7_zeros m7_entry close_span plan_row put rows_hit seg_pals put_seg relocate "
           "dsp_plan dsp_tc_runs replay hdma_events hdma_init hdma_read ppu_replay_changes windows_possible outside "
           "ppu_sub_cfg mark_spans ppu_win_spans win_spans sext13 clip13 hdma_line_counter copy4 chr_bits "
           "bench_plan memset memcpy reg_write.constprop.0 reg_write ppu_m7_setup m7_plan ppu_opt_slivers "
           "ppu_win_layers math_possible dsp_rows rows_finish rows_layer rows_block_dirty rows_sprites rows_span_cmp rmark "
           "spr_extent rows_extent rows_optmap_dirty bench_rows block_rebuild".split())


def sh(cmd, **kw):
    subprocess.run([str(c) for c in cmd], check=True, cwd=ROOT, **kw)


def pack(n=20):
    files = sorted((ROOT / "build/bench15").glob("F*.BIN"))
    D.mkdir(parents=True, exist_ok=True)
    g = 5                               # runs of consecutive snapshots (15 game frames apart), from spread starts
    starts = [i * (len(files) - g) // (n // g - 1) for i in range(n // g)]
    pick = [files[s0 + k] for s0 in starts for k in range(g)]
    out = bytearray(struct.pack(">I", n))
    for f in pick:
        b = f.read_bytes()
        out += struct.pack(">I", len(b)) + b
    (D / "planbench.bin").write_bytes(out)
    (D / "sciv.sfc").write_bytes((ROOT / "build/scivx/sciv.sfc").read_bytes())
    print(f"{n} frames, {len(out)} bytes:", " ".join(f.name for f in pick))


def build(tag, rev=None, extra=()):
    """From the working tree, or from the src/ of a git revision (planbench.c stays the current one)."""
    d = D / tag
    d.mkdir(parents=True, exist_ok=True)
    tree = ROOT
    if rev:
        tree = d / "tree"
        sh(["rm", "-rf", tree])
        tree.mkdir()
        subprocess.run(f"git archive {rev} src | tar -x -C {tree}", shell=True, check=True, cwd=ROOT)
        sh(["cp", ROOT / "src/planbench.c", tree / "src/planbench.c"])
    objs = []
    for s in SRC:
        o = d / (s[:-2] + ".o")
        sh([CC, *[c.replace("-Isrc", "-I" + str(tree / "src")) for c in CFLAGS], *extra, "-c", "-o", o, tree / "src" / s])
        objs.append(o)
    sh([VASM, "-Felf", "-m68030", "-quiet", "-nowarn=62", "-I" + str(D), "-o", d / "glue.o", ROOT / "falcon/planbench.s"])
    sh([VLINK, "-b", "ataritos", "-tos-flags", "1", "-M" + str(d / "map.txt"), "-o", D / f"PB_{tag}.TOS", d / "glue.o", *objs])


def addr(mapfile, name):
    for l in open(mapfile, errors="replace"):
        m = re.match(r"\s*0x([0-9a-f]+) %s: " % re.escape(name), l)
        if m:
            return int(m[1], 16) + BASE
    raise SystemExit(f"{name} not in {mapfile}")


def run(tag):
    d = D / tag
    prof = d / "prof.txt"
    if prof.exists():
        prof.unlink()
    a, b = addr(d / "map.txt", "bench_go"), addr(d / "map.txt", "bench_done")
    (d / "start").write_text("profile on\n")
    (d / "stop").write_text(f"profile save {prof}\nprofile off\nquit\n")
    (d / "parse").write_text(f"b pc = ${a:x} :once :file {d}/start\nb pc = ${b:x} :once :file {d}/stop\n")
    cmd = [str(HATARI), "--machine", "falcon", "--dsp", "none", "--memsize", "14", "--ttram", "0", "--tos", str(TOS),
           "--fast-boot", "true", "--cpu-exact", "true", "--compatible", "true", "--patch-tos", "true",
           "--monitor", "rgb", "--sound", "off", "--natfeats", "true", "--confirm-quit", "off",
           "--alert-level", "fatal", "--fast-forward", "true", "--parse", str(d / "parse"),
           str(D / f"PB_{tag}.TOS")]
    with open(d / "log.txt", "w") as log:
        p = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        t0 = time.monotonic()
        while not prof.exists() and p.poll() is None and time.monotonic() - t0 < 900:
            time.sleep(1)
        time.sleep(1)
        if p.poll() is None:
            p.terminate()
    if not prof.exists():
        raise SystemExit("no profile: see " + str(d / "log.txt"))
    nfr = int(re.search(r"PLANBENCH frames ([0-9A-F]+)", (d / "log.txt").read_text(errors="replace") or "PLANBENCH frames 14")[1], 16) if "PLANBENCH frames" in (d / "log.txt").read_text(errors="replace") else 20
    cur, fn = None, collections.Counter()
    for l in open(prof, errors="replace"):
        m = re.match(r"^([A-Za-z_.$][\w.$]*):\s*$", l)
        if m:
            cur = m[1]
            continue
        m = re.match(r"^([0-9a-f]{8}) .*?([\d.]+)% \((\d+), (\d+), (\d+), (\d+)\)\s*$", l)
        if m and cur and int(m[1], 16) < 0xe00000:
            fn[cur] += int(m[4])
    pics = 2 * nfr
    tot = sum(v for k, v in fn.items() if k in PLAN) / CPS / pics
    top = sorted(((v / CPS / pics, k) for k, v in fn.items() if k in PLAN), reverse=True)[:8]
    print(f"{tag}: planning {tot:.2f} ms a picture (mean of {pics} pictures)   " + " ".join(f"{k} {v:.1f}" for v, k in top))


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "pack":
        pack(int(sys.argv[2]) if len(sys.argv) > 2 else 20)
    elif cmd == "build":
        rev = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("-") else None
        build(sys.argv[2], rev, [a for a in sys.argv[3:] if a.startswith("-")])
    elif cmd == "run":
        run(sys.argv[2])

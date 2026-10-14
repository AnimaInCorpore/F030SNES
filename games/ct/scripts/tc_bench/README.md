# True-colour kernel benchmark (falcon/dsp/tc.asm)

Renders real frames on the DSP in the patched Hatari (TCTEST) and compares the time and the
pixels against a stored baseline. Emulated time only; write the Hatari build identity beside
any figure (`git -C ../../../F030Arcade/third_party/hatari rev-parse --short HEAD`).

Frozen inputs (untracked, from the ROM):

1. `make ctx CTX_TC=1 CTX_SNAP_EVERY=250`, run it (`scripts/run_smkx.py --fast --timeout 900`),
   copy `build/ctx/F*.BIN` to `build/bench/`.
2. `build/tc_snap build/ctx/ct.sfc build/bench/F*.BIN -pack build/bench/real.bin > build/bench/snap.txt`
3. `python3 scripts/tc_bench/split.py` (tile frames in five packs, the Mode 7 frames in a sixth;
   `build/bench/packidx.json` maps a pack's frames back to `real.bin`).
4. `python3 scripts/tc_bench/mk_tctestp.py` (the TCTEST variant for timing and profiles).

Per version (`TAG`):

- `scripts/tc_bench/build.sh TAG` assembles `falcon/dsp/tc.asm` (tile and TC7 builds) and links
  the six TCTEST programs (`M68KCC` names the cross compiler).
- `scripts/tc_bench/bench.sh TAG` runs the six packs in parallel, then
  `cmp.py BASE TAG`: frames with differing words (must be 0), mean time per group.
  Run the first version as `TAG=base`; `BASE=v3 bench.sh v4` compares with another.
- `scripts/tc_bench/prof1.py TAG PACK` writes `build/bench/TAG/profPACK.txt` (the Hatari DSP
  profile, from the first `render` to the SSIOFF marker); `brk.py TAG N [OTHER]` sums it by
  routine (the assembler listing `build/bench/TAG/tc.lst` names the routines) and `hot.py`
  lists the hot lines of a routine.

Before trusting a run: all frames exact, no `TIMEOUT` in the logs, and the guards in `tc.asm`
(external program below P:$1000, the TC7 program below P:$0C00) did not fail the assembly.

## The 68030 side (planning)

- `planbench.py pack|build TAG [REV]|run TAG`: PLANBENCH plans 20 stored real frames as the
  runtime does and the Hatari CPU profile of the timed part is summed by routine (ms of CPU a
  picture). Deterministic; use it to compare planner versions (`build base d83dbf5` builds the
  `src/` of a git revision; tags are at most 5 characters: the TOS names are 8.3, and two names
  that shorten to the same one make Hatari run the wrong program). Needs `build/bench15/F*.BIN`
  (`make ctx CTX_TC=1 CTX_SNAP_EVERY=15`, run it) and `build/ctx/ct.sfc`.
- `cpuprof.py TAG START[,START...]`: the same profile inside the running game, from a given
  `smkx_ppu_plan` call. Only for looking at where the time goes: the pictures differ between
  builds.
- `build/tc_snap rom.sfc -planhash SNAP.BIN...`: a hash of `dsp_plan`'s output per frame; it must not
  change with a planner change (and prints `TRUST-MISMATCH` when the trusted block cache disagrees
  with the compared one).

## Partial rendering (src/ppu_rows.c, dsp_rows in src/ppu_dsp_plan.c)

- `build/tc_snap rom.sfc -rowclean [-rowgap N] SNAP.BIN...`: `dsp_rows` on consecutive snapshots against
  `ppu_render_565`: the rows it calls unchanged must render as they did in the committed picture (every
  N-th one). Prints the share of rows called unchanged and of pictures with nothing to draw.
- `-rowfuzz N`: N random VRAM/CGRAM/OAM mutations of every snapshot (a page the plan reads, a palette entry,
  a sprite byte), only that page marked written: the same check.
- `-rowbuf`: `ppu_rows.c` (the code the runtime runs) with two simulated screen buffers, pictures planned
  during or after the last flight, some sync: the buffer shown must equal the reference in every row.
- `make ctx CTX_TC=1 CTX_PARTIAL_CHECK=2`: the game with every picture also drawn whole by the DSP and compared
  with the partial one (`SMKX PARTIAL CHECK` lines, counters 15/16 of the ppudsp status field);
  `CTX_PARTIAL=0|1|2`: partial rendering off / changed rows only / also skipping unchanged pictures.
- `picrate.py TAG...`: pictures a second, skipped pictures, rows drawn and changed pictures of game runs.

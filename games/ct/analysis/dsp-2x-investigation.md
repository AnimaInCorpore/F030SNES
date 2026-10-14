# Is a 2x DSP speedup possible? (true-colour renderer, 2026-09-29)

**Verdict.** Not on average, at the same visual quality. Measured: **1.32x** on the mean
render time of 222 real tile frames (302.8 -> 228.8 ms), every frame still bit-exact.
The frames that were slow for a specific reason gained up to 2.25x; the frames that already sit
at the output floor gained nothing and cannot. A further 1.1-1.2x is available with more work
(list at the end); 2x would need the output floor itself to move.

All figures are emulated: patched Hatari `3eea7b0` (`AnimaInCorpore/hatari` HEAD; the DSP now
runs at its real clock, so absolute numbers differ from the older `v2.6.1-497-g639fadf6`
figures in the other notes: compare only within this note). Not a physical Falcon. Kernel time
is TCTEST's render+record interval, after the stream was sent and the pages were uploaded:
not the game's picture rate, which also depends on the 68030's planning (about 38 ms of CPU a
picture, see "The CPU side") and the game code.

## Method

- **Inputs.** 246 real snapshots of the attract demo (every 250th frame of 31,000 frames;
  `build/bench/`, untracked). 222 tile frames are compared; 14 Mode 7 frames are unaffected by
  the changes (they run the TC7 program, see below).
- **Exactness.** TCTEST compares every rendered frame with `ppu_render_565`: 0 differing
  words on all frames of every version. Also checked with the final version: 56 random
  frames (all modes, windows, colour math, banded re-runs) through the DSP, and the host model
  on all 246 snapshots plus 400 random frames (`build/tc_snap`, `build/tc_rand`).
- **Profiles.** Hatari's DSP profiler (`dspprofile`), summed by routine through the assembler
  listing (`scripts/tc_bench/`, see its README). Pitfalls that cost time are in the last section.

## Baseline: where 303 ms went (tile frames, ms a frame)

| Routine | ms | What |
|---|---:|---|
| `tx_isr` + vector | 41 | one SSI interrupt per output word, 24 clocks each |
| layer rows: `sliver`, `step_row`, `dsl_draw`, `px_*`, `bg_line`, `sl_*` | ~117 | per-sliver bookkeeping, 33 slivers a layer row |
| sprites: `obj_line`, `sprite`, `obj_sliver`, ... | ~32 | a scan of every OAM entry each row, a 272-word clear |
| `row`, `col_lut`, `col_sub`, `merge` | ~50 | line clears, colour lookups, layer merges |
| `ctab_check` + `tchlook` | 10 average, 330 in 7 frames | fixed-colour table rebuilds |

The per-sliver cost was 200 clocks (about 100 instructions) for a clear row and a full block
rebuild costs 672 clocks; only 4.5% of the tile rows are mixed (56% clear, 40% solid), so
nearly all of it was bookkeeping, not pixels.

## What changed (`falcon/dsp/tc.asm`; each step exact; means over the 222 frames)

| Version | Change | ms | vs base |
|---|---|---:|---:|
| base | | 302.8 | 1.00x |
| v1 | `step_f4`: a specialised loop for 4 bpp layers drawn straight into the line (address stepped by AGU, clear rows two instructions, solid rows without a test, flipped/mixed slivers by the old code) | 283.7 | 1.07x |
| v2 | `rb_all`: all 33 slivers of a block built in one loop (map words read sequentially; the row's page lookup inline) | 265.2 | 1.14x |
| v3 | SUB cleared only when a layer or the colour math uses it; `col_lut` two pixels an iteration | 258.1 | 1.17x |
| v4 | `bg_line`'s results kept per layer between rows (22 variables, restored instead of recomputed) | 253.0 | 1.20x |
| v5 | `apply`: INIDISP, S_SUB, S_MATH, S_FIXED no longer mark the layers dirty (a per-row fixed-colour gradient rebuilt every layer's records every row) | 250.0 | 1.21x |
| v6 | `ctab_check`: when only the fixed colour changed, only the sources with colour math rebuild | 243.1 | 1.25x |
| v7 | `ctab_math`: the table entry loop with `tchlook` unrolled (6x fewer clocks an entry) | 235.6 | 1.29x |
| v8 | sprite rows outside the segment's y band skip `obj_line` | 228.8 | 1.32x |

By base time: frames under 180 ms (63, at the output floor) 159.9 -> 156.2 ms (1.02x);
180-300 ms (46) 268.5 -> 192.2 ms (1.40x); 300-450 ms (105) 354.1 -> 273.6 ms (1.29x); over
450 ms (8, the fixed-colour gradient frames) 951 -> 423 ms (2.25x).

## Why not 2x

1. **The output floor.** 57,344 words over the SSI at 25.175 MHz: about 146 ms, 154 ms measured.
   63 of the 222 tile frames (28%) are at it. Even if every other frame took zero compute, the
   mean could not fall below (63 x 156 + 159 x 154) / 222, about 1.97x of the baseline; with
   the ISR and the drawing that a frame really needs the practical bound is far lower.
2. **The SSI interrupt is 41 ms a frame and stays.** It costs one interrupt per word (jsr 6,
   `jset` 6, `movep` 4, `bset` 4, `rti` 4 clocks). A fast interrupt without the "already sent"
   check would halve it but overruns the ring in compute-bound frames (the DSP is slower than
   the wire there), sending stale words. Every safe variant I found needs the producer to run
   ahead of the reader, which `pixel`/`col_sub` (60-100 clocks a pixel, against 80 clocks a
   word on the wire) cannot promise. Not tried on hardware.
3. **What is left is spread thin.** After v8 the compute-bound frames (159 frames, mean
   about 250 ms) spend it in the ISR (41), sprites (about 25), 4 bpp layer draw (`step_f4` 15),
   `col_sub` (12; 80 in Mode 7 frames), the line clears and lookups (about 24), the 2 bpp
   BG3 path (`step_row`, `dsl_draw`, about 19), merges (10), the block rebuild (about 16).
   No single item is above 10%.
4. **The DSP has 512 words of internal program RAM, and it is full.** Every hot loop must
   fit there (external fetches cost 2 clocks more where they also touch external data); the
   hot region ends at P:$01F7. Each new fast path needs an older one moved out.

## Room, and what it cost (a design constraint for the next change)

- The program's external space is P:$0200-$0FFF: the hot code's load image and the ring
  start at P:$1000. `tc.asm` had no guard: the first larger change ran the program into the
  image and the DSP executed ring words (`$000001`, an illegal instruction). A guard now
  fails the build past P:$1000; the TC7 program (its `TUF` table is Y:$0C00) has one at
  P:$0C00. The TC7 build only ends at P:$0BFC: any shared routine that grows must be made
  conditional (`if TC7==0`) as `ctab_check`, the band code and the sprite routines are.
- **Cold code moved out**: X:$0C64-$0CFF (= P:$4C64, unused X memory, 12 words left), and the
  last two page slots (P:$7E00-$7FFF). The tile program therefore keeps **72 page slots**
  (`TC_SLOTS_TILE`, `src/ppu_tc.h`); TC7 still uses 74. In the 246 snapshots one frame
  needed 74 and two 73 slots; they now go in bands on the CPU side like the one that needed
  78 (`plan_rows` in `src/smkx_tc.c`; TCTEST leaves such frames out, so they are not among
  the compared frames, and I did not run them in the game). Host changes: `src/smkx_tc.c`,
  `src/tctest.c`, `tools/tc_rand.c`.

## Left, with estimates (static, not measured)

| Item | ms a frame | Note |
|---|---:|---|
| sprite scan: one packed word a sprite (ny, h, x), dy in 9 instructions, no branch; off-screen sprites dropped from the list | 6-9 | changes SPK's layout; `spr_geom` decodes it |
| 2 bpp (BG3) fast loop like `step_f4` | 6-8 | needs 60+ hot words: evict `sl_f4` and `px_dmax` |
| `rb_all`: fewer branches (six `jclr` a sliver) | 2-3 | most of its time is in scrolling-HDMA frames |
| merge loop 5 -> 4 instructions (X/Y alternation) | 2 | |
| `col_sub`: fewer instructions a pixel | 4 (Mode 7: 30) | |
| ISR | 0-25 | only with a producer guarantee; risky |

Together 20-30 ms, i.e. about 1.45x overall. Output-floor frames stay.

## Other ways to more than 1.5x, none measured here

- **The planar/8-bit path for frames without colour math.** The planar kernel took 165-206
  ms on field frames and is exact when no colour math, window or per-line CGRAM is used
  (71% of the demo's frames). A picture-by-picture switch between it and true colour needs
  a Videl mode change per picture, which I cannot test without a real Falcon.
- **Sending only changed rows.** `BAND` draws and sends rows [first, last). A row-level
  change test would skip static rows of menus and dialogues; scrolling scenes gain nothing.
- The CPU side becomes the limit once a picture takes about 150 ms; a faster kernel alone
  will not double the game's picture rate (next section).

## The CPU side (measured 2026-09-29; patched Hatari 3eea7b0, emulated)

The earlier "planning about 109 ms a picture" was the runtime's wall-clock `plan` counter, not
CPU time: the DSP renderer's planning runs as a thread in the game's idle time, so the counter
includes the game's NMI and main loop. A CPU profile (`profile on` from a `--cmd-fifo`
breakpoint pair on `smkx_ppu_plan`, 100 pictures, tile scene, frames about $CE4-$12C0):

| per picture, CPU | ms |
|---|---:|
| `need_span_tc` (page needs of the visible map cells, about 1400 `mark_f` a picture) | 11.7 |
| `dsp_plan` itself (table clears, the stream: 256 CGRAM words, changes, segments) | 8.9 |
| `replay` (register/HDMA change log) | 4.2 |
| `plan_row`, `close_span`, `plan_rows`, HDMA counter | 3.9 |
| `compose` (the frame's inputs) | 4.2 |
| upload (`slots`) and send | 5.4 |
| **total** | **about 38** |

The game itself holds **50 fps in almost every scene** of a 20,000-frame run (two scenes 38 and
42 fps, NMI 9-10 ms a frame). The picture rate is 2-4 a second, one per 12-26 VBLs; in scenes
where the game's NMI takes 5-10 ms a frame the render thread gets only the rest, so the
wall-clock `plan` reads 90-370 ms (one starved scene: 2.8 s, 0.3 pictures a second). Those
scenes are limited by the CPU, not the DSP: a faster or partial DSP frame does not help
them; cheaper planning does.

### Cheaper planning (2026-09-29; same Hatari, emulated)

Measured with PLANBENCH (`src/planbench.c`, `scripts/tc_bench/planbench.py`): the 68030 plans 20 real
frames (four runs of five consecutive snapshots, 15 game frames apart, from different scenes) as the
runtime does, with the pages written since the previous frame as `vdirty`; the Hatari CPU profile of
the timed part is summed by routine. Deterministic: the same frames in every build. (Profiling a
fixed picture number of the running game does not compare builds: the pictures land on other game
frames when the timing changes.)

| Planner | ms of CPU a picture |
|---|---:|
| before (`d83dbf5`) | 27.9 |
| after | 15.2 |

What changed (`src/ppu_dsp_plan.c`, `src/snes_ppu.c`, `src/smkx_tc.c`; the plan is identical, see
below):

- **Page needs as bits.** A layer's map cells set bits (character pages from its chrbase, map pages);
  `flush_chr` marks each distinct page once, instead of a read-modify-write of `need`/`fmt` per
  cell. A whole 32-entry map block takes its cached page bits (`block_bits`, valid until VRAM is
  written), 77% of the cells in the demo's frames. The per-tile-offset loop is tight enough for the
  68030's 256-byte instruction cache (the old one missed on nearly every instruction).
- **`replay`** (the register/HDMA change log walk) jumps over lines with no event, and copies its
  start state by longs, not bytes.
- **Sprites:** one pass over the 128 OAM entries a segment (the palettes for the sub-screen
  relocation are read only when it runs); the sprite pages as bits.
- Smaller: CGRAM and Mode 7 zero words filled in loops, table clears by longs, block rebuild split
  for 8x8 and 16x16 tiles, the stream copy unrolled.

**Exactness.** `build/tc_snap rom.sfc -planhash SNAP.BIN...` hashes `dsp_plan`'s whole output
(stream words, page needs, formats, sprite pages, row kinds) per frame, whole and in four bands,
with the block cache compared and with it trusted (`vdirty`); the hashes are identical to the old
planner's on all 2,530 snapshots (2,284 consecutive ones 15 game frames apart, 246 250 apart), and the
trusted and compared runs agree. The renderer/model check (`tc_snap`) still has 0 differing pixels.

**Left** (after the change: `need_span_tc` 5.5 ms, `dsp_plan` 3.8, `replay` 2.2, block rebuilds
1.8): a scene with per-line HDMA (scroll wobble) costs `replay`, `plan_row`, `layer_config` and
`reg_write` about 30 ms by themselves; that is one `update()` per line. A two-level block cache (the
union of a map page's eight rows) and an `update()` that skips unchanged registers would be next.

### End to end: the picture rate did not move (same run, 20,000 game frames)

| | before | after (cheaper planning) |
|---|---:|---:|
| pictures a second, whole run | 3.03 | 3.09 |
| wall-clock `plan` a picture | 105.9 ms | 83.8 ms |

By 1,000-frame slices the picture rate changes by -3% to +14% (mostly 0 to +5%) while the plan wall time falls 5-45%. The
picture rate is not limited by planning: the runtime asks for the next plan so that it ends when the
DSP is done (`smkx_render_poll`), so a picture takes as long as the DSP frame (12-14 VBLs, 240-280 ms
in-game against 229 ms for the kernel alone) plus upload and send. The cheaper planning frees CPU for
the game in busy scenes and lets the plan finish earlier; it does not shorten the picture.
The one scene that is limited by the CPU is frames 15,000-16,000 of the run (0.34 pictures a second,
plan wall 2.9 s, unchanged by the planner): the game's own code leaves the render thread almost no time.

### The starved stretches are the game, not the renderer (CPU profiles, 100 NMIs each; same Hatari, emulated)

- **Frames about 15,000-15,400** (33 fps, NMI 17 ms of every 20): a loading transition. The game's own
  work (WRAM and I/O port emulation, LZ decompression, DMA, the SPC700) takes 33 ms per NMI; no routine
  is above 7%, the planner is under 5%. The renderer gets what is left.
- **Frames about 15,400-17,200** (the game at 50 fps, 0.0-0.3 pictures a second): CT's sound upload
  routine at `$C70192` polls port `$2140` at `$C70A71` for the SPC's echo, gives up after $400 polls and
  starts the handshake again: 7,520 polls in 100 NMIs, two answered. Half of the CPU goes to that
  (`spc_run` 28%, `timers` 9%, `rt_io_read`/`smkx_apu_read`/`rt_iorb` 14%), and the render thread runs
  only when the game idles (the marks in `tools/smkx/marks.txt` do not cover this loop) or by the VBL's
  RWAIT fallback. A run built with a frame limit inside the stretch does not stop there: it waits for
  the pending render and stops at frame 17,259. So the picture freezes for about 40 s of game time in
  the attract demo. Cause not yet found: the SPC700 interpreter (`src/spc700.c`, 4 SPC cycles a port
  access, 1/60 s a frame) does not answer the handshake within $400 polls; on the SNES the driver
  answers within a few. An idle mark in that loop would give the renderer the time; fixing the
  answer is the real cure. Neither is done.

### Partial rendering: measured, and not worth building (2026-09-29)

A host test (a modified `tc_snap`, not kept in the tree) decides per row whether a picture can leave
the row as it was in the buffer being drawn, from the PPU state alone: the registers that the renderer
reads, the scroll, the CGRAM blocks the row's palettes name, the sprites that intersect the row
(entries and the tile rows they read), and the VRAM units (32 words) written since, against the
map words and character data the row's layers can read (a whole map row block; offset-per-tile
modes: all of the map). Checked on the 2,148 pairs of consecutive snapshots (15 game frames apart): **no
row the test calls clean differs in pixels** (0 wrong rows), so it is safe.

| | single buffer (against the last picture) | double buffer (against the one before) |
|---|---:|---:|
| rows identical in pixels (an upper bound) | 56.4% | 52.6% |
| rows the test calls clean | 36.5% | 33.0% |
| pictures with nothing to draw (bound: 35.8% of pictures) | 28.7% | 24% |
| changed pictures: rows still drawn (bound) | 89% (62%) | 88.5% (63%) |

So most of the unchanged rows are pictures that do not change at all (a static screen, a wait);
in a changing picture the cheap test leaves about 89% of the rows to draw, and 80% of the changed
pictures need more than 90% of them. Skipping the unchanged pictures does not raise the rate at which
what is on screen changes (a moving scene has none); it saves work and shortens the wait after a
static period. Rows joined into runs (gaps under 8 rows) come to one run in 90% of the changed
pictures, so the number of runs is not the problem; the test's blind spots are: registers that change
without changing the row (TM, windows, colour math by HDMA, 37k rows) and VRAM units written that
the row does not use. A perfect test would draw 62-63% of the rows of a changed picture; a test that
is cheap enough for the 68030 gets about 89%. With the double buffer's stale rows, the per-run
host-port cost (stream, 256 CGRAM words) and the risk of a stale row on screen, that is about
+12% on changing pictures. Not done.

### Partial rendering, built (2026-09-29; patched Hatari 3eea7b0, emulated)

Built anyway (`CTX_PARTIAL=2`, default in the render context): `dsp_rows` (src/ppu_dsp_plan.c) finds the rows
that differ from the last committed picture from the PPU state (row versions per screen buffer in
`src/ppu_rows.c`, shared with the host), the runtime draws only those rows (`BAND` prefix in the stream, runs
joined at gaps under 8 rows, at most 3 runs), and a picture that changes no row is not drawn (the newest
buffer stays on screen, or an older buffer is presented). `dsp_rows` records the register/HDMA changes once
and `dsp_plan` replays that record, so the two passes agree even though the HDMA tables are live WRAM. The
render thread takes the DSP's finished frame itself (record-window spin, flip with interrupts off), which
shortened the DSP's idle gap between pictures from 67 to 40 ms. When the game leaves the render thread
almost no time (`smkx_cpu_bound`), detection is switched off.

Exactness: the host proof (`tc_snap -rowclean`, `-rowfuzz`, `-rowbuf`: 0 rows called clean that differ) and
an in-game check build (`CTX_PARTIAL_CHECK=2`: every checked picture is also drawn whole by the DSP and
compared; mismatching pictures are dumped by `rt_dump`). Over the demo (20,594 frames): 138 checked
pictures, 0 differences. The check build renders so slowly that it checks few pictures in the starved
stretches; the third-opinion CPU renderer differs from both DSP renders alike (live inputs), so it is not a
verdict on either.

Picture rate, 20,000 frames, same Hatari, emulated (two runs of each, the game's timing varies run to
run): whole pictures 3.09 (before planner work: see above), previous partial build 3.36, this build 3.29
pictures a second; 1.4 pictures a second skipped as unchanged. Rows a drawn picture: 223 of 224. So the
detection is exact and cheap enough but changing pictures still need nearly every row (as measured above), and
the skips fall in static scenes where the rate did not matter. **It does not raise the rate; the DSP frame
(12-14 VBLs) still sets it.** `dsp_rows` costs about 12 ms of CPU a picture. What is left for the rate is the
DSP kernel itself, not the transfer.

### The next picture's window built while the DSP draws (2026-09-29; same Hatari, emulated)

The DSP's idle time between two pictures (host-port wait, DSP profile over 140 renders in the frames
about 4,000-6,400, no skips) was 43 ms a picture, of which the CPU's send took 25: the pages (`slots`,
8.5 ms) and the stream (16 ms over the host port) were made only after the DSP's frame was done. Now
`smkx_ppu_plan` builds the picture's whole SSI window (pages, BAND/FRAME stream, padding, SSIOFF) into one of
two buffers (`pre_build`, `SMKX_PPU_PRE`), with the slot bookkeeping as if sent; `smkx_ppu_send` waits for
the last window, does the SSION handshake and starts the DMA: 0.25 ms of CPU instead of 25. A picture that is
planned but not sent (the render copy of VRAM changed, the other program was loaded, the CPU drew it) puts
the bookkeeping back (`SlotState`, including the dirty marks that `take_dirty` consumed). Every single-part
picture now goes by SSI DMA, not only those with pages. Two things found on the way: a spin on the previous
window inside `pre_build` cost 25-51 ms of CPU a picture (the profile showed it as `smkx_ppu_plan` self time
at the `rt_ssi_play_done` call; two windows fixed it); and the check build's whole-picture comparison has to
run after the picture was sent (`smkx_ppu_send` wrapper), so that it checks the pre-built path.

Exactness: check build `CTX_PARTIAL_CHECK=2` (drawn picture against the same picture drawn whole by the
DSP), whole demo, 20,000 frames: 151 pictures, 0 differences, 57 of 58 pictures by the pre-built window, none
put back. The CPU reference of `CTX_PARTIAL_CHECK=1` differs in some pictures (rows from about 99 down) with
and without the pre-built window (5 of 35 without, 4 of 35 with): the same pictures, so it is not the
window. The CPU renders a picture from the game's live HDMA tables at the time it runs, the DSP plan
at plan time; TCTEST (real and random frames) is exact.

Picture rate (20,000 frames, same Hatari): 3.29 before, **3.35** with the window (frames 4,000-8,000 4.12,
243 ms a picture: the DSP frame itself). So the rate is limited by the DSP where the game leaves the
CPU time (about 4 a second, 230-250 ms), and by the game elsewhere: a CPU profile of one of the slow stretches
(60 pictures from the 600th plan) is 74 ms `spc_run`, 22 `timers`, 13 `rt_io_read`, 12 `smkx_apu_read` a picture
against 18 for `smkx_ppu_plan` and 3 for send: the game's SPC700 and I/O emulation, not the renderer, takes the
CPU. What is left of the DSP's idle time there is the DMA of the pages and the stream (2.5 us a slot),
which needs the DSP to listen while it draws (a receive interrupt into a buffer: no room in P/X/Y, see
"Room"); it is not done.

### Where the DSP still idles, and why more parallelism is not there (2026-09-29; same Hatari, emulated)

Counters (`PPU_ST_EARLY`, `T_GAP`, `T_GAPE`, `smkx_done_at`): for each picture, whether its plan was ready while
the DSP still drew (early) or the DSP was already done (late), and the time from the frame found done to the
next send (matches the DSP profile's host-port wait: 48.7 ms a picture both ways). 20,000-frame runs:

| | pictures | early | gap, early | gap, late |
|---|---:|---:|---:|---:|
| ask by the mean plan time (as built) | 1,183 | 52% | 15 ms | 82 ms |
| ask by an upper envelope of the plan time (tried, dropped) | 1,172 | 76% | 19 ms | 135 ms |

When the CPU keeps up the DSP idles 15-19 ms: the window's DMA, the handshake and finding the frame done, close
to the floor. The idle time is the pictures whose plan is late, and those are the stretches where the game
leaves the render thread no CPU (100-200 ms gaps); asking earlier turns late pictures into early ones but the
total idle does not move (49 ms both ways), so it was dropped. What would help there is CPU time, and the CPU
goes to the game's SPC700/I/O emulation (74 + 22 ms a picture in the profile above), which this project shares
with the other ports and does not change here.

Runs of one binary are identical (two runs each of the two builds above match in every counter), runs of
different builds differ by up to 30% in picture rate from the first thousand frames on (timing-sensitive game
scheduling), so end-to-end picture rates of builds are not comparable to a few percent: 2.84-3.35 a second
for builds that differ only in counters. Compare a build's own gap and early counters, or use PLANBENCH.

### The SPC700 interpreter's struct order (2026-09-29; same Hatari, emulated)

`Spc` (src/spc700.h) began with the 64 KiB RAM, so every field the interpreter touches (`pc`, `cycles`, `psw`, ...)
sat at a 32-bit displacement, an extra instruction word on each access: the step counter's `addq.l #1` cost
33 cycles (2,415,992 cycles over 72,640 hits). With the state first and the RAM at offset 58 the fields take
16-bit displacements and the RAM an 8-bit one: the same instruction 22.8 cycles. `spc_run` plus `timers` came to
108 cycles an SPC instruction before, 100 after (CPU profiles of 60 pictures from the 600th plan of two builds,
`scripts/tc_bench/spcrate.py`): 7%. The host SPC test (`make test-spc`) prints the same, and a 20,000-frame game
run ends with the driver in the same state. The interpreter is 100 cycles an instruction in the dispatch
itself, so a larger gain needs a different dispatch (threaded, registers in locals), a change to the
shared file that every port would take over (`src/spc700.h` is the only file changed here). Other ports: the
field order costs nothing to take over.

## For the other ports (they share the approach, not this kernel)

Entries for their `analysis/porting-notes.md` were not written: the other checkouts are not
in this session. What transfers:

- **Instruction costs in the patched Hatari** (units: 32 MHz clocks, from profiles): most
  instructions 2; a two-word instruction (long immediate, long absolute) 4; every `jXX`,
  taken or not, and `jsr` 6; `rts`/`rti` 4; `movem p:` 6, indexed `(Rn+Nn)` +1 instruction
  cycle. A do-loop needs no branch cycles. Per-pixel branches dominate small loops.
- **`do label`: the label is the instruction AFTER the loop.** A label on the last instruction
  runs the loop one instruction short. Jumping to the last instruction is fine.
- **Bit tests on memory (`jclr #n,x:addr`) and `do x:addr` need a short address (< $40)**;
  load the variable and test the register instead. `(Rn+Nn)` and `(Rn)+Nn` need the same n.
- **P memory is one 32K RAM with X at P:$4000+ and Y at P:$0000+:** unused X words are code
  space, and a program that grows past its Y tables' start executes its own data. Add a
  guard per build.
- **Profiling.** `hatari-debug dspprofile on` through `--cmd-fifo` restarts the profile at
  every further command (each `DebugUI_ParseLine` calls `DebugDsp_SetDebugging`) and needs a
  debugger entry to be processed. What works: `--parse` a file with `dspbreak pc=<end
  marker> :once :file <script>` (the script does `dspprofile save`) and a second breakpoint
  at the render entry whose script does `dspprofile on`. Profiling from the start includes the
  boot loader (it shares internal P addresses with the hot code and waits on the host port
  for seconds). `history dsp N` and a `dspbreak` show the PCs before a crash; note that
  history disassembles the memory as it is now.
- **`tc_snap -pack` etc. exist to run frames without the game:** 246 real snapshots plus
  TCTEST make a 4-minute, bit-exact benchmark of a DSP change.

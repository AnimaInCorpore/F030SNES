# DSP list compositor in SCIV

Integrated on 2026-10-01 from F030CT's working-tree compositor (HEAD
`72f2c2e`). The source remains game-independent; SCIV keeps its own 1 MiB
LoROM bus and the runtime's re-entrant NMI handling and NMI-keyed input.

## Rendering path

`SCIVX_LC=1` is the default for eight-plane builds. The renderer first asks
`lc_covers()` whether the captured frame is suitable, before booting a new
DSP program. Suitable frames go through `lc_plan.c` and `smkx_lc.c`: two
scroll-ring pages, two queued jobs, and retained 16-pixel groups. The DSP
composes dirty groups into list buffers; CPU/blitter copies update the ring
and its mirror, and the Videl shows a window into it.

Three DSP kernels are included: `ppulc.bin` (`LCB=1, WIN=0`),
`ppulcw.bin` (`LCB=1, WIN=1`) and `ppulc7.bin` (`LCB=1, M7K=1`). LCW supports SNES layer windows, including
per-row window-edge changes. The Videl window is a separate mechanism:
fine horizontal scrolling and the seventeenth group at the right edge,
composed by a second shifted render. Modes 0/1 with 8x8 tiles, non-ring
horizontal-scroll effects, and bottom forced-blank rows are supported.

The compositor inherits CT's corrected ring-wrap/mirror copies, OAM
snapshot/shift handling, partial blank-row masks, and 93/89 page-slot
layouts. The Videl base is written high/middle/low; plain-buffer flips
clear the ring's HSCROLL and line offset. Synchronous snapshots drain queued
jobs and copy the actual ring window into the existing version-4 snapshot
layout. No snapshot or planar byte layout changed.

Unsupported frames retain the whole-frame DSP, Mode 7, and CPU paths. These
include 16x16 tiles, unsupported register changes, Mode 7 windows/EXTBG,
visible OAM segments, and horizontal scroll varying by row on the selected
ring layer in retained mode. `SCIVX_LC_MATH=1` additionally refuses unsupported colour math and
mid-frame CGRAM changes; a retained index buffer with one palette cannot
represent those effects generally. `SCIVX_LC_MATH=0` explicitly allows the
compositor to leave colour effects out, as in the sibling implementation.

## Scanline splits and stage 1-1 (2026-10-01)

Mode 0/1/7 changes, main-screen layer selection, map/character-base changes,
and vertical-origin changes use a fixed screen plan. Groups are retained
between frames using exact per-row replay configurations, sprite footprints,
and dirty map/character dependencies. Configuration changes redraw the
affected rows; unchanged frames need no composition. Dirty Mode 7 plane
data conservatively redraws its rows. The ordinary scroll-ring optimisation
resumes cold afterwards. Mixed tile/Mode 7 frames use two disjoint row masks and two DSP
programs into one page, presented only when both passes finish. Blank rows
belong to the tile pass. No snapshot or pixel layout changes.

LC7 places its masks at X:$3300, outside the Mode 7 plane, stream, lookup
tables and sprite slots. Skipped rows still advance the Mode 7 segment
stream. The host uploads packed plane pages separately from raw sprite
pages. LCW saves/restores the row mask across sub-screen composition: the
sub pass must not consume the mask needed by the main pass.

Captured gameplay at the initial castle gate (`build/lc/stage1-1.BIN`):
HDMA sets BGMODE=$09 (Mode 1, BG3 priority) and TM=$14 at visible row 0;
TM=$17 at row 31 adds BG1/BG2 to the HUD's BG3/OBJ selection. At row 191,
CGWSEL changes from $30 to $02 with CGADSUB=$42 and TS=$35: BG2 takes
half-subscreen addition in the ground region. The frame-start BGMODE=7
is overwritten before visible rendering; this capture has no visible
Mode 7 rows. It now uses the fixed LCW path, preserving that selection and
the reference renderer's indexed checkerboard blend. This is the existing
eight-plane approximation, not RGB565 colour math or a claim of SNES RGB
pixel accuracy. Other colour effects retain fallback.

Live checking freezes a private copy of planned VRAM. A one-job queue can
still span runtime frames; comparing to the mutable render copy produced
a false report when HUD digits changed after the DSP upload.

SCIV-specific correction to the imported planner: its original colour
guard only inspected the frame-start state after suppressing colour writes.
The exact-colour guard now replays HDMA/IRQ colour registers too, with reason
bits `0x200` (math) and `0x400` (CGRAM). The replay cache includes the guard
policy. Diagnostic/synchronous builds also retain a depth of one when a
snapshot restores the ordinary queue depth.

## Build and diagnosis

```sh
make scivx                         # compositor on, checks off
make scivx SCIVX_LC=0              # existing rendering paths
make scivx SCIVX_LC_CHECK=1         # CPU comparison before presenting; slow
make scivx SCIVX_LC_CHECK=1 SCIVX_LC_CHECK_FROM=6000 # check later gameplay
make scivx SCIVX_LC_DIAG=1          # job log, list and ring-copy checks
make check-lc                     # deterministic host regressions + snapshots
make test-lcd                     # actual DSP kernels in the required patched Hatari
make check-lcd-model LC_SNAPS=DIR  # retained-ring model on captured SCIV frames
make build/lc/lc_why
LC_REFUSE_MATH=1 build/lc/lc_why build/scivx/sciv.sfc DIR/F*.BIN
```

`SCIVX_LC_BLIT_MIN`, `SCIVX_LC_SYNC`, and `SCIVX_LC_ALLPAGES` expose the
sibling's copy threshold, one-job scheduling, and VRAM change-detection
check. `SCIVX_TC=1` disables the list compositor automatically. Build flags
invalidate the TOS immediately, including switches made within one second.
The LCD test pack records its frame-list input in a stamp to avoid reusing
a pack from a different selection. Synthetic fixtures remain under
`build/lc/`, and all ROM-derived files remain untracked.

On this Windows machine the cross-compiler is
`/mingw64/bin/m68k-atari-mintelf-gcc`; use an MSYS2 shell and pass
`CROSS_CC=/mingw64/bin/m68k-atari-mintelf-gcc` and, for host checks,
`CC=/ucrt64/bin/gcc`. Set `DOSBOX` to the installed DOSBox Staging executable.
Headless tests use `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy`.

The run target uses only `../../../F030Arcade/third_party/hatari/build/src/hatari`
(`hatari.exe` on Windows). This session built the pinned patched source
there: `3eea7b07`, CMake/Ninja Release, DSP emulation enabled, SHA256
`600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf`.
The Windows build lacks Unix command FIFOs, so the runner detects program
termination from the event trace and stops/waits for its own PID. The
machine-local build record is F030Arcade's untracked `hatari.md`.

## Initial integration verification

- Host regression: 160 scrolling frames, 106 with windows, with moving
  sprites, changed tile bytes, WRAM HDMA window edges, ring wraps and blank
  rows: zero differing pixels. Unsupported mode/math/CGRAM transitions and
  the SCIV 1 MiB LoROM mapping are checked.
- Copy regression: 20,000 walks, 20,000 ring copies and 1,000 complete jobs:
  zero bad groups; synchronous-depth restoration checked separately.
- Synthetic snapshot planes match both the current renderer and the
  pre-integration reference renderer from Git HEAD.
- Actual LC/LCW kernels in the patched Hatari above: 28 synthetic mask/shift
  cases and 112 cases from eight captured SCIV snapshots: zero differing
  words. The captured pack exercises the kernels independently of whether
  the retained-ring planner covers the scene; it clears colour effects for
  the kernel geometry tests.
- Compositor-on, compositor-off and check/diagnostic builds pass. A live
  compositor checker run completed with no `LCCHK` mismatch report.
- Attract run configured for 6,000 runtime frames reached its frame limit
  without a trap. All 15 sampled snapshots match the reference planes and
  the DSP host model. Four early samples are covered by the retained-ring
  planner; eleven later samples use fallback (one 16x16-tile sample, then
  colour math, and finally colour math plus ring-layer per-row scroll).

These are correctness checks in Hatari, not physical Falcon measurements.
No speed-up is claimed. In particular, the later sampled attract scenes
still use fallback: windows are supported, but colour math and ring-layer
scroll effects remain the coverage work needed for those scenes.

## Scanline extension verification

The later extension passes 185 host frames (106 with windows), including
moving mode/layer boundaries and half-subscreen composition, with zero
wrong pixels. Copy checks pass 20,000 walks, 20,000 copies and 1,000 complete
jobs. Version-4 fixtures match the unchanged reference renderer.

Actual DSP tests in patched Hatari 3eea7b07 pass 60 cases with zero differing
words, including sparse LC7 masks, both passes of a mixed-mode frame,
half-subscreen LCW, and captured stage 1-1. Logs: build/lc/splits-dsp.log.
The captured-frame ring model covers 47 of 51 probe snapshots, with four
fallbacks and zero wrong pixels. Log: build/lc/splits-final.log.

The final scripted live run reaches stage 1-1, checks gameplay from runtime
frame 6000, and completes its configured limit without a trap or LCCHK
mismatch. All 27 snapshot files retained across the two live runs match the reference. Logs:
build/scivx/gfxcheck-stderr.log and build/gfxcheck/snapshot-check.log.
The default SCIVX.TOS is rebuilt with LC=1, LC_MATH=1 and checks disabled.
The subsequent retention correction is documented below.

## Fixed-screen retention correction

The first scanline implementation redrew all 3,584 groups and then copied a
whole list to the page, adding overhead to the old whole-frame DSP path.
This defeated the compositor's main purpose and could make stage 1 slower.
The corrected planner retains fixed-screen groups, replays row configurations
exactly, marks old/new sprite footprints and checks changed 32-byte VRAM
units against each visible map word and character row. The other page only
copies the updates it missed. Runtime pass masks now use these dirty masks
instead of expanding them back to full rows.

On the same saved castle-gate sequence, frames after the initial redraw
need 0–128 groups rather than 3,584. This is a planner work count, not an FPS
measurement. Across the 47 covered probe snapshots: 945 groups composed
per frame on average, down from 2,592; zero wrong pixels. Regression now
checks 233 synthetic frames, including retained moving sprites, changing
split boundaries, map/character updates and stationary frames. Live gameplay
checking again completes with no LCCHK mismatch. No physical Falcon or
measured emulator speed-up claim.

Occlusion-aware dirty filtering is still conservative: changes to a tile
behind an opaque higher-priority tile can mark its group. The DSP already
stops composing lower-priority pixels when the group is fully covered, but
skipping those invalidations also requires accounting for windows, priority,
transparency and sub-screen blending.

## 2026-10-01 — Required CPU/DSP graphics work sharing

The user-defined contract in AGENTS.md is now authoritative:

`raw VRAM -> SSI/DMA -> DSP -> SSI/DMA -> RAM composition result -> CPU walks result -> screen buffer`

The DSP makes the graphics decisions, including decoding, dirty planning,
visibility, priority, windows, effects and composition. The CPU handles only
unavoidable control/transfer setup and walks the returned result to perform
plain draw/copy/clear operations in the screen buffer. Those final writes are
allowed; CPU staging copies, bulk snapshots and graphics planning are to be
eliminated. A composition result may therefore contain data and draw/clear
runs; the host executes them without reconstructing PPU decisions.

The current LC path does not meet this contract: lc_plan.c plans dirty groups
on the CPU; smkx_lc.c snapshots 128 KiB WRAM, walks lists and copies missed
updates between pages; ppu_dsp_plan.c replays PPU state and discovers VRAM
pages; smkx_ppu.c converts/stages VRAM for SSI. The existing C_UNC mask is
already DSP-side pixel coverage, but is not DSP-owned retained dirty planning.
These remain migration work, not verified offload. No new transport kernel
has been retained from the exploratory protocol work in this session.

Occlusion census of twelve saved stationary stage-1 frames: 768 warm dirty
groups, 629 with unchanged reference pixels, 139 with changed pixels. This
oracle uses fully rendered pixels and cannot be substituted for runtime dirty
planning. Most absent background contributions are transparent tiles, not
opaque foreground occlusion. Genuine opaque-hidden contributions average
14 BG1 groups and 433 BG2 groups per capture; these counts do not demonstrate
savings in dirty groups or elapsed time. Thirty-three ground rows use blending,
so both screens must be considered. Captures are separated samples, not a
consecutive-frame gameplay benchmark. Research tools: tools/lc/occlusion_probe.c,
tools/lc/occlusion_ref.c and tools/lc/occlusion_ref.h.

## 2026-10-01 — Stage-1 regression rollback and raw VRAM page DMA

The documented CPU/DSP contract had not changed the executable: CPU-planned
LC remained the default. Same Start-input script, snapshots/checks off, selected
runtime status samples at about NMI 6,050–7,000:

| Path | NMI interval | VBL interval | Completed picture submissions/presentations |
| --- | --- | --- | --- |
| Whole DSP, staged uploads (`gfxbase`) | 6088–6990 | 8003–8905 | 37 whole-frame submissions |
| CPU-planned LC (`gfxperf`) | 6073–6973 | 8251–9151 | 2 LC presentations |
| Whole DSP, direct raw pages (`gfxraw`) | 6089–6990 | 8003–8904 | 36 whole-frame submissions |

These counters describe a scripted stationary scene; they are not measured
moving-gameplay FPS. Comparing fewer LC groups was insufficient: planning,
WRAM snapshots and page copies stalled picture updates. The new raw path is
about the earlier whole-frame cadence, not a demonstrated improvement over
that baseline. No physical Falcon measurement. Emulator for all three:
patched Hatari 3eea7b07, build/src/hatari.exe, v2.6.1-devel Oct 1 2026,
SHA256 600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf.
Logs: build/scivx/gfx{base,perf,raw}-stderr.log; output build/gfxraw/SNAP.BIN.

Makefile now defaults SCIVX_LC=0; experimental LC remains selectable with 1.
The default whole-frame kernels take RAWPAGE ($22) or RAWPLANE ($23): page and
P address on the host port, ready acknowledgement, then 512 raw little-endian
bytes by DMA directly from rt_vram_r, then done acknowledgement. DSP swaps tile
words or packs Mode 7. No staging buffer, CPU packing loop or host page-data
writes in this path. The command stream stays on the host port; output SSI/DMA
writes the framebuffer directly. The final receive slot must not issue another
request, or the following DMA window can lose its first word. Runtime playback
helper is shared with PPUTEST in falcon/ssi_play.s. Experimental LC kernels and
SMKX_PPU_SSI=0 retain host-port page uploads.

Verified 36 actual DSP frames (captured stage 1, tile/window/sub-screen fixtures,
32 generated cases including Mode 7): zero differing words. Scripted 8,500-frame
runtime completed; final snapshot has zero differing words in 28,672 versus
reference. Normal unlimited build rebuilt with LC=0. Kernel/image bytes and
reference planar/snapshot bytes are unchanged in format.

Remaining CPU work is explicit: PPU/HDMA replay, sprite/page discovery, palette
preparation, page-cache bookkeeping and stable render-VRAM synchronisation.
These are not yet DSP-owned. Retained DSP dirty/occlusion planning remains
unimplemented. This change removes the severe LC regression and staging copies;
it does not establish full-speed gameplay or completion of AGENTS.md's target.

DSP code also needs a memory boundary check: external P aliases Y/X data.
Successful assembly can extend code into sprite data at P:$0C00 or window cache
at P:$5500. tools/smkx/dsp_asm.py now rejects those overlaps for ppu.asm;
all normal and experimental kernels must remain inside their allocated regions.

### 25 Hz budget for a stationary stage-1 scene

PPUTEST captured stage frame 0: draw+record 229.45 ms (standalone, no translated
game). Runtime gfxraw NMI 6089–6990: 36 submissions/901 VBLs; mean elapsed phases
392.67 ms planning, 33.74 ms page transfer, 4.57 ms command send. Runtime phases
include game preemption; they are not exclusive CPU costs and cannot be added
to standalone drawing time because pipeline work overlaps. Same patched Hatari
3eea7b07/build/src/hatari.exe identified above, not physical Falcon results.
A 25 Hz update budget is 40 ms. Both preparing full frames and the measured
229.45 ms full DSP redraw/output violate it. No isolated SSI limit inferred.
57,344 output bytes/frame would require 1,433,600 bytes/s at 25 Hz. DSP retention
and dirty-only output must avoid that work for stationary layers; the timer,
flames and idle sprites still animate. Raw transfer alone does not supply this.

## Walker block counts (2026-10-02, host model)

`build/lc/lcd_model` (`LCD_RUNS=1`) counts, per planned frame, the blocks the
walker would issue (runs of a row's mask bits, rows with equal masks merged),
their rows (one strided DMA record each if the DMA record cannot stride) and
groups. Input: the 22 stage-1 snapshots `build/scivx/F*.BIN`, 400 frames apart
(mostly a stationary scene, so these are **not** scrolling-gameplay numbers):
18 covered, 4 slow, 6 cold. Warm frames (12), mean per frame:

- walk (what the DSP composed): ~0-3 blocks, 8-128 rows, 37 groups on average;
- copy (groups the other page missed): 0-7 blocks, up to 256 rows, 1189 groups
  on average, 3584 groups (57 KB) after a cold frame.

Cold frames are one 16-group x 224-row band (224 strided rows). The finding for
direct DSP-to-page DMA: the DSP's own output is small, the page-to-page
catch-up copy is two orders of magnitude larger. Direct DMA only removes the
walk copy; the catch-up copy needs the DSP to write both pages or a single
retained page. Needs gameplay with scrolling to size it properly
(`SCIVX_SNAP_EVERY` smaller, scrolling input script).

### Scrolling gameplay (2026-10-02): the earlier conclusion is reversed

Captured with the same scripted input in the port (Hatari: SCIVX_SNAP_EVERY=1,
Start pulses at NMI 3000/3400/3800/4200, Right held from NMI 4900) and checked
against snes9x frames for that input (stage 1-1 scrolls from about NMI 5200).
The last 300 consecutive snapshots (port frames 6451-6750, game NMI up to
5863), planned in order by `build/lc/lcd_model`:

- 300 covered, 0 slow, 1 cold; warm frames (299), mean per frame:
  **walk 1 block / 218 rows / 3490 groups**; copy 0 blocks / 2 rows / 24 groups.
- So in stage 1-1 scrolling the DSP composes about 97% of the 3584-group window
  every frame (about 56 KB of list data), as one 16-group x 224-row band. The
  plan is the fixed screen plan (HDMA splits BGMODE/TM at rows 0, 31, 191), so
  the scroll-ring retention does not apply; wx/wy stay 0.
- The page-to-page catch-up copy is negligible here. The earlier stationary
  numbers (copy 1189 groups) came from a scene that is not gameplay.

For direct DSP-to-page DMA this means: the walk is the whole cost, and it is one
band of 224 rows of 16 groups (256 bytes) with a 512-byte destination stride,
i.e. 224 strided records or one record if the page were laid out with the
window's row stride equal to its width (the ring's 32-group row stride is what
makes the destination non-contiguous). Retention that survives scrolling in the
fixed plan is the larger lever.

### How much a hardware scroll could reuse (2026-10-02, `tools/lc/rigid_probe.c`)

`make build/lc/rigid_probe`, then `build/lc/rigid_probe rom.sfc F*.BIN` on the
same 400 consecutive scrolling frames (399 pairs, reference renderer, colour
math off). Per pair it counts the 16-pixel screen groups of the new picture
that equal the previous picture, pixel for pixel: unshifted; moved by one
global shift (-16..16 px, the Videl's whole-picture scroll); moved by one shift
per band (rows 0-30, 31-190, 191-223, the HDMA splits). Mean of 3584 groups:

| reused from the previous frame | groups | share |
|---|---|---|
| no shift (plain retention) | 1986 | 55% |
| one global shift | 2493 | 70% |
| one shift per band | 2895 | 81% |

The best global shift was 0 px in 124 pairs, -1 in 196, -2 in 79 (the port
drops frames, so a pair is one or two game frames; at a full 50 fps the shifts
are smaller and the reuse higher). The planner composes 3490 groups per frame
(above), so even plain retention would remove more than half of them. The
groups that do not match under a band shift (about 19%) are sprites,
parallax layers and animated tiles. Upper bound: pixel-exact, ignores the
cost of moving retained groups.

### Split ring prototype (2026-10-02, `lc_split_ring`, host model only)

Why the fixed plan recomposes everything: an HDMA split of BGMODE/TM (stage 1-1:
rows 0, 31, 191) sets `fixed`, and the fixed plan retains groups in screen
coordinates, which a scroll invalidates (a row whose replayed registers,
including every layer's scroll, differ is composed whole).

`lc_split_ring = 1` (host: `LC_SPLIT=1 build/lc/lcd_model ...`; default 0)
keeps the ring for such frames: `adopt_anchor()` takes the most common visible
row configuration (registers, vertical scrolls, the ring layer's horizontal
scroll) as the frame's, so the ring logic sees an ordinary frame; the rows
that differ ("odd": the HUD, the colour band) are composed whenever the ring
scrolled, VRAM or OAM changed or their own registers did, and retained
otherwise. Same 400 scrolling frames: 400 covered, **0 wrong pixels**; mean
composed groups per frame **1662 (was 3514)**, copied 280; walk 17 blocks /
274 rows. The remainder is probably mostly the odd rows (about 64 HUD and
colour-band rows, up to 1088 groups, every frame the ring scrolls, because the
Videl shifts them too) plus sprites and the parallax layer's cells; the split
was not measured separately.

Not done: the runtime (src/smkx_lc.c, the DSP kernels) renders split frames only
through the fixed path (rowkind, replay per row); the shifted render of a split
frame, `lc_covers()` (it does not adopt the anchor) and the planner on the
Falcon are untouched, so the flag is off everywhere but the host model.

### Game rate and presentation rate in stage 1 (2026-10-02, Hatari, default build)

Default build (`SCIVX_LC=0`: whole-frame DSP, raw page DMA), no snapshots, same
scripted input (Start pulses, Right held from NMI 4900), 9,000 frames, patched
Hatari v2.6.1-devel (Sep 22 2026) `--fast`; rates from the status lines
(game NMIs and renders per VBL, scaled to 50 VBL/s; emulated time, not a
physical Falcon). From the intro to the end of the run, in windows of about
300 VBLs: the game NMI rate is 50/s in about half of the windows and 40-46/s in
the others; **presents run at 0.9-5 per second** (4-5/s on the intro and menus,
1-2.3/s from the stage on). End of run: 507 presents in 9,992 VBLs, 7,838 game
NMIs. So the graphics path is at 1-2 fps in stage 1 against the 25+ fps target,
and the game loses 10-20% of its frames in some windows (not attributed:
forced render slices after RWAIT is the suspect).

Attribution A/B (same run, stage 1 only, NMI >= 4900, new `rforced` counter in the
status line = VBLs that forced the render context in after RWAIT starved frames):

| RWAIT | forced VBLs | game NMIs/s | presents/s |
|---|---|---|---|
| 3 (as built) | 22% | 45.6 | 1.45 |
| 12 | 7% | 45.7 | 0.34 |

Forcing the render context in does not cost the game its frames: the game runs at
about 45.6-45.7/s either way, below 50 because the translated game itself needs
more than a VBL on average in stage 1 (emulated). Only the presentation rate
moves with RWAIT. A throttle that protects the game rate by rationing graphics
CPU would therefore not raise the game rate here; it would only lower the
presentation rate further. The graphics rate is limited by the CPU time the
game leaves over, which is why the CPU-side planning has to go (DSP-owned
dirty planning). RWAIT stays 3.

### Kernel switch cost and DSP time by frame (2026-10-02, PPUTEST, Hatari)

`src/pputest.c` now times each kernel boot. `make test-ppu PPUTEST_FRAMES="..."
PPUTEST_RANDOM=2` on six real frames (intro/plain, a mathF frame, three stage-1
scroll frames, one early frame), standalone (no translated game, so no preemption),
patched Hatari v2.6.1-devel (Sep 22 2026), emulated time, 0 differing words:

- **Kernel boot: 15-22 ms** (tile kernel 15-18 ms, window kernel 19-22 ms).
- **Transport baseline: 76 ms** for a blank frame (all rows zero): the 57,344
  output bytes cost that on their own, about 0.75 MB/s.
- **Frame upload (pages the frame needs, after a boot): 21-108 ms**, so the
  cold refill after a switch is of that order, not the 15 ms a page that the
  runtime's elapsed counters suggested (those included game preemption).
- **Render + record per frame**: plain tile frames 190-206 ms; stage-1 frames
  (window kernel: colour math) 204-334 ms; subtract the 76 ms transport for
  the composition: about 114-130 ms (tile), 128-258 ms (window kernel).

A scene change that switches kernels therefore costs about 0.05-0.15 s once
(boot plus refill), small enough to hide in a transition. The earlier elapsed
figures in this file (15.3 ms a page, 151 ms a plan) include the game's frame
logic running inside the render context and are not CPU or DSP cost.

#### What the window kernel costs in stage 1 (same PPUTEST run, planner data from `PLAN_V=1 build/plan_bench`)

| frame | kernel | sub-screen rows | sprites | pages | render+record | minus 76 ms transport |
|---|---|---|---|---|---|---|
| F0000070, F0000100 | tile | 0 | 128 | 23 | 190 ms | 114 ms |
| F0000D9C (fixed-colour math, 1 row) | tile | 0 | 4 | 42 | 206 ms | 130 ms |
| F0001000 | window | 224 | 0 | 60 | 321 ms | 245 ms |
| F0000830 | window | 224 | 7 | 41 | 334 ms | 258 ms |
| F0001A6C (stage 1 gameplay) | window | 33 | 6 | 58 | 204 ms | 128 ms |

A blended row costs about 0.6 ms ((245 - 114) / 224); the 33 blended rows of the
stage-1 ground add about 14-19 ms to the base composition. A kernel specialised
for stage 1 would save at most the window kernel's overhead on top of that
base (roughly 14 ms of 204 ms, about 7%): not worth a kernel of its own. The
base composition (114-128 ms) and the 76 ms output transport are the costs;
only dirty-group output and retention (not kernel specialisation) reduce both.

### Split ring: model and runtime (2026-10-02)

Consecutive scrolling frames (473 snapshots, port frames 6328-6800, from
`SCIVX_SNAP_FROM=6300 SCIVX_SNAP_EVERY=1`; `build/lc/lcd_model`, pixel check
against the reference, all 473 covered, 0 wrong pixels in every mode):

| plan | colour math | groups composed / frame | copied / frame |
|---|---|---|---|
| fixed (flag off) | left out or refused | 3264 | 24 |
| split ring (`LC_SPLIT=1`) | left out | 1721 (-47%) | 205 |
| split ring | refused (runtime default, `LC_REFUSE_MATH=1`) | 2113 (-35%) | 286 |

`lc_split_ring` also covers the colour band now (`cap_fn`: a supported
half-subscreen row is an odd row, not a reason for the fixed plan; `lc_fixed_why`
and `fixed:*` in the model output say why a frame still takes the fixed plan).
Build switch for the Falcon: `SCIVX_LC_SPLIT=1` (with `SCIVX_LC=1`).

**Runtime check (Hatari, actual DSP kernel, `SCIVX_LC=1 SCIVX_LC_CHECK=1
SCIVX_LC_CHECK_FROM=6350`): not yet correct.** Status line slots 20/21 of `lc` count
pictures checked / wrong. Split off: 2 checked, 0 wrong. Split on: 2 checked, 2 wrong;
the first, a cold frame (3808 groups composed, dl 5, S 0x3FE5 near the ring end),
has 1643 wrong groups: 324 / 913 / 406 in rows 0-30 / 31-190 / 191-223 (of 496 /
2560 / 528), so not only the odd rows. The control used the fixed plan (dl 0), so
this is the first time the DSP's shifted render (`ppu_replay_dx`, mirror and ring
wrap) is checked on this scene; the cause is not yet known. Only two pictures
were checked in a run (synchronous check, seconds a picture): thin evidence.
Next: check the same frames with `build/dsp_snap ... -packlc` on the host (the DSP
model against the lists), and the ring-end/mirror handling for S near N.

### The list compositor's DSP cost is per touched row (2026-10-02, LCDTEST, Hatari)

Checks first: the shifted render of the DSP model equals the reference for the
stage-1 scene at dx 0, 5, -245 and 11, with and without colour math
(`build/shift_probe`, 4 frames x 4 shifts, 0 groups differ), and the actual list
compositor kernels reproduce the model on the packed real stage frames
(`make test-lcd LCD_FRAMES="build/scivx/oldcap/F00018B8.BIN ..." SCIVX_LC=1`:
32 frames, 0 differing words). So the Hatari LCCHK failure with the split ring
(above) is not the plan, the model or the kernels at those shifts: it remains open
(the runtime's page, ring-end or mirror handling, or the check).

The same LCDTEST log gives the kernel time of a list frame ("compose+record", Hatari,
standalone, includes the output transport of the list):

| rows touched | groups | ms |
|---|---|---|
| 224 | 224 | 97 |
| 111 | 232 | 60 |
| 49 | 114 | 28 |
| 40 | 640 | 39 |
| 209 | 539 (colour math, LCW) | 189 |
| 224 | 3584 | 238-243 |

A least-squares fit over all 32 frames (kernels mixed, max error 66 ms) gives about
**0.5 ms per touched row + 0.035 ms per group**. The cost is mostly per row: a row
with any group set pays its setup (state replay, tile rows) however few groups it
draws. In the stage-1 scroll frames the split ring touches **191 of 224 rows**
(179 with colour math left out; the fixed plan 207), so on this fit the list path
costs about 170 ms (split, math refused) against 128 ms composition + 76 ms transport
for the whole-frame path. The split ring cuts groups by 35-47% but not rows, so the
list compositor is not faster here until its per-row cost drops (a lighter per-row
setup in the kernel) or the dirty groups fall into far fewer rows. `lcd_model` prints
"rows touched per warm frame" now.

### Where the whole-frame window kernel's time goes (2026-10-02, PPUTEST + `PROF` builds, Hatari)

Window kernel (`WIN=1, LCB=0`, `--define PROF=n`; `PROF` is now overridable, it was
silently ignored before: `@def(PROF)`), four real stage-1 frames
(`build/scivx/oldcap/F00018B8`, `F000191B`, `F00019E3`, `F0001A90`), render + record,
standalone, patched Hatari v2.6.1-devel (Sep 22 2026), emulated, mean of the four:

| build | ms | what is left out |
|---|---|---|
| PROF=0 | 224.3 | nothing |
| 1 | 98.3 | layers (`layer_row`) |
| 2 | 195.8 | sprites (`obj_row`) |
| 4 | 146.7 | composition |
| 7 | 76.5 | all three: the transport floor and the row loop |
| 8 | 153.0 | slivers (the per-tile work) |
| 16 | hung | descriptor work (wrong output by design) |

The 76.5 ms floor equals the blank-frame transport measured earlier. Of the 148 ms
of compute: layers 126 ms (85%), of which the slivers are 71 ms (48% of compute) and
the rest, 55 ms, is per layer and row (setup from the registers, descriptors); sprites
29 ms (19%); composition is not separable (it shrinks when the layers are off).
Per layer and row the non-sliver part is about 0.08 ms (672 layer rows); a sliver
(tile) about 3 us (21.5k tiles). Together with the list compositor's cost (about
0.4 ms fixed per touched row) this says the per-row setup is the target: about 0.25 ms
of every row is layer setup that depends only on registers, which change at a few
rows a frame (HDMA), not every row. Caching it per register change would save up to
about 55 ms of 224 in the whole-frame kernel (25%) and more in the list kernel, where
rows with few groups are dominated by it. It would not reach 25 fps on its own: the
transport (76 ms) and the 71 ms of slivers stay.

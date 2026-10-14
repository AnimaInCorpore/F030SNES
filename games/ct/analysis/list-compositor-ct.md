# The list compositor in CT (2026-10-01)

Ported from F030DKC (which has F030SMW's, and CT's own renderer stack, in it): the DSP composes only the 16-pixel groups of a scroll
ring that changed, the 68030 walks its list into one of two ring pages and points the Videl at a window of it (HSCROLL, line offset).
Everything here is the patched F030Arcade Hatari `v2.6.1-devel (Sep 22 2026)` (compiled Sep 22 2026, 21:37:14), Falcon,
`--cpu-exact`, emulated: no figure is a Falcon's timing. Build switch `CTX_LC` (default **1**: verified below in Hatari, on the demo's scenes,
with the limits below; in the covered scenes it gives the game more time and fewer pictures than the old renderer, see *Asynchronous run*), `CTX_LC_MATH` (default 1: frames with colour math are not drawn by the compositor).

This is the **tile-path compositor** (the DSP composes the changed groups, `src/smkx_lc.c`). It is not F030CT's earlier
blitter renderer (`analysis/list-compositor.md`: sprite and overlay blits from source caches): that exists as a host model and a
Hatari player (`tools/lc/lc_model.c`, `falcon/lcplay.s`) and no game runtime draws with it, in any port. Their re-run is at the end.

## What was taken, what was changed

From `../dkc` (the newest copy, equal to F030SMW's apart from the DKC bits):
`src/lc_plan.c/h`, `src/smkx_lc.c`, `src/snes_ppu.c/h` (+ `ppu_internal.h`), `src/ppu_dsp.h` (`DSP_KERNEL_TC`/`TC7` kept, `DSP_KERNEL_LC` after
them), `falcon/dsp/ppu.asm` (CT's equalled SMW's without the LCB code; the LCB code merged in as it is), `falcon/smkx_lcput.s`,
`falcon/lcdtest.s`, `src/lcdtest.c`, `tools/dsp_snap.c` (`-packlc`; CT's has no true-colour code in it, that is `tc_snap.c`),
`tools/lc/lcd_model.c` and `lc_model.c` (CT's own `tools/lc_model.c` moved to `tools/lc/`; the newer one only adds the `LC_SMW`
rules, off by default). CT's `snes_ppu.c` had no CT-only code: its nine differing lines were older versions of lines DKC/SMW changed
(HDMA run skipping, `ppu_replay_nocg/dx`, `ppu_replay_ram`); the true-colour renderer lives in `ppu_tc.c`/`smkx_tc.c` and was not touched.
Commits: `98bf572` (shared stack), `b167131` (planner, tests), then the runtime.

**Driver (`src/smkx_ppu.c`)**: CT's pipelined driver with its pre-built SSI windows and true-colour kernels stays as it was; added:
boot for `DSP_KERNEL_LC` (`ppulc.bin`), the LC slot mapping, `smkx_ppu_keep/restore(k)` (`kept[4]`), `smkx_ppu_send_lc(mask, list, words)`
(DMA record armed on the list, `DSP_CMD_LCMASK` + 224 masks, then FRAME; the pages of an LC frame go over the host port, not the SSI window).

**Runtime (`src/smkx_video.c`, `falcon/smkx_rt.s`, `smkx_video.s`)**, CT's render context (VBL asks, the work runs at the game's idle
marks) used as DKC's idle-time runtime is:
- `lc_try()` in `compose()` in front of the DSP path, 8-plane builds only (`SMKX_LC`, off with `CTX_TC=1`): -1 not covered (the old paths draw
  it), 0 nothing new or a job on its way, 1 a picture went up; the render start returns 0 (the pictures go up by themselves), a synchronous
  render (snapshots) returns 2 ("on the screen, nothing to flip"; `rt_render` takes the snapshot without `rt_flip`).
- `smkx_render_poll(idle)`: while the compositor is active or busy it asks for another call every VBL (`r |= 2`), except when a snapshot is
  due (`rt_snapdue`), so that the work ends and the snapshot's render drains the jobs (`smkx_lc_set_depth(1)`: one frame planned at a time).
- `rt_write_snapshot` rebuilds the plain screen from the ring (`smkx_lc_window`) when `smkx_lc_active()`; `rt_flip` clears HSCROLL
  (`$ffff8265`) and the line offset (`$ffff820e`) for a plain screen; `smkx_vid_set_window` and `vid_set_base` as DKC's (the base's low byte
  last: Hatari's Videl clears it when the mid/high bytes are written; CT's `vid_set_base` wrote it first).
- Each LC job carries its palette (`smkx_set_palette`: CGRAM and INIDISP into `load_clut8`, plane count 8) so a fade matches its picture. A frame
  that starts in forced blank and has no IRQ write is a window job with the black palette (CT holds forced blank for many frames at scene
  changes; DKC's blank frames are the NMI's own and are skipped): the ring stays, the order with the jobs before it is kept, no snapshot is made
  of it (the planes would not match the blank frame's reference).
- `lc_covers()` + `lc_likely()` before the DSP is booted into the compositor's program (the boot drops the DSP renderer's page cache): a
  scene the compositor refuses (Mode 7, a battle with a split) must not boot it every frame. `lc_wait` holds it off a few frames after a refusal.
- **VRAM**: CT's renderer reads a render copy, `rt_vram_r`, which `rt_vram_sync` brings up to date from `rt_vram` where a frame's inputs are
  fixed (when the VBL asks, or for a synchronous render) and marks the pages whose content differs (`rt_m7_dirty` for the DSP uploads,
  `rt_plan_dirty` for the planners; all 128 pages, `M7DIRTY` and `rt_m7_mark` set `rt_vr_dirty` for any VRAM write). `lc_plan` reads that
  copy (`in->vram = rt_vram_r`) and takes `rt_plan_dirty` as its "pages written" flags, `dsp_plan` clears them, the DSP uploads read the same
  copy: planner, uploads and DSP agree on one VRAM per call; what the game writes in between is flagged for the next job, as in DKC.

**CT-specific findings and changes** (these are in the shared files; `analysis/porting-notes.md` of the other ports has the entries):

1. **Forced blank from an IRQ.** CT writes `INIDISP=$80` from an IRQ at line 212 (every field and cutscene frame; VRAM updates in the blank),
   usually with an OAM segment at the same line. `lc_plan` refused all of those ("per-line changes", "OAM segments"): **9 of 200 sampled
   snapshots were covered**. Now `cap_fn` accepts an `INIDISP` change to blank (and ignores every register from there on, a change back
   to display is refused), OAM segments at or after the blank row are accepted, and the planner keeps which ring groups the last picture
   showed blank (`Prev.blank`): a group that comes into a blank row is composed blank unless it already was, a group that showed blank and
   comes into a visible row is composed. With it **108 of 200**; the host model (below) is exact on all of them. `shifted()`/`snapshot()`
   in `smkx_lc.c` shifted/copied only the OAM segments, not the frame's own OAM that serves the rows above the first segment (latent in the
   copies of the other ports until a segment at line > 0 is accepted).
2. **The LCB DSP build stalled on forced-blank rows**: `blank_row` sends 128 zero words whatever the row's mask is; a partial mask on
   a blank row (a blank row inside the frame, or a frame that starts blank) makes the DSP send more words than the list has, the DMA record
   window ends, the ring fills and the DSP waits in `room`; the next host `put()` never returns. LCDTEST passed pattern 0 (all groups) and hung
   after pattern 1. Fixed: under `LCB`, `blank_row` sends eight zero words per set bit of the row's mask. This is the open bug in
   F030SMAS's notes ("stops answering after an all-backdrop frame with a sparse mask"): CT's `F0000040` (starts in forced blank,
   no page) hangs after pattern 1 with the old kernel and passes all eight patterns with the fix (checked both ways in Hatari, the old kernel
   from `98bf572`). Found with Hatari's debugger through the command FIFO (`dspreg`, `dm`): the DSP's PC was in `room`, ROW 223.
3. **Page slots**: the LC build took X:$3000-$30FF for its masks, which is P:$7000 and cost 16 of the tile program's 93 slots; CT's field
   frames read 82-91 pages (40 of the 108 covered snapshots need more than 77). Masks and variables now live at X:$0500-$05EF (the end
   of the stream's room: the LC build's stream may hold 1,024 words, covered frames need up to 146), so `DSP_LC_SLOTS` is 93.
4. **Colour math is left out of the compositor's pictures**: 32 of the 108 covered snapshots enable it (CT fades with a fixed colour
   subtracted, `CGADSUB` $83/$8F, and puts BG3 on the sub screen, `TS` 4). Default `CTX_LC_MATH=1`: such frames are not covered (76 of 200); `CTX_LC_MATH=0`
   draws them without the math (`lc_refuse_math`, `LC_REFUSE_MATH`).

5. **`put_block` wrote the mirror of a copy block from beyond the mirror** (the same code is in F030DKC and F030SMW). A block of copied groups (the groups the other page missed) walks its source
   pointer down the rows; past the ring's end the pointer is already in the *source page's mirror*, and the destination's mirror read from `src + mdelta`, beyond it. Under the
   window rows past the ring's end (rows 175-223 at S = 10,792) the page the Videl shows was stale or garbage, in every second picture (page B) until the groups were composed again. Found by the in-game snapshots
   (the odd frames of a window scene wrong in the lower rows, the even ones exact), the snapshot reader hid half of it (it read the ring masked for the first group of a row, the Videl reads the mirror: it reads as the
   Videl does now), isolated by `tools/lc/lcput_test.c` (`make check-lc-put`: the Falcon's `smkx_lc.c` included on the host with C stand-ins for the assembly copy routines; F030DKC's and F030SMW's `put_block` fail
   2.8 million of 20,000 random copy blocks' groups, the fixed one none; whole jobs, walk plus copies: 1,000 of 1,000 exact). Fix: a copy recomputes its source pointer from the ring position when the block wraps.
6. **The planners read the HDMA tables from live WRAM while the game runs** (the finding that took longest; same code in F030DKC, F030SMW, F030SMW2, F030SMAS: `snapshot()` in `src/smkx_lc.c`). Pictures that were planned
   while the game ran had bands of 1-16 rows by 1-16 groups that stayed wrong until the scene changed: 3-7 of 10-35 covered snapshots a run, in ten runs of the same scene (a scroll that jumps by 20-44 rows into the ring's
   wrap, the demo's frames ~8,800-9,000), in the asynchronous build and in the one-job-at-a-time one (`CTX_LC_SYNC=1`) alike; the snapshot frames themselves (planned in the VBL with the game stopped), and every run in which only
   snapshot frames were planned (313 of 313 exact), or interrupts were masked during the render calls (375 of 375), were exact, and the host model is exact on every frame sequence from the same game (consecutive,
   or every 2nd to 30th frame), so the planner alone could not show it. Cause: `lc_plan` replays the frame's HDMA tables through the A-bus callback (live `smkx_wram`) and every `dsp_plan` of the job replays them again; a plan
   spans tens of NMIs on the 030, the NMI rewrites the tables: the groups the planner chose and the rows the DSP composed came from different tables, and nothing corrects a picture the planner takes as exact (the
   wrong groups were the copied ones: 76 copies, 64 + 12 wrong groups). Ruled out on the way, each by a run of the same scene: the written-pages flags (0 of 128 pages differing unflagged), the DSP page uploads (always upload), the DSP
   planner's incremental caches (`dsp_plan_reset` before every plan), the blitter and the copy queue (CPU only), the CPU copy (`CTX_LC_DIAG=1` compares every copy block with its source: 168 of 168 exact), the DMA record (a
   sentinel pattern in the list buffer: nothing left unwritten), a torn input copy (interrupts masked around it). Fix: `snapshot()` copies WRAM (128 KB, `smkx_lc_cpu`, a few per cent of a plan) into `wram_snap` and gives the job's
   `PpuInput` a private `PpuBus` over it, so every plan of a job reads one version. With it the scene that failed every time is exact (72 of 72 snapshots, 51 covered), and so are all later runs (table below).
7. **Debug builds**: `CTX_LC_SYNC=1` (one job at a time), `CTX_LC_DIAG=1` (the job log `SMKX LCLOG`, a check of every copy block and of the list buffers, lc statistics 16-18), `CTX_LC_CHECK=1` (every picture compared with the CPU reference of its inputs before it goes up; the first differing jobs are reported with their plan: `LCCHK` lines
   in the run's log), `LC_SHIFTS` in `dsp_snap -packlc` (LCDTEST also runs the frames shifted by a window's fine scroll and by `dl - 256` for the 17th group, as the runtime renders them: 112 more combinations, 0 differing).

## Results

Hatari `v2.6.1-devel (Sep 22 2026)`, emulated. The demo CT plays without input (CT's attract sequence: the pendulum clock and Mode 7 scenes, then credits over
cutscenes, a world map, a battle) is the only game material here; **it is not a play session**: every in-game figure is for those scenes.

**Host and DSP tests**

| check | result |
|---|---|
| `make check-dsp-model` (DSP model vs reference, 200 snapshots) | 0 words differ in all 200 |
| `make check-tc` (true-colour model, 400 random frames) | 0 differ |
| `make test-ppu` (PPUTEST in Hatari: 15 snapshots + 32 random frames + the blank frame, with the merged `ppu.asm`) | 0 differing words in all 47 |
| `make test-lcd` (the compositor's DSP builds in Hatari: 20 snapshots x 8 mask patterns, and the 120 shifted renders (a window's fine scroll, `dl - 256` for the 17th group) = 280 frames; 10 of the 20 snapshots on the tile build `ppulc.bin`, 10 on the window build `ppulcw.bin`; up to 88 pages, streams to 251 words; includes `F0000040`, which starts in forced blank and hung the old kernel after pattern 1, and `F0000182`, a frame without a BG layer) | **0 differing words in 280** (the old LCB kernel: hang on `F0000182`, `F0000E80`, `F00016CF`, `F0000040`) |
| `build/lc/lcd_model` on all 200 snapshots (not consecutive: planned cold or after a jump) | 116 covered, 0 wrong pixels; with colour math refused (the default) 79 |
| `build/lc/lcd_model` on 186 consecutive in-game frames (`build/lc/field2`, frames 6,640-6,840, the throne-room cutscene with a parallax starfield) | 186 covered, 1 cold start, **0 wrong pixels**; mean 1,961 groups composed and 944 copied a frame of 3,808 (max 3,584) |
| `make check-lcd-fuzz` (`tools/lc/lcd_fuzz.c`: one covered frame made into 400-frame sequences, a forced blank from a random row or none every frame, scroll; 7 runs on 6 snapshots) | 0 wrong pixels; the same harness with the blank logic switched off: 296 of 300 frames wrong |
| `make check-lc-model` (CT's blitter-renderer model, synthetic and census, on the current snapshots) | exit 0, 0 wrong pixels |

**In the game, synchronous snapshots** (`scripts/lc_snaps.sh`: the runtime in Hatari writes `F*.BIN` with its own planes; `scripts/lc_check.py` renders each with the
host reference (the planner says which frames the compositor drew; those are compared with the reference rendered without colour math when the build ignores it)
and counts differing words):

| run (frames of the demo, build) | snapshots | covered by the compositor | differing words |
|---|---:|---:|---:|
| 6,640-6,840 every frame (the throne-room cutscene, parallax starfield; `CTX_LC_MATH=0`, jobs planned in the VBL with the game stopped) | 180 | 180 | **0** |
| 4,950-5,250 every frame (the Epoch scene with windows: 121 of the 182 snapshots set window registers, those frames take the LCW build; `CTX_LC_MATH=0`) | 182 | 182 | **0** |
| 8,200-9,300 every 8th frame, **asynchronous default build** (clock and cutscenes, then a scroll across the ring's wrap) | 53 (49 covered, 3 forced blank, 1 refused) | 49 | **0** |
| 3,400-4,700 every 8th frame, `CTX_LC_MATH=0` (credits; the rest is Mode 7 and goes the old way) | 119 (83 covered, 1 forced blank, 35 old renderer) | 83 | **0** (the others equal the full reference too) |
| 8,300-9,800 every 2nd frame, `CTX_LC_SYNC=1`, `CTX_LC_BLIT_MIN=100000` (CPU copies only) | 72 (51 covered) | 51 | **0** |

Before the fix of item 6 the same scene (a scroll that jumps 20-44 rows into the ring's wrap at ~8,800-9,000) failed in each of 14 runs of the builds of that day, 1-7 of 7-39 covered snapshots, 129-833 differing words each,
always in the pictures planned while the game ran (and, for the diagnosis, exact when only snapshot frames were planned: 313 of 313 covered over frames 8,000-10,000, or when interrupts were masked during the render calls: 375 of 375).
The runs above are all after the fix, with the final code (`CTX_LC_DIAG=0`).

**Asynchronous run** (no snapshots, the first 9,700 frames of the demo, `CTX_LC=1` against `CTX_LC=0`, final code, `CTX_LC_MATH=0`, Hatari `v2.6.1-devel (Sep 22 2026)`, emulated): no hang or fallback storm (6 compositor fallbacks,
7 DSP boots, 21 cold starts). Over the whole run: game NMIs per VBL 7,649/9,988 (0.766) with the compositor against 7,744/10,012 (0.773) without; the compositor planned 298 jobs and put 154 pictures up (the rest identical to the one before),
452 whole pictures went the old way (Mode 7 and the other refused scenes), against 1,097 whole pictures without it. In the stretch where the compositor draws (the same game time in both runs, game NMIs 6,200-7,200, the cutscenes and the scroll; the log has a status line every 50 frames): 986 NMIs in 1,050 VBLs with the compositor
(0.94 a VBL) against 925 in 1,140 without (0.81); **35 pictures against 92** (32 compositor pictures and 3 whole ones against 92 whole ones). A 500-NMI part of it (6,500-7,000) shows 0.92 against 0.96 NMIs a VBL and 16 pictures against 36: the NMI
rate is not stable at this resolution, the picture count is (a third to a half). The compositor shows fewer pictures than the old renderer here: a plan takes tens of ms to 0.5 s of 030 time (the field2 counters below are the cheap end) and the render context works only in the game's idle time. Whether that is a gain depends on what is wanted (game speed or picture rate); it is not measured on a Falcon.

**Per frame, in the compositor's own counters** (1/38,400 s ticks, emulated; a plan includes the DSP planner, the page uploads it starts and, since item 6, the copy of WRAM): the asynchronous run above, over all its scenes (21 cold
frames of 3,584 groups among them): 95.6 ms per plan (298 plans), 58.9 ms to issue a picture (walk the list, copy the groups the other page missed; 154 pictures), 9.1 ms to set its window and palette. The cheap end, the earlier
field2 window (186 consecutive frames of the throne-room cutscene, before item 6): 21.9 ms of planning per plan (757 plans, 554 groups composed per plan on average), 21.6 ms to put a picture on the screen (237 pictures), 4 fallbacks.
In `test-lcd` the DSP composes 3,584 groups (a whole frame) in 208 ms and 70 groups in 33 ms (the row floor); an emulated DSP, not a Falcon's.

**The blitter renderer** (`falcon/lcplay.s` playing the stream `tools/lc/lc_model` records, in Hatari; model and player only, no runtime draws with it, in any port): on the newest consecutive window, 180 frames of
`build/lc/ff` (the throne-room cutscene, frames ~6,660-6,840): census 180 of 180 modelled, rasterizer against renderer 0 pixels different, `seq` and `hreal` 0 wrong, the stream played on the host `play` 180 of 180
frames with 0 differing, **in Hatari `check-lc-play` (`Hatari v2.6.1-devel (Sep 22 2026)`, compiled Sep 22 2026, 21:37:14): 180 frames, 0 window checksum mismatches** (543 vertical blanks, every frame overran one); `bench-lc-play`:
23.29 ms an update (all ops; blitter 11.39, restore copies 7.13, data 0.25, loop and Videl 4.38; 43 updates a second), emulated. The runs before (186 and 193 frames of an earlier window): 0 mismatches, 23.87 and 24.53 ms.
`falcon/lcplay.s` is F030SMAS's (the `d6` fix: `d5` was the memory base).

## What is covered

The 200 snapshots (`build/ctx/F*.BIN`) are the demo sampled every ~64 frames over 12,800 frames: the pendulum clock (Mode 2) and Mode 7 scenes first, then the credits and cutscenes
(Mode 0/1), a world map, a battle. By frame range (`build/lc/lc_why`, planned cold, default build = colour math refused):

| frames | snapshots | covered, colour math ignored | covered, default | forced blank only |
|---|---:|---:|---:|---:|
| 0-1023 | 15 | 1 | 1 | 6 |
| 1024-2047 | 16 | 0 | 0 | 0 |
| 2048-3071 | 16 | 0 | 0 | 0 |
| 3072-4095 | 16 | 6 | 6 | 0 |
| 4096-5119 | 16 | 15 | 13 | 0 |
| 5120-6143 | 16 | 11 | 6 | 1 |
| 6144-7167 | 16 | 14 | 10 | 0 |
| 7168-8191 | 16 | 9 | 9 | 0 |
| 8192-9215 | 16 | 16 | 14 | 0 |
| 9216-10239 | 16 | 15 | 11 | 1 |
| 10240-11263 | 16 | 15 | 9 | 1 |
| 11264-12287 | 16 | 6 | 0 | 0 |
| 12288-13311 | 9 | 8 | 0 | 1 |
| all | 200 | **116** | **79** | 10 |

Not covered (default build, by reason, `lc_why` bits): colour math only 37 (`$200`); Mode 2 and Mode 7 with colour math 50 (`$220`, `$230`: the intro's scenes; Mode 7 and Mode 2 are not tile modes the group composer
takes); a battle scene (16x16 tiles, `$042`: BG tile size 16 and an IRQ that splits the screen at line 50 with another map base; `$242`) 12; per-line changes (`$002`) 5. Nine frames were covered before the
IRQ blank and the windows were taken in (4.5%); then 108 (54%); with the windows 116 (58%), 79 (39.5%) without colour math. **Scenes**: the credits text over black, the cutscenes with
the starry sky and the throne room, the Epoch scene (windows), the world-map frames without fades; not: the Mode 7 intro and the pendulum, the battle, anything with a fade by fixed colour.
**Which of CT's scenes the compositor covers outside this demo is not known**: no play session was run.

## Open and unverified

- **Falcon hardware**: HSCROLL in eight planes, the line offset, the hog-mode blitter against the DSP's DMA record (it may starve it), the SSI and the DSP's real speed; everything here is Hatari's. The DSP column of the
  tests is an emulated DSP.
- **Colour math** (default: such frames are not covered). Most of CT's math in the demo is a fixed-colour subtract on the whole screen (fades): that is a palette transform, `smkx_ppu_palette`/`dsp_math_palette`
  already apply it for the old path (`DspPlan.mathpal`); a job could carry that palette. BG3 on the sub screen (translucent boxes) needs the window/sub build; the LCW kernel has the machinery but the planner would have to say
  where. Not done.
- **Mode 7** (the intro, 50 of 200 snapshots) and **16x16 tiles with a split** (battles) stay on the old paths.
- **Scene changes cost**: each change between a covered and a refused frame boots the DSP (a few ms) and drops its page cache; `lc_wait` keeps a refused scene from booting it every frame, but a scene that alternates
  (a fade between two covered pictures with a refused frame in between) will boot twice. The window build (LCW) is a second program: a job that needs it reboots the DSP, and so does the next one that does not.
- **The throttle**: CT's `smkx_render_poll(idle)` stays; the compositor asks every VBL while it is active, the render context runs the work at the game's idle marks (or after RWAIT VBLs), two jobs in flight are the
  back-pressure. Pictures are only as fast as the idle time plus the DSP allow; no pacing against a target frame rate was done.
- The planner's cost (22 ms a plan on the 030 in the parallax cutscene) and the walk (22 ms a picture, mostly the groups the other page missed) are the next CPU costs; the DSP's per-row floor (33 ms for 70 groups) the DSP's.
- **One version of the inputs per job**: WRAM (the HDMA tables) is copied when a plan starts (item 6) and so is OAM (`rt_oam` has no frame-start copy); VRAM is the render copy `rt_vram_r`, which only changes where a frame's
  inputs are fixed. A job planned ahead (two in flight) still uploads its VRAM pages when the DSP is free, which can be a VBL or two later than its plan: the groups it composes then show the newer VRAM, and the next plan, comparing against the
  VRAM of the earlier plan, composes them again (checked only by the runs above, not proven). Other live inputs the planners read were not found; the runs above are exact.
- **Picture rate**: in the covered stretch of the demo the compositor shows a third to a half of the pictures the old renderer does (see the asynchronous run); the planner (95 ms a plan on average here, the largest CPU cost) is the cost to cut; the DSP's time (test-lcd) runs beside it.
- **FFVI**: see `../ff6/analysis/list-compositor-ffvi.md`.

## Reproduce

```sh
make check-dsp-model test-ppu test-lcd check-lcd-model check-lcd-fuzz check-lc-model         # host, DSP, model (needs the snapshots in build/ctx)
scripts/lc_snaps.sh build/lc/field2 6640 6840 1                                                 # consecutive snapshots of the running game (CTX_LC_MATH=0, CTX_LC=0, ... go to make)
LC_REFUSE_MATH=1 scripts/lc_check.py build/lc/field2                                            # which frames the compositor drew, and 0 differing words
make check-lc-play LC_SNAPS=build/lc/field2                                                     # the blitter renderer's stream in Hatari
scripts/lc_shot.py 6700                                                                         # the Videl's picture in Hatari against the snapshot's planes
```

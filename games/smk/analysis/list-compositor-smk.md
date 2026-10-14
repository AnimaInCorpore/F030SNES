# The list compositor in SMK (2026-10-01)

F030SMW's list compositor (itself F030CT's exact DSP tile renderer plus a DSP-managed ring renderer, see F030SMW
`analysis/list-compositor-dsp-plan.md`, `list-compositor-smw.md`, F030DKC `list-compositor-dkc.md`) taken over into
F030SMK, and F030CT's blitter renderer (host model and Hatari player) on SMK's tile-mode frames.
Everything here ran in the patched F030Arcade Hatari `v2.6.1-devel (Sep 22 2026)` (Falcon, TOS 4.04, `--dsp emu`,
`--cpu-exact`, `--fast-forward`) or on the host. **Nothing is a Falcon timing.** Source commits taken over from:
F030SMW `32cc95c` first, then `e3c633f` (windows in the compositor, the render throttle) the same day.

## SMK is different: what the compositor can cover

SMK is a Mode 7 racer. The race (BG modes 0 and 7 mixed per line, per-line matrices, seven IRQ-logged register
writes a frame, DSP-1) is not a compositor frame and keeps SMK's own DSP renderer (four planes, adaptive 16-colour
palette, `falcon/dsp/ppu.asm`) as it is. The compositor needs BG modes 0/1 with 8x8 tiles, one register state per
frame, no windows, no mosaic, one OAM segment, and it draws eight planes whose pixels are CGRAM indices (256 colours
= CGRAM). So it only applies to the screens in the tile modes. From the reference interpreter (`tools/ref65816.c`,
`scripts/tile_snaps.py`: a 1P GP run through title, mode, driver, cup and course select to the race start,
1,306 consecutive frames before the race) and a 40,000-frame attract run (title, demo races):

| frames (1P GP run) | screen | registers | covered by `lc_plan`? |
|---|---|---|---|
| 1-300 | intro / title, logo scrolling | mode 1, BG1-3 + OBJ, W12SEL `$22` with an empty window (WH0 > WH1) | yes (3,584 groups composed every frame: everything scrolls) |
| 301-741 | title with the curtain / window bands | the same, a window `[$40 $BC]` in the middle band by HDMA | yes with the LCW program (windows); first taken over without it: no (per-line changes), 435 frames refused |
| 743-1005 | driver select | mode 0, BG1-4 + OBJ | yes (896 groups a frame) |
| 1007-1306 | cup and course select | mode 1, BG1 + OBJ | yes (about 890 groups a frame) |
| 1307- | race | mode 0 rows + Mode 7 rows, IRQ writes, HDMA | no, by the cheap gate in the runtime |

`lcd_model` on all 1,306 frames: **1,303 covered, 3 refused (forced blank)**, 0 wrong pixels, mean 1,547 groups
composed and 112 copied a frame (of 3,584); without the LCW program (the first take-over) it was 868 covered, 435
of the others refused for the windows. The 441 frames of the windowed title compose 22 groups a frame on
average (a still picture with a window moving over it). Not reached: the results screens (a race does not
finish unattended: nobody steers). Race and attract demo are not covered at all.

## What was taken, and how it sits next to SMK's own PPU code

SMK is the origin of the shared code but its `snes_ppu.c` is an older line (its DSP-stream builder `ppu_dsp.c`
and `ppu_fast.c` use the `ppu_frame_start/line` API and a 16-pixel sample histogram; F030CT's reference replaced
both). Taking the newer `snes_ppu.c` as the single reference would have meant rewriting SMK's race path. So the
shared stack is a **second world** next to it:

- `src/tile/`: F030SMW's copies of `snes_ppu.c/h`, `ppu_dsp.h`, `ppu_dsp_plan.c`, `ppu_tc`, `ppu_rows`, `m7_plan`,
  `lc_plan.c/h`, `smkx_ppu.c/h`, `smkx_lc.c`, `pputest.c`, `lcdtest.c`; SMK's HiROM bus (512 KiB ROM, 2 KiB SRAM) in
  `snes_ppu.c`. New: `smkx_tile.c`, the seam to the runtime. Changed against F030SMW: `smkx_lc.c` takes a runtime
  `sync` argument (a synchronous render finishes the queued jobs, then plans and draws exactly its frame), a VBL
  `smkx_lc_poll`, and a watchdog on the DSP (8 s without an answer ends the compositor). Jobs carry their
  frame's CGRAM and INIDISP (`smkx_set_palette` when the picture goes up) as in F030SMW's version.
- `falcon/dsp/ppu_tile.asm` (F030SMW's `ppu.asm`) in five builds (`--define`): base, Mode 7 (`M7K=1`), windows
  (`WIN=1`), the compositor's group composer (`LCB=1`) and that with the window build in it (`LCB=1 WIN=1`, LCW,
  for frames whose rows have windows; the DSP is reset into it when a job needs it). `tools/smkx/dsp_asm.py` gained `--boot`, `--prog`, `--define`
  (its default mode, SMK's own kernel, is unchanged: `ppu.bin` and `ppu.ext` come out byte for byte the same).
- `tools/tile/`: `dsp_model`, `dsp_snap`, `dsp_rand`, `lc_model` (the blitter renderer's model), `lcd_model`
  (the planner against the reference), `ppu_snap` (eight-plane snapshots), `snap_to_ct.py` (SMK's snapshot
  version 3 -> the shared version 4). `falcon/tile/`: `pputest.s`, `lcdtest.s`, `lcplay.s`; `falcon/smkx_dspboot.s`,
  `falcon/smkx_lcput.s` (runtime).
- **Linking.** The tile objects are linked into one relocatable object and every symbol they define is renamed
  `tl_*` (`objcopy --redefine-syms`) except `tools/tile/interface.txt`: SMK's `ppu_render`, `ppu_init`, ... and the
  tile world's coexist. The tile world meets the runtime only at `smkx_tile_render/poll/active`, the statistics
  and `smkx_clut8`. Host tools are built against `src/tile` first (`-Isrc/tile -Isrc`).

## The runtime (`make smkx SMKX_LC=1`; default 0: none of it is linked or assembled)

`smkx_video.c: smkx_render()` asks `smkx_tile_render()` first. The tile world is entered when a frame passes a
cheap gate (no IRQ-logged register writes, BG mode 0/1 at the frame start: the race fails it without a replay) and
a probe `lc_plan` covers it; a refusal is not asked again for 12 frames. Entering resets the DSP and boots the
LCB program (about 20 ms emulated). From then on `lc_plan` decides every frame; the DSP composes the changed
groups in up to two renders (the second for the window's 17th group), the 68030 walks the list into one of two
ring pages (`falcon/smkx_lcput.s`: movem copies, the blitter in hog mode for big blocks), and `smkx_vid_set_window`
points the Videl at a window of the ring (base with the low byte last, HSCROLL, line offset). A frame it does not
cover goes to SMK's renderer: the DSP is reset again into SMK's program (`smkx_m7_revive`, about 27 ms emulated,
all VRAM pages sent anew) and the Videl goes back to four planes.

- **Videl modes.** `rt_want8` / `rt_vid_mode` (`falcon/smkx_video.s`, applied in the VBL together with the palette:
  `rt_clut8_due` copies the 256 colours, `rt_clut_due` the 16): plane count (`$ffff8266`), line length (`$8210`),
  and **HDB/HDE** move 16 units later in eight planes (as F030SMW's `smkx_video.s`). Checked: with the four-plane
  values the eight-plane picture sat 4 SNES pixels left of the four-plane one and was clipped (screenshot
  comparison); with them both modes' pictures match the reference render in a Hatari screenshot (0.000 mismatch,
  `tools/tile/shot_compare.py`, cup select in eight planes, race in four).
- `rt_render` return codes: SMK already used 0-3; **4**: the ring window is on the Videl already (count a render,
  snapshot), **5**: nothing new. The snapshot writer rebuilds the plain 57,344-byte screen from the ring
  (`smkx_lc_window`) and writes version 4 (SRAM padded to 8 KiB, a `planes` word, the empty palette blocks), so
  `tools/tile/ppu_snap` reads it; four-plane snapshots stay version 3 (`build/ppu_snap`).
- All 128 VRAM pages are marked for the DSP and the planner (`M7DIRTY`, `rt_m7_mark`; SMK marked pages below
  `$4000` for its Mode 7 kernel only). `vid_set_base` writes the low byte last (Hatari's Videl clears it when the
  mid or high byte is written).
- Colours: CGRAM at the frame, widened and scaled by brightness as SMK's palette does (5 bits to 8,
  `(b + 1) / 16`), black in forced blank; a picture brings its job's colours, and a palette change alone makes a
  job (F030SMW's `pal_changed`).
- **Render throttle** (`src/smkx_video.c`, F030SMW's share controller, replacing `SMKX_RENDER_EVERY`): `smkx_render`
  starts a composition only when the renderer's share of the time (35 %, adapted every 25 VBLs between 10 and
  85 %) allows it; the cost of the last start sets the next (`spent * 100 / share`). The loss it watches is the
  game's: VBLs that passed without a SNES frame (SMK's main loop paces the frames; F030SMW counts frame handlers
  still running, which is all that shows there). A DSP frame in flight is polled and never throttled, a snapshot's
  (sync) frame is not either. `SMKX_RENDER=0` renders nothing but the last frame (`check-ref`, timing runs).
- After a stint in the tile world SMK's adaptive palette starts from fresh counts (`have_hist = 0`): the first old-path
  frames pick their 16 colours again; the pictures stay exact against the reference given the palette used.

Diagnostics: `SMKX_HOLD=1` keeps the last picture on the Videl at the frame limit (`run_smkx.py --until holding
--screenshot`), `SMKX_VBLSTAT=1` prints VBL and `_hz_200` counts, the status line has `tile entered/left/refused/
boot failures/blank/probes/boot ticks/revive ticks` and `lc shown/planned/cold/groups/copies/fallbacks`.

## Results

Host (`make check-dsp-model check-lcd-model check-lc-model`, snapshots from the reference interpreter):

- DSP design model against the reference, identity map, eight planes: 326 of 326 snapshots (every 4th tile frame
  up to the race), and all 1,306 consecutive ones: 0 differing words; the planner takes all of them (the race is
  refused: "stream does not fit"). `dsp_rand` 32 random frames: exact.
- `lcd_model` (the compositor's planner, two ring pages, shown picture against the reference): window A (frames
  43-300, the intro) 258 covered, window B (743-1306, driver, cup and course select) 563 covered and 1 forced
  blank, window C (301-741, the title with its windows) 441 covered: **0 wrong pixels**. Window B composes 1,915 groups a frame on average (896 in cup select, 3,584 in
  driver select where all four layers move) and copies 241.

Hatari, programs (`make test-ppu-tile`, `make test-lcd`):

- PPUTILE: 11 real SMK frames (title, intro, window bands, driver, cup select) and 32 random ones, the blank
  frame: **0 differing words**; the DSP takes 78 to 278 ms a frame (render and record, emulated).
- LCDTEST (LCB and LCW builds, 8 mask patterns on each of the same real frames, the window frames on LCW): 88
  frame/mask combinations, **0 differing words**.

Hatari, the game (`SMKX_LC=1 SMKX_SNAP_EVERY=40`, synchronous snapshots, scripted input through the menus):

- 32 eight-plane snapshots (intro, the windowed title, driver select, cup select, with INIDISP `$0E` `$02` `$0C` `$09`
  fade frames): **0 bytes differ** from the reference (`build/tile/ppu_snap`). The 3 four-plane ones (the race
  start, the first race frames right after the compositor was left) pass `build/ppu_snap` as before: planes and
  next palette 0 bytes differ. (Before the LCW program: 19 and 16, the windowed title being four-plane.)
- Screenshot of the Videl against the reference render: 0.000 mismatch, eight planes (cup select) and four planes
  (race).
- `SMKX_LC=0` is the old runtime: before the render throttle was added, the final `SNAP.BIN` of a 1,320-frame
  menu-and-race run was **byte for byte equal** to the one of commit `73f976b` (before this work). With the
  throttle (which also governs SMK's own renderer, see above) pictures are skipped when the game loses frames, so
  such a comparison is no longer meaningful; the 23 synchronous snapshots of a 2,300-frame menu-and-race run
  (every 100 frames, `SMKX_LC=0`) all pass `build/ppu_snap` (planes and next palette exact).
- A watchdog trip (`SMKX_LC_WATCHDOG=10`) leaves the compositor after its first job and the game goes on with SMK's
  renderer: the snapshot at frame 900 passes `build/ppu_snap`.
- An unattended 20,000-frame attract run with the final code (`SMKX_DIAG=0`, `SMKX_SNAP_EVERY=500`, release build with the
  compositor and the throttle, title and demo races, no input): it ran to the end, the compositor was entered 6
  times and left 6 times (title <-> demo race, each way), 1,388 pictures shown, 7 cold starts, 1 forced-blank frame
  handled, no boot failure, no hang, no watchdog trip. The 40 synchronous snapshots of that run: **22 eight-plane
  ones 0 bytes from the reference, 18 four-plane ones (demo race, frames right after a switch) pass `build/ppu_snap`**.
  (The first take-over, without LCW, ran 40,000 frames: 12 entries and exits, 2,793 pictures, 42 and 38 snapshots
  exact.)

Speed, Hatari, emulated, a 1,320-frame menu run (`SMKX_DIAG=0 SMKX_VBLSTAT=1`, same input script; the whole run,
title to the race start, with the render throttle on in both): with `SMKX_LC=1` 173 pictures in 1,650 VBLs (5.2 a
second at 50 Hz) and 0.78 game frames a VBL; with `SMKX_LC=0` 61 pictures in 1,447 VBLs (2.1 a second) and 0.88
game frames a VBL. **The compositor shows 2.5 times as many pictures and slows the game in the menus by about a
ninth** (before the throttle: 1.6 times as many pictures at 0.66 against 0.88: SMK's own path costs the 68030
little, the DSP draws the frame while the game runs; the compositor's planner, DSP planner, page uploads and
walker cost tens of milliseconds a picture). In the intro and driver select everything moves, so it composes 3,584
groups a frame (about as much DSP work as the full-frame renderer): the gain there is exactness in 256 colours,
not speed; the windowed title (22 groups a frame) and the course select are where it saves work. `SMKX_LC` stays 0
by default until it has been run on a Falcon.

## The blitter renderer (F030CT's model and player), `make check-lc-model check-lc-play bench-lc-play`

`tools/tile/lc_model.c` against `src/tile/snes_ppu.c` on the 1,306 consecutive tile-mode frames: **868 frames
modelled, 438 skipped** (435 windows, 3 forced blank); Mode 7 frames are skipped by the model and none is in
this set (the race is not in it). `seq`: 866 updates, wrong pixels 0, wrong planar bytes 0; `blitfuzz` 50,000
pieces: 0 pages wrong. The longest run of consecutive covered frames is **300 frames (the course-select menu)**:
`hreal` 0 wrong Videl words, recorded as an 11 MB stream (465 blitter rows, 212 restore runs, 699 dirty groups a
frame); the host player replays it with 0 frames differing, and `falcon/tile/lcplay.s` plays it on the 030 and
Hatari's blitter: **300 of 300 window checksums match**. `bench-lc-play` (Hatari `v2.6.1-devel (Sep 22 2026)`,
5 passes, no vertical-blank wait): 31.1 ms an update (blitter ops 14.7, restore copies 9.2, data 0.01, loop
and Videl 6.5), emulated. **This is the model and the player only. The CT-style sprite/overlay producer (clean
ring, sprite and tile blits from source caches) exists in no port's game runtime; the runtime's compositor above
is the DSP composing every changed group, not blits.**

## Limits, honestly

- Coverage is the menus: the intro, the title with its windows, driver select, cup and course select. Not the race
  (Mode 7, IRQ writes), not the attract demos. The results screens were not reached.
- Colour math is left out (as in F030SMW): SMK's tile screens have none (`CGWSEL`/`CGADSUB` 0 in all of them), so
  the reference and the compositor agree.
- Asynchronous pictures lag the inputs by the job pipeline (two jobs); exactness is checked in the synchronous
  build only. Fades with a pending job show the palette of the picture on the Videl.
- The blitter renderer's model skips frames with windows (435 of the 1,306), the compositor does not.
- A switch between the two worlds costs a DSP reset, the 512-word bootstrap and the loader (about 20-27 ms
  emulated each) and, going back to SMK's renderer, a re-upload of every VRAM page: fine at scene changes, so a
  refused probe waits 12 frames and a frame the tile world cannot take leaves it at once. A screen that alternated
  between covered and refused frames every few frames would thrash; none does in the runs above.
- HSCROLL and the blitter in eight planes are checked in Hatari only; on a real Falcon the hog-mode blitter may
  starve the DSP's record of the next job, and Hatari's Videl fetch with HSCROLL (one more group a line) is not
  verified there.
- F030SMAS reports an open bug: the LCB program may stop answering after an all-backdrop frame with a sparse mask.
  SMK's only zero-page frames are forced-blank ones, which are never planned; the watchdog above is the safety net
  (not exercised on that bug).

## Reproduce

    make check-dsp-model check-lcd-model check-lc-model     # host
    make test-ppu-tile test-lcd check-lc-play bench-lc-play # Hatari programs
    make smkx SMKX_LC=1 SMKX_SNAP_EVERY=40 SMKX_FRAMES=1400 SMKX_INPUT="300:8000,306:0,350:800,356:0,400:8000,406:0,500:8000,506:0,600:8000,606:0,700:8000,706:0,801:8000,807:0,901:8000,907:0,1265:8000,1271:0,1765:8000"
    python3 scripts/run_smkx.py --fast --timeout 580
    for f in build/smkx/F*.BIN; do build/tile/ppu_snap $f build/smkx/smk.sfc; done   # version 3 ones: build/ppu_snap

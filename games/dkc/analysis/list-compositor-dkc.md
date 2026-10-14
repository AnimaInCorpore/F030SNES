# The list compositor in DKC (2026-10-01)

Ported from F030SMW (`analysis/list-compositor-dsp-plan.md` there): the DSP composes only the 16-pixel
groups of a scroll ring that changed, the 68030 walks its list into one of two ring pages and points the Videl
at a window of it (HSCROLL, line offset). Hatari `v2.6.1-devel (Sep 22 2026)`, Falcon, `--cpu-exact`, release
build (`DKCX_DIAG=0`); emulated, nothing here is a Falcon timing.

## What was taken over

DKC was a generation behind F030SMW, so the port came in two steps (one commit each):

1. **The reference renderer and the DSP stack** (F030CT's, through F030SMW): `snes_ppu.c/h` (colour math, windows,
   replay), `ppu_fast.c`, `ppu.asm` in its builds (tile, Mode 7, windows and sub screen, LCB), `ppu_dsp.h`,
   `ppu_dsp_plan.c`, `ppu_rows`, `ppu_tc`, `smkx_ppu.c/h`, `smkx_m7dsp.s`, `dsp_snap`, `dsp_model`, `dsp_rand`,
   `pputest`. `make test-ppu` (22 frames: the jungle ones and random ones) and the host model: 0 differing words.
   The game draws through the new driver (the planner says which build of the program a frame needs).
2. **The compositor**: `lc_plan.c` (planner), `smkx_lc.c` (jobs planned ahead from an input snapshot, walker, blocks,
   blitter), `smkx_lcput.s`, `ppulc.bin` (LCB), `lcdtest` (`make test-lcd`: 24 frame/mask combinations on
   the jungle frames, 0 differing words), `tools/lc/lcd_model.c` (the planner against the reference on snapshots;
   `make lc-snaps build/lc/lcd_model`).

DKC's runtime renders in the game's idle time (a render context at WAI, preempted by every VBL), so the compositor
hangs on that: `smkx_render_start` plans and pumps the jobs (it returns 0: the pictures go up by themselves), the
VBL's `smkx_render_poll` asks for another call every frame while the compositor is active, a snapshot (`rt_snapdue`)
makes it stop asking so that the work ends and the snapshot's sync render drains the jobs and plans one frame
at a time. A job carries its frame's palette (`smkx_set_palette` when it is shown: fades). Forced-blank frames
(the NMI's own blank) are skipped, not drawn.

## What DKC needed that SMW did not (shared code, also in F030SMW's copy now)

- **Per-line HDMA.** The jungle level has CGRAM gradients on channels 1 and 2 and BG3's horizontal scroll per line on
  channel 3, plus colour math that changes with TS/CGADSUB writes in the frame. The compositor draws indices with
  the frame-start palette and without colour math, so `ppu_replay_nocg` leaves CGRAM, TS, CGWSEL, CGADSUB and
  COLDATA writes out of the replay (the gradient channels then are not even walked), and the planner takes later
  rows that differ only in a layer's *horizontal scroll* (`State.rowh`, `varmask`): such a layer, which is not the
  ring's, is scanned line by line (`nb_mark_rows`) at its old and its new place. A ring layer with per-row scroll,
  windows, mosaic, other modes or other register changes make the frame "not covered" as before.
- **The DSP renders the frame shifted by `dl`**: the BG scroll less `dl` must reach the HDMA writes too, so
  `ppu_replay_dx` subtracts it in the replay itself (the frame's own scroll and every HDMA value); the OAM is
  still shifted by the driver.
- **Sprites in the window's extra group**: a sprite at OAM x 256-271 is drawn by the DSP into the 17th group;
  the planner had dropped it (x >= 256), leaving stale pixels that came into view as the camera moved (2 wrong
  pixels in 267 frames; found by the host model, `LCD_BAD=1`, `LCD_TRACE`). Cells now run from -2 to 34 (the
  window reaches 15 pixels left and right of the screen).
- **Forced blank**: `lc_plan` returns 1 for a frame that is nothing but forced blank (nothing planned, nothing
  forgotten). Taking it as "not covered" restarted the ring cold at every NMI blank.

## Results

- **Host model** (`build/lc/lcd_model` on 267 consecutive frames of Jungle Hijinxs with Right held, frames
  0x1135-0x1260): 235 covered, the 32 others forced blank; **0 wrong pixels**. Mean 2,380 groups composed a frame
  (of 3,808), because BG2, at half the speed of BG1, moves against the ring every frame and its non-blank cells
  are all redrawn: **the compositor saves little in DKC's jungle** as long as it does not know which BG2
  cells BG1 hides (the lc_check estimate of 440 groups did).
- **Falcon, synchronous snapshots** in the level (`DKCX_SNAP_EVERY`): 0 bytes differing from the reference on
  every snapshot taken (levels at frames 3,924-4,500).
- **Falcon, asynchronous**, 3,050-5,150 VBLs of the level: the compositor shows 150 pictures (3.6 a second), the DSP
  alone showed 73 in the same stretch (2.4 a second); the game runs at the same speed (nmi 3,971 against 3,962 at
  frame 5,200). Per frame 25-60 ms of planning, the DSP about 130 ms for a frame that redraws most of the window,
  and only the game's idle time (5-8 ms a frame) to run in.
- Frames the compositor does not cover (logos in mode 5, map and menu screens with per-line changes) go the old way
  (DSP tile program or the CPU), as before.

## Open

- **Which cells are visible**: marking BG2/BG3 cells only where BG1 does not hide them (BG1's per-row opacity, with the
  priority bits) would cut the groups to the estimated 440. The DSP's cost is per row, so it only helps where whole
  rows stay clean.
- The CPU fallback for refused frames takes seconds in the idle-time context; the compositor refuses few frames in a
  level, but any new effect that makes rows differ (windows, mosaic) sends a frame there.
- Other levels (water, mine carts, bonus screens, fades) have not been run.

## Later fixes (2026-10-01)

Windows (the LCW kernel, window rows in the planner), the render throttle (no render interval: the idle-time context is the throttle),
`lc_covers()` and the exact-path plan fix, the `blank_row` DSP hang fix and the private WRAM copy of every plan are in this tree;
the reasons and the numbers are in `games/smw/analysis/list-compositor-dsp-plan.md` ("Step 3", "Later fixes"). Re-checked after each:
synchronous snapshots in the jungle 0 bytes differing (8 of 9; one was a forced-blank frame without a comparison), host model 235 covered, 0 wrong
pixels, `test-lcd` and `test-ppu` 0 differing words.

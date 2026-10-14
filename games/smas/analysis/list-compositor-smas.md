# The DSP renderer and the list compositor in SMAS: what was taken, what was checked

2026-10-01. F030SMAS has no translation yet (`make smasx` stops at "SMAS placement is not
written yet", ROADMAP step 1), so **nothing here has run inside the game and nothing is a Falcon
measurement.** What was done: the tree was brought to the renderer and compositor state of
F030SMW (first its commit `32cc95c`, then `e3c633f`: windows in the compositor and the render throttle), everything that builds without the translated sources builds, and
everything that can be checked without the game was checked. Hatari for every emulator figure:
`v2.6.1-devel (Sep 22 2026)` (the patched F030Arcade build), Falcon, TOS 4.04, `--cpu-exact`,
`--dsp emu`, fast-forward.

## Which frames the checks used

1. **Random frames** (`tools/dsp_rand.c`): flips, 16x16 tiles, mode 0, windows, sub screen,
   Mode 7, mid-frame register writes. No game involved.
2. **Real SMAS frames from a reference run** (`tools/ref65816.c`, new in this tree): the ROM on an
   interpreted 65816 with the runtime's hardware model, writing the renderer's inputs the way the
   runtime's `SNAP.BIN` does (IRQ log, OAM per segment, scroll latches; the Falcon's own frame is
   zero). `tools/ref_tour.py` scripts the pad from the title through the select screen into each of
   the four games (SMB1, The Lost Levels, SMB2, SMB3) and into a level. These are **real game
   frames of the real ROM, but not frames of the translation and not frames the Falcon drew**: the
   interpreter has not been compared with the translation (its hardware model is the one the
   translation check of F030SMW used; the SMAS-specific lines are listed below), its frame clock is
   a fixed instruction budget (30,000 per frame, `$4212` and `$4210` follow a pseudo time inside
   it), and game speed, lag and the audio side are not modelled. Per game: 45 frames at every
   100th frame of a 4,500-frame run (title, menus, levels, SMB3 map) plus 120 consecutive frames
   (3,000-3,119) of play. 652 snapshots, 183 MB under `build/ref` (`make ref-snaps`; untracked).

Where a result below says "real frames", it means (2).

## What was taken from F030SMW

From the commits `e1e1923` (reference renderer), `3c0a4b7` (DSP stack), `f922dd9` (DSP tile
renderer in the game), `4698c31`, `0b60eb5`, `c1da8a9`, `fa54277`, `69c4aa6`, `32cc95c` (list
compositor), taken as the committed files of SMW's HEAD `32cc95c`; a second pass took `e3c633f` (the LCW kernel,
window rows in the planner, the render throttle; below) and the `PPU_RAW` line of `e3635a5` (SMW's working tree had
uncommitted changes in `snes_ppu.c`, `ppu_dsp_plan.c`, `smkx_ppu.c/h`, `lcdtest.*`, `dsp_snap.c`,
`smkx_m7dsp.s`, `smkx_rt.s` and the Makefile while this was done: they are not in this copy).

| | |
|---|---|
| `src/snes_ppu.c/.h`, `ppu_fast.c/.h`, `ppu_internal.h`, `tools/ppu_snap.c` | reference renderer (windows, sub screen, replay, `aptr`); **SMAS change:** `ppu_abus_lorom`/`ppu_aptr_lorom` mask the ROM offset with `PPU_LOROM_MASK` = 2 MiB - 1 (SMW: 512 KiB), SRAM with 32 KiB |
| `src/ppu_dsp.h`, `ppu_dsp_plan.c`, `ppu_rows.*`, `ppu_tc.*`, `smkx_prof.h`, `falcon/dsp/ppu.asm`, `smkx_dspboot.s`, `tools/dsp_model.*`, `tools/smkx/dsp_asm.py` | exact DSP tile renderer: planner, DSP program (tile, Mode 7, window, list-compositor builds), host model |
| `src/smkx_ppu.c/.h`, `smkx_m7.c/.h`, `falcon/smkx_m7dsp.s`, `smkx_video.s` | DSP boot and tile driver |
| `src/lc_plan.c/.h`, `smkx_lc.c`, `falcon/smkx_lcput.s` | list compositor: planner, ring pages, DSP jobs, walker, hog-mode blitter |
| `src/smkx_video.c` | compositor first (`lc_on`), then the blocking DSP renderer, then `ppu_fast`; **SMAS change:** build switches `SMKX_DSP` (0: CPU renderer only) and `SMKX_LC` (0: no compositor), Makefile `SMASX_DSP`, `SMASX_LC` |
| `falcon/smkx_rt.s` | hooks merged by patch from the five runtime commits only (see below) |
| `src/pputest.c`, `lcdtest.c`, `falcon/pputest.s`, `lcdtest.s`, `tools/dsp_rand.c`, `dsp_snap.c` | PPUTEST and LCDTEST, host packers; **SMAS change:** `dsp_rand` also writes LCDTEST's input (optional 5th argument), `dsp_snap` does not pack frames the tile program cannot hold |
| `tools/lc/lc_model.c`, `lcd_model.c`, `lc_longest.py`, `shot_compare.py`, `smas_to_ct_snap.py` | blitter renderer's host models (F030CT's, with SMW's `LC_SMW=1` rules); **SMAS change:** the LoROM HDMA map takes `LC_ROM_MASK` (2 MiB); the model is built against this tree's `snes_ppu.c`, not F030CT's |
| `falcon/lcplay.s`, `lcwalk.s`, `scripts/run_lcplay.py`, `run_lcwalk.py` | Hatari players of recorded streams; **SMAS change:** `lcplay.s` writes the Videl base through `d6` (finding 1 below) |

`falcon/smkx_rt.s`: the hunks of SMW's `f922dd9`, `4698c31`, `0b60eb5`, `c1da8a9`, `fa54277`
(xdef/xref, all 128 VRAM pages marked in `rt_m7_dirty` and `rt_plan_dirty`, `rt_render` codes 2
and 3, HSCROLL (`$ffff8265`) and line offset (`$ffff820e`) cleared on a plain-screen flip, the
snapshot writer rebuilding the plain screen from the ring through `smkx_lc_window`, status
counters, `SMKX_SNAP_FROM` and the asynchronous snapshot schedule); `smkx_video.s`: base low byte
written last, `smkx_vid_set_window`. **Not** taken: SMW's V flag in `rt_vf`, the I flag restore,
page tables, dispatch cache and `rt_r2180` (they belong with `emit.py` changes that this copy does
not have: ROADMAP step 2). One SMAS-only runtime line: `$4016` reads 1 (finding 3).

Makefile: `ppu/ppu7/ppuw/ppulc.bin`, `m7c2p_boot.bin`, `test-ppu`, `test-lcd`, `check-dsp-model`,
`lc-snaps`, `check-lc-model`, `check-lc-fuzz`, `check-lc-play`, `bench-lc-play`, `check-lcd`,
`bench-lcw`, `ref-snaps`, `smasx-parts` (runtime and C objects without `sources.txt`),
`smasx/smkx_lc.o` and `rt.o` follow the switch stamp (SMW's Makefile ties only `smkx_video.o` to
it, so a changed `LC_BLIT_MIN` or `LC_SYNC` does not rebuild `smkx_lc.o` there).

## Windows in the compositor and the render throttle (F030SMW `e3c633f`, second pass)

Taken as SMW's diff of `32cc95c..e3c633f`, patched in file by file (the Makefile and `smkx_video.c`, `snes_ppu.c`,
`lcdtest.s`, `dsp_snap.c` by hand around SMAS's own changes): `falcon/dsp/ppu.asm` (the LCB+WIN build,
`ppulcw.bin`, `lc_range` in the window region), `ppu_dsp.h` (`DSP_KERNEL_LCW`, `DSP_LCW_SLOTS` = 73,
`DspPlan.force_win`), `smkx_ppu.c` (`smkx_ppu_plan_lc`, the LCW boot), `lc_plan.c` (window rows), `snes_ppu.c`
(`ppu_replay_dx` shifts BG scroll and window edges inside the replay), `smkx_lc.c` (a job carries its kernel),
`lcdtest`/`dsp_snap` (LCW frames), `lcplay.s` (the same `d6` fix this tree had made), `ppulcw.bin` in the Makefile.
**Render throttle:** `SMASX_RENDER_EVERY` and `SMASX_SLOW_WAIT` are gone; `SMASX_RENDER=0` turns rendering off
(`SMKX_RENDER`, as `SMWX_RENDER`), otherwise `smkx_video.c` throttles itself. `SMASX_DSP` and `SMASX_LC` stay.

Windows in SMAS (real frames, reference run): the reference's `ppu_win_layers` finds window rows in **15 of 660
snapshots: SMB1 6, Lost Levels 6, SMB2 0, SMB3 3** (of 165 each: the sparse frames and the 120-frame windows). The DSP planner
gives the same 15 the window build when it plans as the compositor does. Checked on them:
- `lcd_model` (the compositor's planner `lc_plan.c` against the reference): the 12 SMB1 and Lost Levels window frames are all
  covered, **0 wrong pixels**. SMB3's three are "slow (why $82)": its status-bar IRQ (one `INIDISP`/`TM`/`TSW`/scroll
  write at line 193 and a second OAM segment) is a per-line change and an OAM segment, not the window.
- `lcd_model` over each game's 120 consecutive frames (3,000-3,119): SMB1, Lost Levels and SMB2 **120 covered, 0 wrong
  pixels**; SMB3 0 covered, 120 slow (the same status-bar split on every frame). So the compositor, as it stands, takes
  SMB1, Lost Levels (windows included) and SMB2, and refuses SMB3's level frames. (`lcd_model` knows nothing of the DSP's
  page slots: SMB1 and Lost Levels frames mostly need more than the build's 77 or 73, see above.)
- LCDTEST on the 15 window frames (LCW) and 3 tile frames, 8 patterns each: 0 differing words (see the table above).
- `make check-lc-model` (F030CT's older model, no windows) still skips the window frames ("windows": 12); `check-lc-play` and
  `check-lcd` after the take-over: 120 frames, 0 checksum mismatches each.

## What builds

- `make smasx-parts`: `rt.o` (`smkx_rt.s` with the four DSP programs and the 2 MiB ROM incbin'd)
  and the nine C objects cross-compile; linked together the only undefined symbols are the
  translation's (`smkx_reset`, `smkx_nmi`, `smkx_irq`, `smkx_dispatch`, `smkx_dispatch_count`,
  `smasx_input` from `input.s`). No stub translation was linked or run.
- Host tools: `c2p_test`, `palette_test`, `ppu_snap`, `dsp_snap`, `dsp_rand`, `lc_model`,
  `lcd_model`, `ref65816`.

## What was checked, and how

| check | result |
|---|---|
| `make test-c2p`, `test-palette` | pass |
| `make test-m7` (Hatari) | `M7TEST lines $40 segments $2FD differing words 0` |
| `build/dsp_rand` | 32 random frames (seed `0x5eed`): the planner plus the DSP model equal the reference on all, byte for byte with SMW's `dsp_rand` output |
| `make test-ppu` (Hatari, DSP LC-free tile/window/Mode 7 builds) | 117 frames: **85 real** (reference-run frames the tile program holds) + 32 random, **0 differing words** |
| `make test-lcd` (Hatari, the compositor's DSP builds, eight mask patterns a frame) | 240 frames: **144 real** (18 reference-run frames: 15 with windows on the window build LCW, 3 on the tile build, each with eight mask patterns; `dsp_snap -packlc` plans them as the compositor does: colour math left out, at most 77 or 73 pages) + 96 random, **0 differing words** |
| `make check-dsp-model` | of 652 real frames 592 model = reference, 0 differing, 0 read outside the plan; the other 60 are Mode 3 (below) |
| `make check-lc-model` | census 428 of 652 frames modelled, 224 skipped; `seq`: 426 updates, 0 wrong pixels, 0 wrong planar bytes; `blitfuzz` 50,000 pieces: 0 pages wrong; the longest covered run is 120 consecutive frames (SMB1, frames 3,000-3,119); `hreal` on it: 0 wrong Videl words; the recorded stream replays on the host with 0 differing frames |
| `make check-lc-play` (Hatari, `falcon/lcplay.s`) | 120 frames, 0 window checksum mismatches |
| `make check-lcd` (Hatari, `falcon/lcwalk.s` on `lcd_model`'s stream) | 120 covered, 0 wrong pixels; Hatari 120 frames, 0 mismatches |
| `ppu_snap` on the reference frames | the fast composer (`ppu_fast.c`) equals the reference where it applies (the Falcon-plane comparison does not: the planes are zero) |

`make bench-lc-play` (Hatari, 5 passes, no vertical-blank wait, **emulator time, not a Falcon**):
12.2 ms per update; blitter ops 0.9, restore copies 5.5, data writes 1.8, loop and Videl 4.4. The
stream is SMB1 scrolling, 31 blitter rows and 527 dirty groups an update). The loop figure is
large because this stream has about 1,200 COPY records a frame (SMW's: about 120).

### What the real frames showed (SMAS-specific; all from the reference run)

- **Mode 3 for the splash, title and game-select screens** (60 of 652 frames: 15 a game). The
  DSP planner refuses BG modes 3-6 (`fail 4`), so these go to the CPU renderer. `ppu_fast` equals
  the reference on them (host). The Falcon cost of the CPU path on a 256-colour title picture is
  unknown. The games themselves (SMB1, Lost Levels, SMB2, SMB3) draw in mode 1 (SMB2 also uses
  mode 2 for some frames).
- **Page budget.** The planner's `need` (VRAM pages it uploads) against the program's slots (93 tile,
  89 window, **77 compositor build**): SMB1 median 91 pages, max 109; Lost Levels the same; SMB2 max
  70; SMB3 max 46 (frames of 148 planned: SMB1 146 and Lost Levels 135 are over 77; 2 and 3 over
  93). So in SMB1 and Lost Levels the compositor's DSP build cannot take nearly any frame, and
  the blocking tile renderer cannot take a few; SMB2 and SMB3 fit. The model reads far fewer pages
  than the planner asks for (SMB1 frame 3,000: need 91, read 37): the planner uploads every page
  the name tables point to, including parts off screen. Narrowing that would help SMB1.
  F030SMW's note in `ppu_dsp.h` ("frames read at most 65 pages") is SMW's, not SMAS's.
- **SMB3 is not covered by the compositor model**: 145 of its frames are skipped with "TM varies"
  (the status bar's screen split changes `TM` between rows; the model accepts constant `TM`), 2
  covered. SMB1: 140 of 163 covered, Lost Levels 142, SMB2 144. Other skips: 12 windowed frames,
  3 forced blank.
- **The sub screen is on in nearly every game frame** (`TS` $06 or $13, `CGWSEL` $02-$06,
  `CGADSUB` $20: BG2 and BG3, the clouds, hills and the HUD, reach the picture only through the
  backdrop's colour math). Of the sparse frames the reference's `ppu_snap` reports sub-screen rows
  in 38 of 45 (SMB1), 40 (Lost Levels), 36 (SMB2), 19 (SMB3). The CPU renderer `ppu_fast.c`
  has no sub screen, so on those frames it differs from the reference in 20,000-50,000 of 57,344
  pixels (SMB1 frame 3,000: 19,427; the HUD and BG2 are missing). **The CPU path is therefore not a
  usable fallback for SMAS game frames**: only the DSP builds (`ppuw`: window and sub screen, and
  the compositor's) draw them right. The title, splash and select screens (Mode 3) have no sub
  screen and `ppu_fast` equals the reference there.
- **Pause menu**: SMAS draws its own "CONTINUE / SAVE&CONTINUE / SAVE&QUIT" box in SMB1, Lost
  Levels, SMB2 and SMB3 on Start.
- **Mosaic** (the fade between title and select screen) is composed by the reference.

### Findings about the platform, from the reference run

1. **`lcplay.s` at F030SMW's HEAD is broken** (`make check-lc-play` there fails after the first
   frame): commit `9348783` ("video base low byte last") used `d5` as a scratch register in the
   frame end, but `d5` holds the memory base every stream op adds. Hatari then reads at a wild
   address (`Bus Error reading at address $e7f75002`). Fixed here (`d6`); not in SMW's copy.
   `lcwalk.s` has the same code with no register clash.
2. **SRAM must mirror every 8 KiB in bank `$70`-`$7D`.** SMAS's copy detection
   (`CODE_00803B`..`CODE_00807D`, `SMAS_Global_CopyDetectionCheck2` at `$70:2000`,
   `...Check1` at `$70:0000`) writes `$AA`/`$55` to the first and compares with the second; on a
   failure the game shows "WARNING: IT IS A SERIOUS CRIME TO COPY VIDEO GAMES" and loops forever
   (`$94B5`). This copy's runtime maps `$70-$7D:0000-$7FFF` to a 32 KiB `rt_sram` without
   mirroring (and the translator's inline page lookups, once taken over, must mirror too): the
   translated game will show the warning. Done in the reference run; **not** in `falcon/smkx_rt.s`
   (ROADMAP step 3).
3. **`$4016` must read 1 whatever the game wrote.** SMAS strobes `$4016` (`STZ`) in every joypad
   poll; with a shadow register the controller check stores `Controller1PluggedInFlag` = 2 and
   SMB1's poll (`$05C800`) adds that to `$4218`, i.e. reads the pad of port 2. Fixed in
   `falcon/smkx_rt.s` (`.joyser`, as F030DKC's runtime has it).
4. SMAS polls V-blank in its start-up (`BIT $4212`), so `$4212` needs a time model; the runtime's
   alternating bits answer such loops (they did here too, only slowly).
5. **The compositor's DSP build stopped answering after a forced-blank frame: found and fixed.** Reproducer: LCDTEST on
   one frame (`F0001194.BIN` of the reference run: `INIDISP` $80, nothing drawn, 0 VRAM pages) with mask pattern 1 (about
   15% of the groups at random) followed by any other frame: the next frame's DMA record never completed. Cause, found with
   Hatari's DSP debugger (`hatari-debug dspreg`, `dm x ...` through the command FIFO; the listing `build/smasx/ppulc.lst` maps
   the PC): in `falcon/dsp/ppu.asm` `blank_row` (forced blank and Mode 7 rows) put out all 128 words of a row whatever the
   list compositor's row mask said. All planes of a blank frame are zero, so the host's list (zero words for the masked groups)
   matched the first `nl` words anyway and the test printed 0 differing words; the DSP went on, its 512-word transmit ring
   filled (`room` waits for at most 256 queued), the host's DMA record was complete and nothing drained it, and the DSP never
   returned to its command loop to read the next `LCMASK`. In the debugger: `PC` in `room`, `fill` = 295 words queued, the
   masks in X:$3000 those of the previous frame, the 68030 in `put` waiting for the host port. Fix: with `LCB` `blank_row`
   sends 8 zero words per group of the mask (`ppu.asm`, 12 lines; the five builds assemble, tile and Mode 7 builds are
   byte-identical to before). The same fix is committed in F030SMW (`93c0357`) and noted in the other ports. In the game the
   compositor skips a frame that is forced blank throughout (`lc_plan` returns 1); a frame that turns blank part-way (an IRQ
   writing `INIDISP`) or a Mode 7 row in an LC frame reached it. `dsp_snap -packlc` packs these frames again.
6. **`dsp_snap -packlc` must plan with `ppu_replay_nocg`**, as `smkx_lc.c` does: SMB3's status-bar split (an IRQ at line 193 writing
   `INIDISP`, `TSW`, `CGADSUB` $20 and scroll) otherwise gives 123 of its frames a sub screen from row 192 on and the window
   kernel, whose list then lacks the last 32 rows (LCDTEST timed out at word $6000 of $7000). With the replay as the game's
   there are 15 window frames in 652 (SMB1 6, Lost Levels 6, SMB3 3), the same 15 the reference reports as "windows on N
   rows". The cause of that confusion was in the test packer, not the DSP.
7. `falcon/lcdtest.s` of F030SMW incbin's `ppu_tests.bin` as well (an unused label): the program grows with PPUTEST's frames and
   Hatari refuses a `LCDTEST.TOS` above about 13 MB ("Program too large"). Removed here.
8. Minor: `vasmm68k_mot -DSMKX_TRACE_FROM=0x7fffffff` (the Makefile's default) is parsed as 0
   with "warning 24: trailing garbage" (vasm wants `$7fffffff`); with traces off nothing prints, so
   it is harmless unless traces are on. Same in F030SMW's Makefile.

## Not verified (everything that needs the game)

- Any run of the DSP renderer, the compositor, the walker or the snapshot writer **inside the
  Falcon runtime**: `smkx_rt.s`, `smkx_video.s`, `smkx_lcput.s`, `smkx_lc.c`, `smkx_video.c` and
  the video-base/HSCROLL handling were assembled and compiled, never executed with SMAS.
- Whether the merged `smkx_rt.s` hooks are right for SMAS's translator output (they are SMW's
  hooks; the SMAS translator is SMW's of `17baedb`).
- Hatari's blitter/DSP behaviour with SMAS's real mixes of work: the compositor's DSP build was
  tested on 72 real plus 96 random LC frames (see below), not driven by the game's planner.
- The reference interpreter against the translation; game speed; frames with an IRQ chain in
  SMB3 beyond what the census shows; sound.
- Timing on a Falcon: none anywhere in this document.

## First things to check once the translation runs

1. `lc_why`/status line of the frames (`make run-smasx`, `rt_status`: `lc`, `lcp`, `ppu`
   counters): how many frames the compositor takes per game; expect SMB1 and Lost Levels to fall
   back (77-page slots, finding above) and SMB3 to be refused ("TM varies"), the splash, title and
   select screens to take the CPU path (Mode 3).
2. A window of consecutive snapshots (`SMASX_RENDER=1` renders every frame it can, `SMASX_SNAP_EVERY=1
   SMASX_SNAP_FROM=n`, `SMASX_LC_SYNC=1` so that the picture and the snapshot's inputs are one
   instant) through `make check-lc-model` and `lcd_model` (`LC_SNAPS=build/smasx`): the model must
   cover the frames the game's compositor drew, with 0 wrong pixels.
3. Synchronous snapshots against `ppu_snap` (`make check-snaps`): the Falcon's planes against the
   host's, as for SMW.
4. The copy-detection screen (finding 2) and port-1 input (finding 3) at the very start.
5. The reference run's frames against the translation's at the same game state (scripted
   `SMASX_INPUT` from `tools/ref_tour.py`, frame numbers differ with speed).
6. DSP and sound: `smkx_m7_init` is not called any more (the DSP boots when a scene needs it);
   the SPC700 port (ROADMAP step 6) shares the DSP and will have to give it up per frame.

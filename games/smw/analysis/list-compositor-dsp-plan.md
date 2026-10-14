# DSP-managed blitter renderer: plan

2026-09-30. Decision (from the project owner): the DSP does all graphics management; the 68030 only
walks a list and clears and blits (sprites, tiles). Ideal data flow:

    VRAM/OAM/CGRAM/register deltas in RAM
      -> DMA playback -> crossbar -> DSP SSI receive      (handshake mode, no CPU word moves)
      -> DSP: resident VRAM pages, scroll, dirty sets, composition, sprite and tile caches
      -> DSP SSI transmit -> crossbar -> DMA record -> list buffer in RAM
      -> CPU walker: clear / blit / load / flip           (falcon/lcplay.s is its ancestor)

Companion records: `list-compositor-smw.md` (model and player). This note replaces the earlier
"CPU-side producer" plan; it was dropped by that decision. Nothing below is implemented yet.

## What already exists (other ports), what does not

- **DSP tile renderer, exact** (`games/ct/falcon/dsp/ppu.asm`, shared with SMK, DKC, FFVI): resident VRAM
  pages (93 slots), layers, sprites, priorities, windows and the sub screen (`WIN=1` build), eight
  planes, streamed over the SSI to a DMA record buffer, 165 to 205 ms a frame. Its host model
  (`tools/dsp_model.c`) and the planner (`src/ppu_dsp_plan.c`) go with it. **SMW has none of it.**
  Measured here with CT's `dsp_snap` on SMW snapshots: 0 differing words against the reference on the
  title frame and four level frames, at most 46 of the 93 page slots needed.
- **SSI input by DMA playback** (`games/smk/falcon/dsp/ssirx.asm`): handshake mode is exact under Hatari
  (free-running loses words), up to 393,000 16-bit words/s; ScummVM's `hsc2p.asm` runs playback in and
  record out together. Scaffold only, not integrated in a renderer anywhere.
- **The list and its walker** (`F030CT`, `falcon/lcplay.s` here): DATA, COPY and BLIT records played
  on the 030 and Hatari's blitter, bit-exact on 337 SMW level frames. Its producer is the model on the
  host. **The DSP-side producer does not exist in any port.**

Traffic is small: a level frame changes about 7 VRAM tiles, 2 OAM entries and up to 5 px of scroll
(census in the record), so the input is well under 1 KB a frame; the list is about 2 to 5 KB. The
SSI carries that easily; cold starts (64 KB of VRAM) cost tens of milliseconds and happen at scene
changes.

## Order of work, each step with an exactness check

1. **Take over the exact DSP renderer** (shared code): CT's `snes_ppu.c/.h` as SMW's reference (with
   the LoROM bus and the exports `ppu_fast.c` needs), `ppu_dsp.h`, `ppu_dsp_plan.c`, `smkx_ppu.c`,
   `ppu_rows`, `ppu_tc` (unused here), the boot and test programs, `dsp_model`/`dsp_snap`/`dsp_rand`,
   `ppu.asm` built as `ppu.bin` and `ppuw.bin`. Check: `dsp_snap` and `dsp_rand` on the host, PPUTEST
   in Hatari. Result: SMW's picture comes from the DSP as a whole frame over the existing route.
   Known gap in the shared reference and so in both paths: `ppu_sub_cfg` returns 0 when `CGWSEL`
   bits 7-4 are set, which SMW's level frames do (`$12`, `$22`), so the sub screen (BG2 shown through
   the backdrop by colour math) is composed on the title frame (`CGWSEL $02`) but not in levels.
   SMW's fast composer equals the reference on all level snapshots (`make check-snaps`), so neither
   shows it there. Whether the SNES does, and so what the exact picture is, needs the colour-math
   renderer (`ppu_render_rgb`) or snes9x as judge (CT has `tools/s9x_ref.c`); not done. Keeps `ppu_fast.c` as fallback while it is needed.
2. **SSI plumbing both ways**: deltas in by DMA playback, output by DMA record, the walker replacing
   the full-frame display; both directions in handshake mode. Check: a stream of the same frames
   arrives bit-exact in RAM, with the DSP busy.
3. **DSP-side producer, first version**: the DSP finds the dirty 16-pixel groups from its inputs (VRAM
   writes, OAM diff, scroll, registers; it cannot afford per-group page hashes: DSP RAM is full of
   VRAM pages), composes exactly those groups with the existing row code, and emits them as group
   stores (16 bytes each, about 326 a frame). Check: page window equals the reference every frame,
   as the player's checksum does. DSP cost per group is CT's 26 to 50 microseconds; scrolling frames
   (about 840 groups) are the test.
4. **Blits instead of group data**: sprite and tile caches uploaded by the list, sprite rows and
   tile rows as blits with masks, clears, hardware scroll with the ring. This is the model's list
   (`list-compositor-smw.md`); the walker is already there.
5. **Falcon measurements**: DSP time per frame, SSI and host-port behaviour, blitter and Videl
   contention, eight-plane HSCROLL. Hatari models none of these faithfully.

## Risks

- DSP time per frame decides the rate. Exact composition of every dirty group may take more than a
  frame on scrolling frames (22 ms at 26 microseconds a group); step 4 exists to cut that.
- DSP memory: VRAM pages take most of it; per-group state for two pages does not fit, so dirtiness
  must come from inputs, which needs the runtime to report every VRAM, OAM and register change
  faithfully (missing one shows as a stale group).
- Frames the exact renderer cannot do incrementally (mode 7, layer-masking windows, HDMA scroll) go
  over the same record path as whole frames; the walker treats one as a full blit.

## Blitter facts from f030fastbobs

`../../../f030fastbobs` (private repo `AnimaInCorpore/f030fastbobs`, cloned next to the other projects on
2026-09-30; `src/main.s`, `blit` .. `blit_fastest`) is the Falcon blitter prior art. What it shows, read
from the code (I have not run it, and it carries no timing figures):

- **Two ways to draw a masked sprite.** `blit`: pass one, OP 4 (`D = ~S & D`) from a mask clears the
  sprite's footprint in each plane; pass two, OP 7 (`D = S | D`) ORs the sprite, whose transparent
  pixels are zero. Works for any mask, costs two passes. `blit_fast` .. `blit_fastest`: one pass per
  sprite row, the 1-bpp row mask turned into endmask 1+2 (one long write) and endmask 3, which is the
  run-with-masks form `falcon/lcplay.s` and the model use; it needs each row's visible span to be one run.
- **All eight planes in one start**: Y count 8, destination Y increment `-16+2` (plus `-16` when
  shifted): the same register arithmetic the model derives (recorded in `list-compositor-smw.md`).
- **Fewer register writes per row** (`blit_fast2`, `blit_fastest`): registers loaded once into address
  registers; endmasks rewritten only when the mask changed (`cmp.l d3,d1 / beq`); the sprite data
  reordered so that the source address the blitter leaves behind is already the next row's; rows with an
  empty mask skipped; X and Y count written as one long; start, HOG, flags and skew written as one word
  to `$ffff8a3c`.
- **HOG is set** (`$c0` / `$c000`): the blitter holds the bus and the 030 waits, so `falcon/lcplay.s`'s
  busy poll after each start is not needed on hardware. Hatari's behaviour there is not checked.
- A comment warns of strange behaviour when `OP` (`$ffff8a3b`) is written right behind a blitter start.

Consequences for the walker: a blit record should carry few words (the DSP can pre-compute the
increments and the skew/flags word, as the packed records already do), consecutive rows of one sprite
should share source increments so the source pointer advances by itself, and the OP 4 / OP 7 pair is the
fallback for a sprite whose rows are not single runs. None of this is in `lcplay.s` yet.

## Step 1 status (2026-09-30): the exact DSP renderer runs on SMW frames in Hatari

Taken over from F030CT (`falcon/dsp/ppu.asm`, `src/ppu_dsp.h`, `ppu_dsp_plan.c`, `ppu_rows`, `ppu_tc`,
`smkx_prof.h`, `src/pputest.c`, `falcon/pputest.s`, `smkx_dspboot.s`, `tools/dsp_model`, `dsp_snap`,
`dsp_rand`, `tools/smkx/dsp_asm.py`; `m7c2p.bin` assembles identically with the newer assembler). LoROM bus
in `dsp_snap`. Not taken over yet: the runtime glue (`smkx_ppu.c`, the boot and page upload in
`smkx_rt.s`, `smkx_video.c`), so the game still draws with `ppu_fast`.

- `make check-dsp-model`: the DSP design model equals the reference on all 530 snapshots, at most 65 of the
  93 page slots needed. (An earlier run showed thousands of differing words: `dsp_snap` had the HiROM bus,
  which misreads SMW's HDMA tables; it is the LoROM bus now.)
- `make test-ppu` (`PPUTEST.TOS`, patched Hatari `v2.6.1-devel (Sep 22 2026)`, emulated): the DSP composes 12
  real SMW frames and 32 random ones, **0 differing words**, and the blank frame. Times for the real frames,
  per frame, render and record over the SSI, after the VRAM pages are uploaded:

  | frame | pages | upload | render+record |
  |---|---:|---:|---:|
  | title | 11 | 23 ms | 218 ms |
  | level frames (seven) | 37-46 | 66-98 ms | 153-170 ms |
  | level frame, window and sub screen on every row (two, `CGWSEL $03`) | 63-65 | 128-131 ms | 426-430 ms |
  | no sub screen, few pages | 19 | 37 ms | 100 ms |

  The upload is the cold start (every page needed); later frames upload only the pages that changed. About
  155 ms for a full level frame is CT's figure too (165-205 ms), and is what the list compositor has to beat
  by composing only what changed. The sub-screen build is about 2.7 times slower.

## Step 2 status (2026-10-01): the game draws through the list compositor

What runs (Hatari `v2.6.1-devel (Sep 22 2026)`, emulated; nothing here is a Falcon timing):

- `src/lc_plan.c` (planner: which 16-pixel groups of the scroll ring change, from VRAM writes, OAM, scroll,
  registers), `src/smkx_lc.c` (jobs: plan, the DSP's two renders, list buffers; walker; block copies; Videl
  window), `falcon/smkx_lcput.s` (list walker, movem copies and blitter blocks), `falcon/dsp/ppu.asm` LCB build.
  Covered frames: uniform registers, BG modes 0/1 with 8x8 tiles, no windows or mosaic, at most one OAM
  segment; colour math is left out. Anything else falls back to the exact renderers.
- Exact: the synchronous build (`SMWX_LC_SYNC=1`) matches the reference on every snapshot checked (1000-1600 and
  1200-1500 after the later changes; sub-screen and window rows are the known differences). Host model:
  337 consecutive frames, 0 wrong, 520 composed and 295 copied groups a frame. The pipelined asynchronous
  build shows the same picture on paused scenes (six snapshots, 0 differing bytes); its pictures lag the inputs
  otherwise.
- The DSP's list has no headers any more (the eight plane words of each group, in row and group order): the CPU
  knows the masks, so it finds the blocks itself: rows that repeat a mask are a band, a run of a band is one
  2D block (source rows `rowbytes` apart, destination rows 512 bytes apart). The groups the other page missed
  are blocks the same way. A block of 24+ groups goes to the blitter in hog mode (`smkx_lc_blit`; the biggest
  block of a job, the CPU copies the others first, the job is shown when the blitter is idle; data cache cleared
  after). **In Hatari the blitter is no faster than the CPU's long moves** (4.4 against 4.7 microseconds a
  group) and **non-hog mode with the CPU working meanwhile left scattered groups wrong, differently every run**
  (hog mode: exact in every run). On a Falcon a hog blit may starve the DSP's DMA record of the next job:
  untested. `SMWX_LC_BLIT_MIN=100000` keeps everything on the CPU. This is not F030CT's blitter renderer
  (sprite and overlay blits from source caches over restored groups): the DSP still composes every changed
  group, sprites included.
- Pipeline: a job is planned from a snapshot of registers, scroll, OAM, CGRAM and the IRQ log (VRAM stays the
  live one: what changed since is planned again by the next job) while the DSP renders the job before it, so the
  CPU's planning (25-50 ms) hides under the DSP's time. Static frames cost about 15 ms of planning, none of DSP.
- Planner per plan, 030 ticks (1/38400 s), an SMW level with an opaque moving BG3: replay 835 -> 135 (HDMA run
  skipping in `snes_ppu.c`, HDMA tables by pointer), layers 1,400 -> 520, masks and copies 330 -> 130, VRAM diff
  700 (unchanged). Whole plan 3,400 -> 1,900 (heavy) and 1,600 -> 900 (light); the DSP planner and page upload add
  about 35 ms. Walk 900 ticks for a frame with 3,600 groups (17 ms for the list, 6 ms for the copies).
- Game in an unattended 2,000-frame run (emulated): full game speed in the scrolling sections; pictures shown
  per 100 frames: 25-50 in light scrolling, 8-14 where BG3 moves opaque (the DSP composes the whole window,
  about 130 ms, and the page is 3,584 groups to walk).

Open, in the order they pay: the DSP's per-row cost (0.25-0.8 ms a row, desc/sliver build) is the floor for heavy
frames; the VRAM diff (700 ticks) and the DSP planner (19 ms) are the next CPU costs; tile and sprite blits from
cached sources (the plan's step 3) would take composing off the DSP for sprites and static tiles; HSCROLL and the
blitter are checked in Hatari only.

## Step 3 (2026-10-01): windows, and the render throttle

**Windows.** A frame whose rows have a window masking a layer used to be "not covered" (`lc_why` bit 2) and went the old way
(the DSP's window build, or the CPU). Now:

- **Kernel.** `ppu.asm` with `LCB=1 WIN=1` (`ppulcw.bin`, `DSP_KERNEL_LCW`) is the compositor with the window build in it.
  The window build leaves 73 page slots (`DSP_LCW_SLOTS`, the tile build's LC has 77); the main program with both
  features is 58 words over the 3,008 it may have (P:$0040-$0BFF), so `lc_range` moved into the window build's own
  region (P:$5300-$54FF, now 499 of 512 words) when both are on. `make test-lcd` packs window frames for it too (4 of the
  12 test frames; 0 differing words; `DSP_SHIFT=dx build/dsp_snap` checks a shifted frame, also 0).
- **Driver.** `smkx_ppu_plan_lc(in, force_win)` says which program a render needs (LC, or LCW when rows have windows); a
  job carries it and the DSP is booted into it before the job's first send (a few ms; a transition's frames alternate
  between the two programs). The two renders of a frame must use one program: the one that did not need windows is
  planned again with `force_win`.
- **Replay.** The compositor draws frames shifted by `dl` pixels, so `ppu_replay_dx` shifts every BG horizontal scroll and
  the windows' edges in the callback state. **An edge on the row's edge (left 0, right 255) stays there**: the ring holds the
  picture beyond the screen's edges (the 17th group, the left margin), and an inverted full window shifted by dx would hide
  the margin; when the camera moved those pixels came into view blank (found in the game as 12-64 wrong bytes in the last
  row of windowed frames; the host model does not see it, it needs consecutive frames).
- **Planner.** `cap_fn` takes later rows that differ in window registers ($2123-$212B, $212E, $212F) and records per row the
  hidden spans of the masked layers (`WinRow`); `win_dirty` marks per row and layer the groups whose hidden pixels differ
  between the last frame and this one at the same world place (the edges are screen positions, the ring is the
  world's); spans touching the row's edge go on beyond it. Content that changes under a window is marked as before (more
  than it shows, never less). The replay result is not kept between frames when windows are on.
- **In the game** (synchronous build, Hatari v2.6.1-devel Sep 22 2026, emulated): 75 snapshots at frames 1,500-4,600 (every 40), 5 of them
  with windows on 48-224 rows (the transition and the HDMA ones): **0 bytes differ from the reference on every one**, the windowed
  ones included (`PPU_RAW=1 build/ppu_snap` prints the Falcon planes' difference on rows where windows or the sub screen
  make the CPU composer differ by design; it used to be zeroed).
- **Host model.** `lcd_model` over all 533 SMW snapshots in file order (windows on 186 of them, mostly the 'window 1 inverted
  empty' transitions and the HDMA ones): 531 covered, 0 wrong pixels (before: 342 covered). The 2 left: non-uniform rows
  and more than one OAM segment.

**Render throttle.** `RENDER_EVERY` and `SLOW_WAIT` are gone. `smkx_render` is called every frame and starts a
composition only when `next_ok` has passed: a composition's cost sets when the next may start (`spent * 100 / share`);
a blocking DSP or CPU frame costs its own duration again (the game gets as long as it was stopped). `share` (percent of the
time the renderer may take) adapts every 25 frames from the frames the game lost (`rt_stuck`): twice in a row more than
10 over what it loses with no composition going on (measured in the windows without one) shrinks it by a quarter,
otherwise it grows; windows with a blocking frame say nothing. Snapshot builds (`SMWX_LC_SYNC=1`) do not throttle. `SMWX_RENDER=0`
renders nothing (what `RENDER_EVERY=60000` was for). F030DKC renders in the game's idle time, which is its throttle;
`DKCX_RENDER_EVERY` is gone too. In the 2,000-frame run of the intro and the demo level (emulated): 146 pictures
against 330 unthrottled and 45 with a stricter controller; the game ran at 79% in both (its own speed in the intro).
The controller is a first cut: `rt_stuck` is a coarse measure of the game's lost frames.

## To do: visibility-aware dirty marking (2026-10-01, asked for)

A moved layer (BG2 at half speed in DKC's jungle, an opaque moving BG3 in some SMW levels) marks every non-blank cell at
its old and new place, whether or not a layer above hides it. DKC's jungle then composes about 2,400 of 3,808 groups a frame,
SMW's BG3 levels about 3,600. Mark a cell only where the layers above it leave a pixel visible (the ring layer's per-row
opacity bits with the priority bits: a group is hidden when both of its BG1 cells are opaque on that line and BG1's
priority covers the layer below); the lc_check estimate for DKC is about 440 groups a frame. The DSP's cost is per row,
so the gain is real only where whole rows stay clean. Same item in `games/dkc/analysis/list-compositor-dkc.md` ("Open").

## Later fixes (2026-10-01)

- **WRAM snapshot (found in F030CT's game runs).** Every replay reads the HDMA tables from the game's live WRAM; a plan spans tens of
  NMIs, so the groups the planner chose and the rows the DSP composed came from different tables and bands of rows stayed wrong
  (only in pictures planned while the game ran; synchronous snapshots plan with the game stopped and were exact).
  `snapshot()` in `src/smkx_lc.c` now copies WRAM (128 KiB) and gives the job a private `PpuBus`.
- **`lc_covers()` (found in F030SMW2).** The DSP is booted into the compositor's program only for frames the compositor covers; the exact
  path's plan (`smkx_ppu_plan`) no longer refuses window or sub-screen frames while that program is loaded (they went to the CPU
  composer, which has neither).
- **`blank_row` (found in F030SMAS).** With LCB a forced-blank or Mode 7 row sent all 128 words whatever the row mask said; the host
  expected only the masked groups, the DSP's transmit ring filled and it hung on the next frame. It sends 8 words per masked group now.
- The other ports' state is in their own `analysis/list-compositor-<game>.md`.

# List compositor (blitter renderer) on SMW: model, player and what they show

2026-09-30. F030CT's list compositor (`games/ct/analysis/list-compositor.md`, commit `fc09a13`)
taken over as far as it can be checked without a DSP-side producer: the host model on SMW's
snapshots, and the Hatari player on the stream the model records. **Nothing here runs inside the
game and nothing is a Falcon measurement.** The hardware is Hatari `v2.6.1-devel (Sep 22 2026)`
(patched F030Arcade build), Falcon, TOS 4.04, `--cpu-exact`, `--dsp emu`, fast-forward.

The idea in one paragraph: a retained frame buffer per Videl page; per frame only a list of
changed 16-pixel plane groups and sprite/overlay pieces; the 68030 restores the dirty groups from
a clean copy and draws the pieces with blitter rows. The exact renderer stays the reference.
What does not exist in any port yet is the DSP (or CPU) side that produces the list from the
PPU state in the game: the model computes it on the host from snapshots.

## Files

| | |
|---|---|
| `tools/lc/lc_model.c` | F030CT's model, with `LC_SMW=1` (below) |
| `tools/lc/smw_to_ct_snap.py` | SMW snapshots in CT's layout (same conversion as F030DKC's `dkc_to_ct_snap.py`) |
| `tools/lc/lc_longest.py` | picks the longest run of consecutive covered frames from the census |
| `falcon/lcplay.s` | the Falcon player (CT's, plus a timing mode and skip switches) |
| `scripts/run_lcplay.py` | builds and runs the player in Hatari: `check` and `bench` |
| `make check-lc-model`, `check-lc-play`, `bench-lc-play` | the whole chain; `LC_CT=../ct` is where `src/snes_ppu.c` comes from |

The model is built against **F030CT's** `src/snes_ppu.c` (`ppu_replay`, `ppu_walk_all`, colour
math and windows): SMW's own copy has none of them. SMW's renderer is not touched.

## What SMW needed in the model (`LC_SMW=1`)

- **LoROM HDMA bus.** CT's model reads ROM-sourced HDMA tables with the HiROM map.
- **Colour math that changes no index.** CT skips a frame when `CGWSEL` bits 5-4 are not 3 and
  `CGADSUB & $3F` is non-zero. SMW sets `CGWSEL $02/$12/$22`, `CGADSUB $20` in every level (BG2 or
  BG3 shown through the backdrop). `ppu_sub_cfg` composes the backdrop-only case when `CGWSEL` bits 7-4
  are zero (the title frame, `$02`), so there the skip lost nothing; every SMW frame was skipped. Now a frame is kept when the math is against a black fixed colour (no sub screen, no
  halving), or backdrop-only math the renderer composes itself (`ppu_sub_cfg` bit `$20000`, which
  the "sub screen" skip lets through as well), or a colour window that is empty (mode 1 "inside":
  never, mode 2 "outside": always) or full (the reverse). SMW's level frames have `CGWSEL $12`,
  `WOBJSEL $23` and window 1 over the whole screen (`WH0 $00`, `WH1 $FF`): a full colour window,
  so "math inside" is math everywhere. `W12SEL $33` inverts window 1 for BG1/BG2, which masks
  nothing there.
- **Not solved** (of 530 snapshots: 342 covered, 188 skipped): 46 with a window that masks layers
  (window 1 inverted over an empty range with `TMW $15`: BG1, BG3 and OBJ hidden, as in a screen
  transition) and 142 whose colour-window math uses a partial or per-row window (`WH $21/$DF`, or
  a full window only on some rows): skipped, as in CT. I looked at a handful of each, not all. CT's reference renderer also leaves out the sub screen on rows with
  `CGWSEL` bits 7-4 set (`ppu_sub_cfg` returns 0), so on those rows the model is exact against a
  reference that, by reading the code, is not what the SNES shows. Not checked against a picture.

## How the window was made

Consecutive frames need `SMKX_SNAP_FROM` (this project's `falcon/smkx_rt.s`) and a build that
renders every frame:

    make smwx SMWX_SNAP_EVERY=1 SMWX_SNAP_FROM=4100 SMWX_FRAMES=4700
    python3 scripts/run_smkx.py --program build/smwx/SMWX.TOS --fast --timeout 3300 --tag win2

About 25 minutes; 520 snapshots of 288 KB. A build that renders every frame is on a different
timeline from the default one (the level the demo shows at frame 4,000 there is a forced-blank
transition here, and nothing is written for blank frames): find the scene with a coarse
`SMWX_SNAP_EVERY` first. Then `make check-lc-play`. The default `SMWX.TOS` comes back with `make smwx`.

## Results

Window: demo level, frames 4,364-4,700 of that timeline, 337 consecutive covered frames, BG1
scrolling 0 to 158 px (72 frames move it by 3-5 px, BG2 about half of that), 3-4 sprite instances
an update. **One stretch of one level, at 3-5 px a frame: not a mix of SMW.**

- **Exactness.** `seq` and `hreal`: 0 wrong pixels, 0 wrong planar bytes, 0 wrong Videl words.
  `blitfuzz` 50,000 pieces: 0 pages wrong. The recorded stream replays on the host with 0
  differing frames, and `falcon/lcplay.s` in Hatari matches all 337 page checksums.
- **Work per update (mean):** 96 blitter rows, about 326 dirty groups, 60 restore runs, 4 background
  groups rewritten; list 2,155 bytes unpacked, about 600-820 packed. Scrolling frames cost about
  2.3 times the blitter rows of still frames (220 against 62).
- **Player time in Hatari** (`make bench-lc-play`, 5 passes, no vertical-blank wait):

  | | ms per update | share |
  |---|---:|---:|
  | all ops | 5.90 (169 updates/s) | |
  | blitter ops (95 a frame, about 40 µs each) | 3.83 | 65% |
  | restore copies | 1.61 | 27% |
  | data writes | 0.04 | 1% |
  | loop and Videl | 0.38 | 6% |

  By the op counts that is about 4 ms for a still frame and about 13 ms for a scrolling one.
- **The model's unit costs are off for Hatari.** It assumes 13 µs for a blitter row (`b`) and
  4 µs for a copied group (`c`); Hatari's player measures about 40 µs and 5 µs, which gives
  5.9 ms against the model's 3.0. A real Falcon's blitter will differ from both; measure there
  before using any of these.
- **Against the rest of the frame** (same Hatari, rendering off): game logic in this demo runs at
  50.0 game frames per 50 VBLs with the ported translator (47.6 before), so it is capped by the
  VBL and the headroom is not visible; with the blitter side at 4-13 ms the two do not both fit
  in 20 ms in the scrolling frames if they run back to back on the 030.

## What is missing, in order

1. The producer: something in the game that turns PPU state (OAM, scroll, VRAM/CGRAM deltas,
   HDMA) into this list per frame, on the DSP or the CPU. Its cost is the unknown that decides
   the frame rate; the exact DSP renderer takes 165-205 ms a frame in CT's measurements.
2. Windows and colour-window math (the skipped frames; `ROADMAP.md` item 4 wants them anyway),
   and mode 7 (the model skips it).
3. Consecutive frames from more levels and the overworld; scrolling at 1-2 px; HDMA scroll effects.
4. Unit costs measured on a Falcon (`b`, `c`, the host-port byte cost).

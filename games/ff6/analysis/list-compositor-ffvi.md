# The list compositor in FFVI (2026-10-01)

Taken from F030CT's finished port (`../ct/analysis/list-compositor-ct.md`; FFVI's runtime is a copy of CT's, the differences are
names), which has F030DKC's/F030SMW's window build (the LCW kernel) in it and F030CT's own fixes (the `blank_row` stall, `put_block`'s mirror, one copy of WRAM per plan). Everything here is the patched F030Arcade Hatari
`v2.6.1-devel (Sep 22 2026)`, emulated. **The compositor builds, its host and DSP tests pass, and in the game it is exact on the intro/title scene it covers (31 of 177 snapshots; the field frames are refused).**
Build switch `FF6X_LC` (default **0**: only the title/intro scene is covered and verified), `FF6X_LC_MATH` (default 1), `FF6X_LC_SYNC`, `FF6X_LC_DIAG`, `FF6X_LC_CHECK`, `FF6X_LC_ALLPAGES` (debug builds, as in F030CT).

## What was taken

Copied (FFVI's versions were identical to CT's before the port, apart from names): `src/snes_ppu.c/h`, `ppu_internal.h`, `ppu_dsp.h`,
`smkx_ppu.c/h`, `lc_plan.c/h`, `smkx_lc.c`, `lcdtest.c`, `falcon/dsp/ppu.asm` (LCB build, `blank_row` fix, masks at X:$0500, 93 page slots),
`falcon/smkx_lcput.s`, `lcdtest.s`, `lcplay.s`, `tools/dsp_snap.c` (`-packlc`), `ppu_snap.c` (`PPU_SNAP_NOMATH`), `tools/lc/` (`lc_model.c`,
`lcd_model.c`, `lcd_fuzz.c`, `lcput_test.c`, `lc_why.c`, `lc_longest.py`), `scripts/lc_snaps.sh`, `lc_check.py`, `lc_shot.py`, `run_lcplay.py`. Patched (the same hunks as in CT): `smkx_video.c`,
`smkx_video.s`, `smkx_m7dsp.s`, `smkx_rt.s` (also gains `SMKX_SNAP_FROM`, which FFVI's runtime lacked), `Makefile` (flags `FF6X_LC`,
`FF6X_LC_MATH`, `FF6X_HOLD`, `FF6X_SNAP_FROM`, and `FF6X_RENDER` (0: no rendering) in place of the render interval `FF6X_RENDER_EVERY`, which is gone
(the runtime's own throttle, `smkx_render_poll`, paces the renders: as in F030CT, F030DKC and F030SMW); `ppulc.bin` and `ppulcw.bin`; `test-lcd`, `check-lcd-model`, `lc-snaps`, and the blitter model's `check-lc-model`,
`check-lc-real`, `check-lc-play`, `bench-lc-play`). The ROM is `build/ff6x/ff6.sfc`, HiROM like CT's: the same HDMA bus.

## Results

FFVI has 28 snapshots (`build/ff6x/F*.BIN`, 295,112 bytes each, from F030FFVI's runs of 2026-09-30; none consecutive).

- **Build**: `make ff6x FF6X_LC=1` compiles, assembles and links (`FF6X.TOS` 12.0 MB with `ppulc.bin` 9,069 bytes); the default build (`FF6X_LC=0`)
  has the compositor compiled out and the other configurations (`FF6X_TC=1`) compile as before.
- **`make check-dsp-model`**: the DSP renderer's model equals the reference on all 28 snapshots (0 words differ).
- **Planner on the snapshots** (`build/lc/lc_why`, `build/lc/lcd_model`): **1 of 28 covered** (title frame `F00005DD`, mode 1, with colour math), 0 wrong pixels
  in it; **27 not covered, all "per-line changes"** (with colour math refused: 0 covered). What changes per line in a field frame (a replay of `F0003AA9` with the
  planner's own flags, row by row): rows 2-6 write `$2111`/`$2112`/`$210D`/`$210E` (BG scroll set up by HDMA), a window edge pair (`$2128/$2129`) and **`TM` = 0 until row 6,
  then `$17`** (a six-row band at the top shows only the backdrop), then `$2106` (mosaic) `$FF` at row 214, and an IRQ at line 216 that writes `INIDISP=$80` (the
  bottom-of-screen blank CT has too, which the planner handles) with an OAM segment. HDMA runs on channels 1-7 (`$2130/$2131` CGWSEL/CGADSUB and `$2132` COLDATA gradients, which
  are left out of the replay; BG1/BG2 scroll; `$2128`; `TM`; `$2106-$2109`). Per-line `TM` (even for a band at the top) and the mosaic write are what the planner refuses.
- **`make test-lcd`** (the compositor's DSP builds in Hatari, on real frames): the first 12 of the 28 snapshots (more would make `LCDTEST.TOS` larger than the 14 MB the machine has: FFVI's window frames read up to 78 pages),
  eight mask patterns each and the shifted renders (a window's fine scroll, `dl - 256`): 168 frames, **0 differing words** (the window build, LCW: FFVI's field frames have window rows, `window rows 208`). An earlier run with all 28 snapshots
  x 8 patterns (224 combinations, before the shifted renders): 0 differing words.
- **Host tests**: `make test-ppu` 60 frames (28 snapshots + 32 random) 0 differing words; `make check-lc-put` (the block writer on random walks and copies: 20,000 blocks each, 1,000 whole jobs) 0 wrong; `make check-lcd-fuzz`
  (the title frame made into 3 x 400-frame sequences with a random forced blank and scroll) 0 wrong pixels.
- **In the game** (Hatari `v2.6.1-devel (Sep 22 2026)`): without input the game stays on the title: `nmi 1` through the first 1,900 frames (FF6's SPC driver costs the 68030 minutes for the title theme), so a first synchronous run over
  frames 1,400-1,650 was all forced blank (63 snapshots equal the reference, the compositor never engaged). With `FF6X_APU_ECHO=1` (no SPC700, the port writes echoed): `scripts/lc_snaps.sh build/lc/ffa 1500 4500 16 FF6X_LC=1
  FF6X_LC_MATH=0 FF6X_APU_ECHO=1 FF6X_SNAP_FROM=1500`, asynchronous default build, a snapshot every 16th frame: **177 snapshots, 31 covered by the compositor (frames 1,508-1,986: the title screen and the slow scroll
  over the world picture behind it), 0 differing words; the 146 others went the old way (132 "per-line changes", `$002`, and 14 `$102`: the field frames' per-line `TM`, and the frames with colour math/other) and equal the full reference too.**
  The compositor planned 187 jobs and showed 187 pictures (1 cold start, 1 fallback). That is the only scene of FFVI it covers, and a title screen is not a play session: field, menu and battle frames are refused. The title is exact
  also with the planners reading one copy of WRAM per plan (F030CT's fix: the HDMA tables the planner read while the game rewrote them gave bands that stayed wrong; FFVI's title uses little HDMA, so this run does not exercise the fix hard).
- **Blitter renderer (F030CT's model and the Hatari player)**: `make check-lc-model` runs: the model's census skips all 28 snapshots (`colour math`, `TM varies`,
  `forced blank`), so only the synthetic checks run (`blitfuzz` 50,000 pieces, 73,621 blits, 0 pages wrong, and 50,000 more with a 64 KB cache, 0 wrong);
  `check-lc-real`/`check-lc-play`/`bench-lc-play` need a window of consecutive snapshots the model covers (`make lc-snaps LC_FROM= LC_TO=`) and there is none
  (FFVI's field frames vary `TM` per line, which the model refuses): not run. That renderer exists as a model and a Hatari player only, in every port.

## What it means for FFVI

The compositor does not help FFVI's field frames as long as HDMA changes `TM` (rows 0-5 off) and writes the mosaic register `$FF` on row 214. Both are small bands
at the edges of the picture: a planner that takes a top band of rows with no layer visible (and one mosaiced row before the blank) as part of the picture would cover them; that
is the thing to try, not the DSP builds. The title and intro scene
(frames ~1,500-2,000 with the SPC driver off) is covered and exact; the menu and later scenes were not run (no input scripted past the title in this session).

## Open

- The compositor covers the title/intro only; run the field, menu and battle scenes (an input script past the title: `FF6X_INPUT`) before turning `FF6X_LC` on by default, and look at what the planner says there (`build/lc/lc_why`).
- The planner for FFVI's edge bands (`TM` = 0 on the first rows, mosaic on row 214): the field frames' only reasons for refusal; a planner that takes a six-row band without a layer and one mosaiced row as part of the picture would cover them.
- Not verified on a Falcon (Hatari only); the blitter renderer (model and player) was not run: no consecutive window of frames it models (FFVI's field frames vary `TM` per line).
- F030CT's table of the compositor's costs (95 ms a plan on average, about half the pictures of the old renderer in a covered stretch) applies here as well.

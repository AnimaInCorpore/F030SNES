# F030CT's list compositor checked on DKC (host model, 2026-09-30)

Question: does the blitter strategy of F030CT (`games/ct/analysis/list-compositor.md`:
retained frame buffer per page, the DSP sends a list of dirty groups and sprite pieces, the
030 restores groups and draws pieces with blitter rows, the Videl scrolls a ring) fit DKC?
Emulator and host model only; nothing ran on a Falcon, every count is list items, not time.
Per the task, colour math, windows, the per-line CGRAM gradient and other effects are
**ignored**: the model runs on the same unshaded indices DKC's renderer draws today.

Hatari for the captures: patched F030Arcade build, Falcon, `--dsp emu`, `--cpu-exact`,
release build (`DKCX_DIAG=0`).

## Method

- DKC's `src/snes_ppu.c` is the older copy (no colour math or windows), so CT's model
  cannot be built against it. CT's `tools/lc_model.c` and `ppu_census.c` were built
  against **CT's** `src/snes_ppu.c`, on DKC snapshots converted to CT's layout
  (`tools/lc_check/dkc_to_ct_snap.py`: SRAM padded to 8 KiB, `planes = 8`, empty palette
  blocks). Nothing in DKC's own PPU was touched.
- `lc_model` skips a frame that uses colour math. CT's index renderer ignores colour math
  anyway, so for this check the skip was taken out of a scratch copy (`else if (rows.math)
  why = ...` removed). The result is "the list scheme on DKC's unshaded picture".
- A window of consecutive real frames: new switch `DKCX_SNAP_FROM` (Makefile, and
  `SMKX_SNAP_FROM` in `falcon/smkx_rt.s`, taken over from F030CT):

  ```
  make dkcx DKCX_FRAMES=4600 DKCX_SNAP_EVERY=1 DKCX_SNAP_FROM=4400 \
    DKCX_DIAG=0 DKCX_INPUT='2100:1000,2110:0000,2500:1000,2510:0000,2700:1000,2710:0000,2900:1000,2910:0000,3100:1000,3110:0000,3300:0100'
  ```

  195 snapshots, frames 0x1135-0x11F7, Jungle Hijinxs with Right held. 42 of them
  are forced blank (the snapshot fell inside the NMI's blank); the other 153 are real
  picture. Gaps of one or two frames between modelled frames exist where a blank one was
  dropped; the per-update figures below use only pairs with no gap.

## Results

**Census (modellable frames).** With the colour-math skip removed, 153 of 153 picture
frames are modellable (mode 1, TM = BG1+BG2+BG3+OBJ, no mode 2-6, no mosaic, no window).
Of the older 11-frame set (logos, intro, map, menu, three level frames) only the three
level frames that have no colour math were modellable before. The renderer/rasteriser
disagree on 0 pixels.

**Sprites.** 17-18 OAM instances, about 180 pieces and 180 blitter rows per frame, list
4.5 KB as 20-byte records, **2.46 KB packed**: the sprite side of the scheme works on
DKC as on CT, bit-exact against the exact renderer in the model (0 wrong pixels, 0
wrong planar bytes over 151 updates).

**Background without hardware scroll** (`lc_model seq`, every snapshot a scene change):
2,811 of 4,032 groups dirty per update, because the level scrolls every frame. The
scheme is worth nothing there; it needs the Videl scroll.

**Hardware scroll: what the level asks of it.** `lc_model hreal` skips every frame
("scroll varies per line") and `hsim` refuses ("the scroll comes from HDMA tables that
cannot be patched"): neither model can run DKC yet. The level's HDMA is

| channel | target | meaning |
|---|---|---|
| 1, 2 | `$2121/$2122`, mode 3 | CGRAM writes every line (gradient), ignored here |
| 3 | `$2111`, mode 2 | BG3 horizontal scroll, a different value on 153 of 224 rows |

and the layers scroll as BG1 x = 62 to 194 over the window (rigid, about 1 px a frame,
the Kongs' speed), BG2 at half that (rigid), BG3 per line. Pixels each layer supplies
to the final picture (of 57,344; 3 frames of the window):

| layer | opaque | shown in the final picture |
|---|---|---|
| BG1 (foreground) | 43.6-48.5 k | 42.3-47.1 k (**74-82 %**) |
| BG2 (half speed) | 40-41 k | 6.3-6.8 k (11 %) |
| BG3 (per-line, priority top) | 5.8 k | 0.1-0.9 k |
| OBJ | 1.3-1.6 k | 1.3-1.6 k (2.5 %) |

So the standing set is obvious here: BG1 on the Videl ring; BG2, BG3 and the sprites are
overlay pieces (runs of pixels that differ from BG1 alone, blitted into the ring). An
estimate on the real window (`tools/lc_check/lc_export.c` renders the frame and BG1
alone, `lc_groups.c` hashes the overlay per world-aligned group and row and counts the
groups whose hash changed since the last frame; **not** a bit-exact model of the ring):

- overlay pixels 7,828 a frame (13.7 % of the screen), in 992 groups of 4,032;
- **dirty groups per update: mean 440 (10.9 %)**, median 158, p75 851, p90 909, max
  2,608; 39 of 111 updates change nothing;
- the big updates (about 900 groups, 40 % of the updates) are most likely the frames
  on which BG2 (half speed, not on the hardware scroll) moves a pixel against BG1, so
  that everything BG2 shows in BG1's holes is redrawn. Not checked against BG2's
  scroll value per frame. If so, faster scrolling raises the mean toward redrawing BG2's
  share every frame, about 1,000 groups, a quarter of the screen;
- restore+redraw runs 307 a frame on average.

Against this: the exact DSP renderer produces all 4,032 groups every frame.

## What this says

1. The strategy fits DKC's levels structurally: one dominant rigid layer, sprites as
   pieces, a thin per-line layer (BG3) that costs little because almost none of it
   shows. Expect 1,000 dirty groups at worst, 150 typically, in the jungle.
2. It is not yet proven: the ring model (`hsim`/`hreal`) cannot run DKC because BG3's
   scroll comes from HDMA and the model patches only CT-style tables. The figures above
   are the hash estimate. To validate bit-exactly the model must learn BG3-per-line as an
   overlay layer with its own scroll (CT's "a layer behind the standing one works as an
   overlay too" covers the layer order, not a per-line scroll).
3. DKC-only things the compositor does not cover, each deliberately ignored here:
   colour math (every row of the window: CGADSUB `C2`, BG2, fixed colour: a
   darkened BG2, so a palette question for the renderer, not for the list), the CGRAM gradient
   on channels 1 and 2 (per-line palette; the Videl palette is per frame), forced-blank
   frames (snapshot artefact), and the non-level screens (logos, intro, map: modes 2-6
   and colour math: those stay on the exact renderer).
4. Only one level section was captured. Other levels (water, mine carts, bonus screens,
   fades) need their own window before a number is believed.

## Files

`tools/lc_check/dkc_to_ct_snap.py` (snapshot converter), `lc_export.c` (per-frame
render + BG1-only render, built like CT's `ppu_census.c` against CT's `snes_ppu.c`),
`lc_groups.c` (the group estimate). The `lc_model` scratch copy is CT's file with one line
removed; not kept.

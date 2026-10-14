# The race screen, line by line (census, 2026-09-25)

What SMK's race picture is made of, as input for a race-only DSP kernel.
Everything here counts what the picture contains; no timing is involved.

## Method

- `tools/race_census.c` (`make build/race_census`;
  `build/race_census SNAP.BIN smk.sfc [frame.ppm]`) runs the reference
  frame loop (`ppu_render_rows`: HDMA, IRQ-logged writes, OAM segments)
  with a row function that records each row's registers, and prints the
  enabled HDMA channels, the IRQ-logged writes, rows grouped by layout
  with the values changing inside them, sprites per row and CGRAM changes.
  With a third argument it also draws the frame in SNES colours.
- The snapshots come from the reference interpreter, not from Hatari:
  `tools/ref65816.c` writes SNAP.BIN files (`REF_SNAP_EVERY`,
  `REF_SNAP_DIR`) and plays scripted input (`REF_INPUT`, as
  `SMKX_INPUT`; the upper 16 bits drive pad 2). Its renderer inputs equal
  the Falcon's: at the same game frame VRAM, CGRAM, OAM, the latched
  scroll and Mode 7 values, the IRQ log and the OAM segments are
  identical to a Falcon snapshot of the same game frame (checked on the
  title screen; the I/O shadow differs in `$2118`, which the renderer
  does not read, and WRAM in `$34`, see below). A run of
  several thousand frames takes about a second.
- `scripts/race_census_runs.py OUTDIR` plays every mode the menus reach
  and summarises the race frames (`$36` = 2, or $0E in battle), a
  snapshot every 100 frames; it finds each menu screen from the game
  state rather than from fixed frames. Every run holds B (accelerate)
  in the race; nobody steers.

A side finding: Falcon builds with `SMKX_SNAP_EVERY` draw snapshot frames
synchronously, and the NMIs during that wait are lag frames (WRAM `$34`
counts them): such a run is 13 frames behind the reference by frame 1000.
Builds without snapshots are not affected (a rendering build matched the
reference over 3000 frames).

## Coverage

| Run | Mode | Courses |
|---|---|---|
| `gp1` | 1P Mario GP, 50cc | Mushroom Cup, race 1 |
| `tt_CK` | 1P time trial | all 15 open courses (Mushroom, Flower, Star Cup) |
| `p2_0_K` | 2P Mario GP | Mushroom Cup, Star Cup |
| `p2_1_K` | 2P match race | Mario Circuit 1, Donut Plains 2, Vanilla Lake 1 |
| `p2_2_K` | 2P battle | Battle Course 1-4 |
| demo | attract mode | two demo races |

648 race frames from the 25 scripted runs, plus the 32 of the attract demo.

## Layout: two, fixed

Every race frame of every run has one of two layouts, and within a run the
layout never changes:

**One player** (GP, time trial, the second demo race): perspective view
over the course map.

| Rows | Mode | Main screen | What it is |
|---|---|---|---|
| 0-22 | 0 | BG2 BG3 BG4 OBJ | sky / horizon |
| 23-106 | 7 | BG1 OBJ | track in perspective, per-line matrix |
| 107-113 | - | (forced blank) | the split |
| 114-218 | 7 | BG1 OBJ | course map, A and D per line (B = C = 0) |
| 219-223 | - | (forced blank) | |

**Two players** (GP, match race, battle, the first demo race): two
perspective views.

| Rows | Mode | Main screen | What it is |
|---|---|---|---|
| 0-22 | 0 | BG2 BG3 BG4 OBJ | upper view: sky |
| 23-106 | 7 | BG1 OBJ | upper view: track |
| 107-113 | - | (forced blank) | the split |
| 114-134 | 0 | BG2 BG3 BG4 OBJ | lower view: sky |
| 135-218 | 7 | BG1 OBJ | lower view: track |
| 219-223 | - | (forced blank) | |

Per picture: 168-189 Mode 7 rows with one layer, 23-44 rows of Mode 0
with three 2bpp layers, 12 blank rows. No other modes, no mosaic, no sub
screen (TS = 0) in any race frame.

## Where the per-line changes come from

The same seven HDMA channels in every race frame of every run:

| Channel | Target | Table |
|---|---|---|
| 1-4 | M7A, M7B, M7C, M7D (mode 2, indirect) | $0640/$064D/$065A/$0667 |
| 5 | BGMODE (mode 0) | $0674: Mode 0 for 24 lines, Mode 7 for 88, ... |
| 6 | TM (mode 0) | $067D |
| 7 | WH0-WH3 (mode 4, indirect) | $7FE500 |

Channels 1-4 point at one per-line table each: 24 lines of one value,
84 lines one word per line (repeat), and the same for the lower view. In
the first demo race both views read the same tables (identical ranges:
A 55..1535, C 1..37, D 192..5290, B = 0); each view's own position comes
from the Mode 7 centre and scroll, which the IRQ at line 108 rewrites for
the lower view. The one-player map view reads its own A and D values
(105 lines).

The same IRQ writes in every race frame: line 108 (forced blank, OBSEL
$02 -> $0A, M7SEL, the lower view's BG and Mode 7 scroll and centre,
colour math: 32 writes), line 115 (display on), line 220 (forced blank,
then 24 CGRAM entries from $80 or $A0, written while the screen is
blank). OAM in three segments (lines 0, 108, 220): one sprite list per
view.

## Windows and colour math

When the census was taken the renderers ignored windows and colour math.
Main-screen windows are modelled since (`analysis/translator.md`,
"Windows on the main screen"); colour math is still not. In the race:

- Channel 7 sets the window edges per line. Mostly both windows are empty
  (left $21 > right $20). The colour window selects where colour math
  applies; `CGWSEL` $10 in the upper view allows it only inside the
  window, `CGADSUB` $3D adds the fixed colour (COLDATA 14/14/14) to BG1,
  BG3, BG4, OBJ and the backdrop. Seen in the one-player GP: a window of
  x = $E0-$EF on lines 80-96, a brightened patch of about 16x17 pixels.
  The lower view has `CGWSEL` $30 (no colour math); on Koopa Beach its
  windows cover the full width without effect.
- **Outside the race it matters:** the title menu (1P/2P GAME, the mode
  and class choices) is a box revealed by a window (HDMA channel 1 moves
  WH0/WH1, `TMW` $03 masks BG1 and BG2 inside it). Without windows the
  Falcon showed the title picture and no menu; with them it shows the
  menu (Falcon planes equal to the host render).
- No race frame hides a layer: `TMW` is 0 or $10 (sprites) and the
  sprites' window select enables no window, so the race kernel needs no
  windows.

## Sprites

Entries in range per row, before the 32/34 limits: at most 17 (8 racers
in the GP), mostly 5-12 in the Mode 7 rows, 2-3 in the sky rows, none in
the blank rows.

## What changes between pictures

Between race frames 100 frames apart:

- **VRAM:** 3-14 of 64 1 KB pages: sprite graphics ($5000-$6E00, kart
  poses and items), the HUD map ($7A00), a few Mode 7 pages (coins and
  item boxes taken, Ghost Valley's fences). The track itself stays.
- **CGRAM:** 1-11 entries (Koopa Beach's water: 6).
- **The HDMA tables and the IRQ values:** every picture (the camera).

## For the kernel

- The DSP can keep the whole Mode 7 VRAM (32 KB) and the sky's tiles and
  maps for the race, and get only changed pages.
- Per picture the 68030 would send: the matrix tables (as they sit in
  WRAM, a few hundred words), the per-view centre and scroll values, the
  BG2-4 scrolls, three OAM lists (compact, as `ppu_dsp_oam` builds them),
  changed VRAM pages, and CGRAM when it changed. No per-line replay on
  the 68030. The row layout is one of two; which one follows from the
  BGMODE/TM tables and the IRQ values, and any frame that fits neither
  goes to the general path.
- The palette mapping only needs recomputing when CGRAM changes.
- Rows to draw: Mode 7 with one layer plus sprites (168-189 rows), Mode 0
  with three 2bpp layers plus sprites (23-44 rows), blank rows.
- Windows: none hide a layer in a race frame. Colour math (not modelled):
  the one-player GP's brightened patch is the only visible use seen.

## Not covered

- The Special Cup (Donut Plains 3, Koopa Beach 2, Ghost Valley 3,
  Vanilla Lake 2, Rainbow Road): locked until the other cups are won.
- 100cc/150cc, the start fly-over and the finish sequence beyond the
  frames above, items with screen effects (lightning, star, feather),
  rear view, since nobody steers or uses items.
- Non-race screens (title, menus, driver select, results, podium) go
  through the general path in any case.

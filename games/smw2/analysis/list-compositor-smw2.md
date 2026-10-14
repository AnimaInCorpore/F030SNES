# The DSP stack and the list compositor in SMW2 (Yoshi's Island), 2026-10-01

Ported from F030SMW (`analysis/list-compositor-dsp-plan.md` there; HEAD `e3c633f` / `6ba29da`, which has the
compositor with windows and the render throttle) in the two steps SMW and DKC went through, plus F030CT's blitter renderer
(model and player). Emulator for every run below: the patched F030Arcade Hatari `v2.6.1-devel (Sep 22 2026)`, compiled
Sep 22 2026 21:37:14 (`../../../F030Arcade/third_party/hatari/build/src/hatari`), Falcon, TOS 4.04, 14 MB, `--cpu-exact`,
`--dsp emu`, `--fast-forward`. **Every time is emulated; nothing here is a Falcon timing.**

## Short version

- The DSP tile renderer, the exact reference renderer and the compositor run in the game (the game draws through them); the
  host model, PPUTEST and LCDTEST pass with 0 differing words.
- **The game reaches the logo, the Nintendo intro, the title, the file menu and the start of the next scene in Hatari, not a level.**
  Two things stop it. The translated SuperFX code (`YIX_GSU=xlat`, the default) corrupts the game: it stops with a stack jump at game
  mode 7; only `YIX_GSU=interp` (the interpreter: 5 to 20 times slower) gets past it. And in game mode `$38` (after the file menu) the game
  waits for ever on the APU port `$2140`: the sound upload of the next scene needs an SPC700, not the echo model. So **nothing of YI's
  gameplay has been run**.
- **The compositor covers only the logo frames** (BG mode 1, OBJ only). The intro, title and menu frames are refused
  (`lc_why` `$092`, `$142`, `$042`): YI's frame IRQ changes INIDISP per row and records the OAM in four segments, the title uses
  HDMA for mode, scroll, TM and Mode 7 per line, 16x16 tiles. They are drawn the exact way (DSP window program, or the CPU for
  Mode 7 rows) and are exact there.
- Found on the way, and not covered by anything in F030SMW: **a frame that starts in forced blank gets a black palette** (below).

## What was taken over

| Step | Commit | What |
|---|---|---|
| 1 | `222a7c9` | `src/snes_ppu.c/h` (colour math, windows, sub screen, replay), `ppu_fast.c` (the duplicate `ppu_planar8` goes), `ppu_dsp.h`, `ppu_dsp_plan.c`, `ppu_rows`, `ppu_tc`, `smkx_ppu.c/h`, `smkx_prof.h`, `pputest.c`, `falcon/dsp/ppu.asm` (its builds), `pputest.s`, `smkx_dspboot.s`, `smkx_m7dsp.s` (`rt_dsp_boot`, `rt_ssi_play`, the program images), `smkx_m7.c/h`, `tools/dsp_model`, `dsp_rand`, `dsp_snap`, `tools/smkx/dsp_asm.py`, `tools/ppu_snap.c`; Makefile: `ppu*.bin`, `check-dsp-model`, `test-ppu`; runtime: all 128 VRAM pages marked in `rt_m7_dirty` and `rt_plan_dirty`, the DSP programs boot on demand; `smkx_render` plans the frame, sends it to the DSP, falls back to `ppu_fast` |
| 2 | `0d4341c` | `src/lc_plan.c/h`, `smkx_lc.c`, `falcon/smkx_lcput.s`, `lcdtest.s`, `src/lcdtest.c`, `tools/lc/lcd_model.c`, `lc_model.c`; runtime hooks (`rt_render` returns 2 "ring window shown" and 3 "nothing new", a plain screen resets HSCROLL and the line offset, the snapshot writer rebuilds the plain screen from the ring, status line counters, `vid_set_base` writes the base's low byte last, `smkx_vid_set_window`); `make check-lcd`, `test-lcd` |
| blitter renderer | `fdb7ca0` | `tools/lc/lc_model.c` (F030CT's model, YI rules), `smw2_to_ct_snap.py`, `lc_longest.py`, `falcon/lcplay.s`, `scripts/run_lcplay.py`; `make check-lc-model`, `check-lc-play`, `bench-lc-play` |
| policy | `5120318`, `fc01de0` | below |
| windows, throttle | `fc01de0` | F030SMW's LCW kernel (`ppulcw.bin`), window rows in the planner, `ppu_replay_dx` moving window edges, `smkx_ppu_plan_lc(in, force_win)`, the automatic render throttle in `smkx_render` (replaces `RENDER_EVERY`/`SLOW_WAIT`; `YIX_RENDER=0` is the render-off switch) |

Where this port differs from F030SMW's copy:

- **The bus** is the SuperFX cartridge's: `ppu_abus_yi`/`ppu_aptr_yi` (LoROM at `$8000` of banks `$00-$3F`, the GSU RAM's first 8 KiB at
  `$6000`, linear ROM in `$40-$5F`, the GSU RAM in `$70-$7D`); `PpuBus.sram` is the 32 KiB GSU RAM.
- **Snapshots** keep YI's own layout, now **version 5 on both sides** (the runtime wrote 4 while the host tools expected 5):
  WRAM, the GSU RAM (32 KiB), the Falcon's planes, OAM segments. Every tool reads it directly; `tools/lc/smw2_to_ct_snap.py`
  converts to F030CT's layout for CT's own tools (first 8 KiB of the GSU RAM as SRAM).
- **When the compositor does not cover a frame the exact path stays exact** (`5120318`). F030SMW's `smkx_render` boots the DSP into the
  compositor's program before every frame, and its `smkx_ppu_plan` then refused any frame that needs the window program while the
  compositor's was loaded, so those frames went to the CPU's fast composer, which has no windows and no sub screen: **828 words of YI's
  intro frames differed from the reference**. Now `lc_covers()` (the register replay alone, no state touched, own window-row buffer)
  decides whether to boot the compositor at all, and `smkx_ppu_plan` (exact path) is separate from `smkx_ppu_plan_lc` (jobs).
  Intro frames in the game: 0 words differ from the model (was 828).
- Smaller fixes taken from the other ports' notes: `rt_wb` `.sysw` (ROM writes were stored; F030SMW), `dsp_snap` leaves out frames the
  programs cannot hold and all-backdrop frames (F030SMAS), `lcdtest.s` without PPUTEST's data (F030SMAS). **`falcon/lcplay.s` (F030SMW's
  since `9348783`) clobbered `d5`, the memory base of every later frame**: 112 of 113 checksum mismatches on the logo window, 0 with `d6`.
- `rt_io_write`'s VMADDH keeps 15 bits (`09258e3`): the runtime corrupted its own data when a game set the VRAM address to `$8000` or more (every
  port has the same lines). `smkx_inidisp()` (palette brightness, below). `ppu.asm`'s `blank_row` fix (F030SMW `93c0357`).
- `YIX_LC=0` builds without the compositor and without the throttle: every frame is drawn, so snapshots are consecutive.
- `lcd_model` prints, for every frame it refuses, the register state and `lc_why`; `tools/lc/snap_info.py` lists the registers and the
  HDMA channels of a snapshot, `snap_png.py` draws its Falcon frame with the frame's CGRAM.

## What the game reaches in Hatari, and what it does not

Timeline in VBLs (`rt_frames`, one handled frame each) of the interpreter build; the translated build reaches the same screens
earlier (loading at about frame 2,700 instead of 10,500) and then stops.

| Frames | Game mode | What | Drawn by |
|---|---|---|---|
| 0-1,500 | 0-3 | boot, SPC700 upload, SuperFX initialisation, forced blank | nothing |
| about 1,500-1,720 | 3 | Nintendo logo with sprites fading in: BGMODE 1, TM `$10` (OBJ only), 21 sprites, 2 VRAM pages | **compositor** (covered) |
| about 2,970 | 4 | "Nintendo presents": TM `$10`, 4 sprites | compositor (covered) |
| 1,700-10,500 | 5 | loading (SuperFX decompression), forced blank, one NMI every few hundred frames in the interpreter build | nothing |
| 10,500-12,000 | 7 | patchwork "Nintendo" screen: BGMODE `$09`, TM `$15`, TS `$02`, CGWSEL `$02`, CGADSUB `$20`; 51 pages | exact DSP, window program (sub screen) |
| from 12,300 | 9/10 | **title** (island, Mode 7 sea): BGMODE `$10` (mode 0, BG1 16x16), TM `$13`, seven HDMA channels | the CPU (`ppu_fast`): the DSP takes no Mode 7 rows |
| about 29,000 | 10 | **file menu** ("Play which file?"), reached with Start held 100 frames every 400 | CPU |
| from about 33,000 | `$38` | after A/Start at the file menu: forced blank, no NMI, 335 SuperFX runs, then **an endless wait on `$2140`** (the last I/O of the game for the next 27,000 frames; nothing changes: NMIs, DMA, APU writes, GSU runs) | nothing |

- **No level.** Two runs ended at frame 29,250 with `rt_frames` corrupted (`$00FFxxxx`, the game's mode byte `$38`, NMIs stopped). **A runtime
  bug, found and fixed here: `.vmaddh` stored VMADDH unmasked, so a game that sets the VRAM word address to `$8000` or more
  made the VRAM writes (`.vmdatal/h`, the DMA's VRAM path) go up to 64 KiB past `rt_vram` and set flags past the 128 bytes of the
  dirty arrays.** It is the same two lines in every port's `falcon/smkx_rt.s`. With the mask (`09258e3`) the same input goes on to game mode `$38` and stays there:
  the game's last I/O access is `$2140` for 27,000 frames. The sound upload that follows a file choice needs the driver's handshake, which the echo
  model of this runtime (written for the IPL upload) does not give; F030CT's SPC700 interpreter is the way (ROADMAP 5).
- **The translated build cannot get there.** `YIX_GSU=xlat` (default) reaches game mode 7 at frame 2,777 and stops with `stack jump to an
  unlisted target $000FCACB` at `$0FBF7F` (`LDA DATA_0FBF80-$1A,y : PHA : RTS` with a sprite status that is not `$1A`/`$1C`). F030SMK
  saw the same stop and called it "a new push-and-return target for `jumps.txt`". **It is not**: `YIX_GSU=check` runs every GSU call both ways
  and reports the **first difference at run 3** (as in `analysis/yi.md`), and with `YIX_GSU=interp` the same game passes game mode 7
  without a trap and runs on to the title. The translated SuperFX code writes something else than the core does, and the sprite
  tables in the GSU RAM go wrong; adding jump-table entries cannot help. The `.sysw` fix does not change it.
- Hatari speed: the interpreter build runs 12-48 frames per emulated second on an idle machine (loading is 9,000 frames), a long run
  takes tens of minutes; the CPU renderer costs about 1.4 emulated seconds per title frame, so the throttle (or `YIX_RENDER=0`)
  decides how far a run gets.

## Yoshi's Island's picture, as far as it was seen

(Snapshots of the screens above; levels not seen. "per line" = the register differs between rows of one frame.)

- **Modes:** BG mode 1 (logo, intro) and **mode 0 with BG1 on 16x16 tiles** (title, file menu: BGMODE `$10`), switched by HDMA to **Mode 7**
  for the sea (`$2105` per line, `$211B`/`$211D` matrix, `$210D`/`$210F` scroll, `$212C` TM per line, `$2121` CGRAM per line: seven channels,
  `HDMAEN $FE`/`$CE`). BGMODE `$09` in the intro is mode 1 with BG3 priority.
- **BG3 as a SuperFX bitmap layer:** not observed (no level). The intro's TM `$15` (BG1, BG3, OBJ) with TS `$02` (BG2 on the sub screen) has
  its text drawn by SuperFX code (`FXCODE_08C745` and neighbours), not a bitmap BG.
- **Windows:** the registers are set (intro `W34SEL $30`, `TMW $0A`; title `W12SEL $32`) but **no row had a layer masked** in any frame seen
  (`ppu_win_layers` 0): TMW selects layers the frame does not show.
- **Colour math:** intro CGWSEL `$02`, CGADSUB `$20` (backdrop math with the sub screen on BG2: the exact DSP window program composes it,
  the CPU composer does not); title CGADSUB `$3F` (applied by the replay per the registers; `CGWSEL 0`).
- **Mosaic:** none. **Sprites:** 4-21 on the logo screens, 8 in the intro.
- **Per-line effects (the reasons the compositor refuses):**
  - **YI's frame IRQ**: INIDISP is `$8F` (forced blank) at the start of every frame, the IRQ at line 13 writes `$00`, at line 15 `$0F` (or the
    fade value), and at line 199 forced blank is set again and **the whole CGRAM (up to 247 `$2122` writes) and the next frame's BG and OBJ
    registers are rewritten** (`IRQ writes 9` or 256, the log is full). The visible picture is rows 15-198 (184 rows, black bars
    above and below on a SNES). The OAM is recorded in four segments (lines 0, 13, 15, 199) that are **byte for byte equal**.
  - **Title/menu:** HDMA changes BGMODE (row 218/222), the ring layer's horizontal scroll varies by row, BG1 has 16x16 tiles.
- **Palette brightness bug (new, found here):** the runtime builds the Videl palette from INIDISP **at the start of the frame**, which is
  `$8F` for every IRQ-split frame: **the whole palette comes out black** (read from the code and the snapshots: `io[0x100]` is `$8F` in every
  intro frame). `smkx_inidisp()` now takes the value the IRQs set before row 112 (`$0F` or the fade step); a screenshot of the intro after the
  change shows the picture (the black one before it was not photographed). The title frames start with `$0F` and are unaffected. The
  compositor's palette (`smkx_set_palette`) takes the same value. Per-row brightness and the blank rows (a Videl palette is one brightness)
  are not modelled.
- **VRAM written per frame:** the runtime counts DMA bytes (`dma` in the status line, all DMA channels): loading 1.2-2.7 KB per NMI,
  **intro 47 bytes per NMI, title 20-38 bytes per NMI, file menu 455 per NMI over its first 1,500 frames**. The SuperFX's output reaches VRAM
  by DMA from the GSU RAM; the scenes seen rewrite almost nothing per frame, so the planner's VRAM diff is cheap there. A **level** (where YI
  streams tiles and runs its sprite code) was not seen; the planner's page need there is the open number (the intro frames need 51-52 of
  the 128 pages, the window program holds 89, the compositor's builds 77/73).

## Results

All Hatari figures: `Hatari v2.6.1-devel (Sep 22 2026)`, emulated.

| Check | Result |
|---|---|
| reference vs DSP model (`make check-dsp-model`, 161 snapshots of the logo, intro, title) | 120 equal, **0 differing words**, 41 frames the DSP cannot take (Mode 7 rows) |
| `make test-ppu` (6 real YI frames: logo x3, "presents", intro x2; 32 random; blank) | **0 differing words** in 38 frames; intro frame (51 pages): upload 103 ms, render+record 336 ms |
| `make test-lcd` (4 covered real frames, 8 mask patterns each) | **0 differing words** in 32; with `LC_ALL=1` (the two intro frames too, forced-blank rows and all) 0 in 48, after F030SMW's `blank_row` fix (before it the intro frames gave wrong lists for sparse masks and the DSP stopped answering) |
| `lcd_model` on 113 consecutive logo frames | **0 wrong pixels**; 1 cold start (3,584 groups), mean 31 composed and 31 copied a frame |
| `lcd_model` on the other 48 snapshots | 1 covered ("presents", cold), 47 refused: `$092` x6 (intro), `$142` x29, `$042` x12 (title, menu) |
| in the game, synchronous (`YIX_LC_SYNC=1`) and default asynchronous builds, logo scene | every snapshot **0 bytes** from `build/ppu_snap` (19 + 3 snapshots) |
| in the game (interpreter build, compositor on, 29,000 frames: logo, intro, title, file menu), 72 snapshots of the Falcon's frame against `build/ppu_snap` | **0 bytes differ in all 72**; the intro frames drawn by the DSP window program equal the DSP model (**0 words**, was 828 before the policy fix), the 67 title and menu frames (Mode 7 rows: the DSP cannot take them) were drawn by the CPU |
| the palette brightness of the intro on the screen | Hatari screenshot of the intro (`smkx_inidisp`): the patchwork picture in colour; not compared with a screenshot before the change |
| asynchronous build, 2,790 frames (xlat), and the interpreter builds through intro, title and file menu (up to 29,000 frames with the compositor, 60,000 without rendering) | no hang (the 60,000-frame run ends in the endless APU wait of the game itself) |
| CT's blitter model (`make check-lc-model`) on the logo window (113 frames) | census 113 modelled, `seq` 0 wrong pixels, `blitfuzz` 50,000 pieces 0 pages wrong, `hreal` 0 wrong Videl words, host replay 0 frames differ |
| `make check-lc-play` (`falcon/lcplay.s`) | **113 of 113 page checksums match**, 0 mismatches |
| `make bench-lc-play` | 1.04 ms an update: 2 cold updates and 111 empty ones, **not a measure of anything** (a static scene) |
| other repos' tests | `make test-m7`, `make test-c2p` pass |

**Not verified:** any frame of a level, the compositor on anything but the sprite-only logo frames (its scroll, copy and blitter paths ran in
the host model on 113 frames that change nothing after the first), the LCW (window) kernel on a real YI frame (no YI frame reached it;
LCDTEST's intro frames were taken out: see below), HSCROLL and the blitter on a Falcon, the new palette brightness on the screen,
CT's sprite/overlay blit producer (it does not exist in any port's runtime: only the model and the player), asynchronous pictures
against a moving scene.

## Coverage and `lc_why`

`lc_why` bits: 0 no frame, 1 per-line changes, 2 windows, 3 mosaic, 4 forced blank at the start, 5 mode above 1, 6 16x16 tiles, 7 OAM segments, 8 the ring
layer's H scroll varies by row.

| Frames | `lc_why` | Meaning |
|---|---|---|
| logo, "presents" | 0 | covered |
| intro | `$092` | per-line (INIDISP at 13/15/199), forced blank at the start, OAM in four (equal) segments |
| title | `$142` | per-line (BGMODE at row 218), 16x16 tiles, the ring layer's H scroll per row |
| title, a few rows later | `$042` | the same without the scroll |

`lcd_model` on the intro with INIDISP ignored: `$090`, i.e. the forced-blank start and the OAM segments remain. Both are cheap to see through
(equal segments could be merged in `lc_plan`; the blank start is the letterbox), but covering YI needs more than that: the black rows
(blank, and brightness 0 at rows 13-14) would have to be written as groups (or shown by a shorter Videl window), and the CGRAM rewrite at line 199
is already left out (`ppu_replay_nocg`). Without it **the compositor is idle in everything of YI seen so far except the logo**, and each
refused frame costs a replay (about 3 ms, `lc_covers`) before the exact path.

## Open, in the order it matters

1. **A level.** Two blockers, both outside the compositor. The translated SuperFX code is wrong (`YIX_GSU=check`, run 3): fix it (the interpreter is the
   reference), then every Hatari run is 5-20 times faster. And the next scene's sound upload waits on `$2140` (an SPC700; scripted input: Start held 100
   frames every 400 frames reached the file menu, A/Start every 240 frames after it reached game mode `$38`). Until then the compositor's value for YI is a
   measurement of its refusals.
2. **Letterbox frames in the planner**: INIDISP per row (brightness 0 / forced blank above row 15 and below 199), equal OAM segments; then
   whether a level's frames are uniform otherwise (scroll, 8x8 tiles, the SuperFX bitmap BG).
3. Per-row brightness and the blank rows (the black bars) on the Falcon; a before/after screenshot of `smkx_inidisp`.
4. The sound: an SPC700 for the handshake after the file menu (the same wait would stop every level).
5. The LCW kernel and the window rows on a real YI frame (none seen), and the LCB hang F030SMAS found after an all-backdrop frame (`ppu_dsp_plan`
   frames that read no VRAM page; the logo frames have two pages).
6. Falcon measurements of everything (blitter, DSP records, HSCROLL): none exist.

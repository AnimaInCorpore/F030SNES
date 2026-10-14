# F030SMK port roadmap

The port translates the original program (see
[analysis/translator.md](analysis/translator.md)). The sections below the
translator status keep the earlier asset work.

## Translator status

Done: placement of all listing instructions against the ROM, context- and
path-sensitive discovery from the vectors, flag liveness, 68030 emission,
the TOS runtime, NatFeats diagnostics, the APU IPL handshake model. The
program reaches the title screen in the patched Hatari.

Also done:

- DSP-1: the µPD77C25 firmware translated to 68030 code, checked against
  its interpreter.
- Video adapter: BG modes 0, 1 and 7, sprites, HDMA, IRQ writes, with a
  per-frame adaptive 16-colour palette (pixel-weighted k-means in OKLab).
- The DSP Mode 7 path: sampling, palette and c2p on the DSP, streamed over
  the SSI into DMA record; the CPU adds the other rows and the sprites.

- The list compositor for the tile-mode screens (title with its windows, intro, driver, cup and course select),
  taken over from F030SMW as `SMKX_LC=1` (off by default): eight planes, 256 colours, the DSP composes only the
  changed groups of a scroll ring; the race and every frame it does not cover keep the four-plane path. Exact in
  Hatari on every checked frame; speed and the blitter in eight planes not checked on a Falcon
  (analysis/list-compositor-smk.md).

Next, in order (revised 2026-09-24 against the design discussion and the
render profile, see analysis/translator.md "Render profile"):

1. Game code per frame, measured (analysis/translator.md, "Where the
   race's time goes"). Done so far: OAM/CGRAM DMA fast paths and a
   smaller frame-start copy (IRQs 30 → 6 ms per VBL, 3.2× game speed);
   the DSP-1 raster command made native (exact; runs while loading only).
   Next, by the timer-C profile:
   - the remaining runtime paths: DSP-1 port accesses through
     `rt_io_write`, other DMA kinds, `rt_resolve`;
   - a release build without diagnostics (`rt_note_caller`, clocks,
     status lines);
   - DSP-1 `project` ($06) native like raster (8% of samples in the race).
2. **The DSP renders, the 68030 plays** (decided 2026-09-24; measurements
   in analysis/translator.md, "Fast composer and the move of rendering to
   the DSP"). The C composer stays as reference, fallback and host check.
   - The DSP keeps a mirror of the VRAM the scene uses, as CGRAM indices'
     sources (palette-independent): X = `$0000-$3FFF` in the Mode 7
     format (as now), Y = `$4000-$7FFF` one VRAM word per DSP word (race:
     sprite characters, 2-bpp characters, the sky map: 12K of 16K words),
     the rest of Y for program, colour tables and one line buffer. The
     CPU sends only written 256-word pages (`rt_vram_dirty`, as for Mode
     7 now): the DSP's copy is the cache, keyed by VRAM address. Screens
     with a different layout send the pages the frame's registers point at.
   - Quantisation stays on the DSP's output side: pixels are CGRAM indices
     until the line is mapped through the colour tables (256 entries,
     re-sent only when the slot map changes), so a new palette costs no
     graphics transfer. Sprites are drawn 1:1: SMK's scaling is done by
     the game, which picks pre-scaled graphics.
   - Per frame the CPU sends OAM (per segment), the per-line register
     state (later: the HDMA tables themselves, so that the 68030 does not
     replay HDMA), and dirty pages. The DSP composes each line in one line
     buffer (layers back to front, sprites by OAM order, priorities), maps
     to slots, does the c2p and streams the 64 plane words over the SSI
     into DMA record: one record window per frame for all 224 lines.
   - Design (2026-09-24, analysis/translator.md "The DSP renderer"): the
     DSP holds the whole VRAM: X = words $0000-$3FFF one per DSP word (the
     Mode 7 format), Y = words $4000-$7FFF in 10,923 DSP words (two thirds
     one per word, a third as bytes in their top bits); the rest of Y takes
     pixel and colour tables, layer buffers, row caches, sprite lists and
     the program beyond the boot image. A two-stage boot (Dsp_ExecBoot
     loads only P:$0000-$01FF) loads the rest over the host port. Y is now
     full to the last word and the 512-word boot image nearly so: Mode 7
     needs overlays or smaller tables (see translator.md).
   - Milestones, each checked byte for byte against the C reference by a
     Falcon test program (as M7TEST does now):
     a. loader and packed VRAM: upload, read back (done: `make
        test-ppu`);
     b. BG rows (modes 0 and 1, 2/4 bpp, flips, scroll, priorities) into
        the line buffer, colour map, c2p, SSI (done, exact; three sky
        layers 598 us per line, emulated);
     c. sprites (32 per line, 34 slivers, sizes, flips, priorities) (done,
        exact; ~20 us per sliver);
     d. Mode 7 in the same program (done, exact; 493 us per BG1 line on
        the DSP; the per-line setup and planning still run on the 68030);
     e. the runtime sends per-line register state, OAM and dirty pages;
        the CPU composes nothing (done 2026-09-25: FRAME, the DSP derives
        every line and returns palette samples; all race snapshots equal
        the host's render). Since then: the stream is fed by the
        host-port interrupt (the 68030 returns to the game while the DSP
        draws), and SNES frames are paced by SMK's main loop (the missing
        karts; analysis/translator.md).
   f. Towards 40 ms a frame: follow the order in
      [analysis/dsp-renderer-budget.md](analysis/dsp-renderer-budget.md)
      (2026-09-25): prove the SSI transport and move the palette samples
      to DSP histogram bins; overlap SSI output with drawing through a
      ring; a fused Mode 7 sample/compose/c2p path; per-segment sprite
      membership and decoded-row caches; tile rows kept decoded; compact
      HDMA loops, a cheaper palette update, and no idle gap between DSP
      frames. Gates: pixel-only output near 30 ms (32 MHz route), then a
      race frame on the DSP below 80, 50 and 40 ms, byte-exact throughout.
3. SSI: 256x224 at 4 planes is 28,672 bytes a frame: 36.45 ms at the
   ideal 1 MB/s-class rate of the current 25.175 MHz clock, 28.67 ms at
   32 MHz. With the 8 sample words per line (32,256 bytes, 41.0 ms) the
   current payload cannot deliver a frame under 40 ms even with instant
   rendering (analysis/dsp-renderer-budget.md): the samples leave the SSI
   and the transfer moves to the 32 MHz route, checked on a real Falcon.
4. The rest: resolve traps and dispatch misses as they appear.
5. Colour math, windows, HDMA CGRAM changes within a frame.
6. Sound (the S-SMP program and BRR samples; F030CT's SPC700 interpreter).

Done 2026-09-24 (late): the demo race-end crash (long,X placement), the
attract loop through four demo races (24,000 frames, all snapshots equal
to the host), keyboard pad and input scripts (`SMKX_INPUT`; the title
takes B once the screen is at full brightness, then the driver select),
and the sibling fixes listed in `analysis/porting-notes.md` ("Taken over").

## Current verified checkpoint

- The supplied U.S. v1 ROM matches the base hash in the pinned U1 map.
- The ROM asset extractor follows named source ranges and the disassembly's
  nested LZ5 and Mode 7 tile routines.
- Course pixels are converted to DawnBringer 16 with a fixed nearest-color
  mapping and no dithering.
- The existing Falcon Videl viewer and host renderer are bring-up tools. No
  gameplay behavior from them is considered part of the SMK port.

## 1. Recover the original race screen route

Start at the ROM's dispatch and follow the race entry through its frame
dependencies. The source landmarks are in
`third_party/Super-Mario-Kart-Disassembly/SMK/Routine_Macros_SMK.asm`:

- the dispatch code around `CODE_808000` and the indexed state tables;
- `CODE_81E258` for the race-entry state;
- `CODE_81BF93` for race setup;
- `CODE_81E627`, `CODE_81E745`, `CODE_81E6D4`, and `CODE_81E72E` for course
  assets and palette loading;
- the routines that publish Mode 7 transforms, background layers, OAM, HUD
  sprites, and per-frame updates.

Record each routine's inputs, writes, arithmetic widths, ordering and caller
state in the active slice notes before writing its Falcon counterpart. Keep
unknown display placement, camera values, controls and game rules unresolved
until the listing settles them. The 256×224 Falcon mode does not imply a
112×112 viewport or a HUD split.

## 2. Complete source asset conversion

- Derive every source palette from the original palette tables and runtime
  palette upload path. (The translated program supersedes this route: its
  colours go through the adaptive OKLab palette at run time.)
- Recover sprite-sheet tile order, pose boundaries, transparency and OAM
  attributes from the source tables and drawing routines.
- Keep original ROM bytes and derived Falcon assets separate; record each
  conversion and source range.

## 3. Port one connected gameplay slice

- Port the recovered race update and render dependencies for one course and
  one supported player mode.
- Preserve original input semantics, update cadence, integer widths, wrapping,
  and render order. Mark only genuine Falcon hardware and operating-system
  adapters as target artifacts.
- Compare the source-derived asset outputs, captured state vectors and
  normalized 256×224 frames at each boundary. Do not claim game parity until
  the source contract fixtures pass.

## 4. Bring the slice up on Falcon

- Present the source-grounded frame in four-plane Videl mode with the agreed
  16-color palette.
- Verify the `.TOS` only in patched F030Arcade Hatari and record its build
  identity. Keep Hatari measurements separate from physical Falcon results.
- Add the remaining courses, game modes, audio and DSP/SSI work after the
  first connected gameplay slice has a complete contract and comparison path.

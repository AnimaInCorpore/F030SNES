# F030CT

An Atari Falcon 030 port of *Chrono Trigger*, following the approach of
the sibling [F030SMK](../smk) port: the original 65816 program is
statically translated to 68030 code from the user's ROM and a pinned
disassembly, so gameplay, logic and screen design stay the original's. The
DSP composes the picture: tile scenes in eight Videl planes with the SNES
palette as the 256 colours (`falcon/dsp/ppu.asm`), Mode 7 scenes in four
planes with 16 colours per frame chosen adaptively in OKLab
(`falcon/dsp/m7c2p.asm`).

Cartridge: HiROM FastROM, 4 MiB, no coprocessor.

## Sources

- `third_party/ct_disassembly`: [dscotton/ct_disassembly](https://github.com/dscotton/ct_disassembly),
  pinned as a submodule (eeae27a, 2026-07-11). A DiztinGUIsh export traced
  in bsnes-plus: `main.asm` includes `bank_C0.asm` .. `bank_FF.asm`, and
  every line carries its SNES address and bytes in a comment. Data is
  `incbin`'d from the ROM.
- `assets/`: the user's ROM, untracked (see `assets/README.md`).

## Status (2026-09-24)

The translated program boots, uploads its sound driver to an SPC700
interpreter that runs the game's own driver, initialises the field engine
and runs the intro (decompressed into WRAM and translated there), the
title, the attract scenes and the menu shell. Frames are composed on the
DSP while the game runs: the tile renderer for tile modes, the Mode 7
kernel for the intro. What CT needed beyond the shared
translator — the Diz adapter, direct-page tracking, the WRAM interrupt
trampolines and overlay, the menu's coroutine stacks, the SPC700 — is in
[analysis/ct.md](analysis/ct.md); the order of work in `ROADMAP.md`.

## Starting point (copied from F030SMK on 2026-09-24)

Translator (`tools/smkx/`), TOS runtime (`falcon/smkx_rt.s`), video
adapter and C reference renderer (`src/snes_ppu.c`, `src/smkx_video.c`),
adaptive OKLab palette (`src/palette.c`), DSP Mode 7 kernel and driver
(`falcon/dsp/m7c2p.asm`, `src/smkx_m7.c`), checks (`tools/ppu_snap.c`,
`tools/palette_test.c`, M7TEST) and scripts (run, profile, game-time).
Names keep their `smkx` origin so fixes move between the projects.
Everything SMK learnt so far, and what it means here, is in
[analysis/porting-notes.md](analysis/porting-notes.md);
`analysis/translator.md` is the SMK design document.

## Build

```sh
make ctx-source       # translation: ROM + Diz listing -> build/ctx/*.s
make ctx              # CTX.TOS
make run-ctx          # in the patched Hatari
make test-palette     # host: adaptive palette
make test-m7          # DSP Mode 7 kernel vs the C reference
make test-ppu         # DSP tile renderer vs the host model on real frames
make test-lcd         # the list compositor's DSP build (LCB) on real frames, masks of several patterns
make check-lcd-model  # host: the compositor's planner vs the reference on consecutive snapshots (make lc-snaps)
make check-lc-put     # host: the compositor's block writer (src/smkx_lc.c) on random blocks and jobs
scripts/lc_snaps.sh DIR FROM TO EVERY   # snapshots of a stretch of the game in the patched Hatari
scripts/lc_check.py DIR                 # in-game snapshots vs the host reference (compositor frames: no colour math)
make check-dsp-model  # host: the DSP renderer's plan + model vs ppu_render
make check-snap       # SNAP.BIN: Falcon planes vs host render
make test-spc         # host: the SPC700 interpreter on CT's sound driver
scripts/ctx_seed_loop.py  # run, add reported dispatch targets, repeat
scripts/ctx_snap.py build/ctx/TRAP.BIN 7E3000 40   # WRAM from a snapshot
python3 -m tools.smkx.disasm 7E5A90 30            # ROM / WRAM-program code
```

Toolchain as in F030SMK: `../../../F030Arcade/third_party` (vasm, vlink, TOS
4.04, ASM56000 for the DSP), `../../../cross-mint` (m68k-atari-mintelf-gcc) and
the patched Hatari. See `ROADMAP.md` for the order of work.

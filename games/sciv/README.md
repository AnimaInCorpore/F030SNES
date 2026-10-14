# F030SCIV

An Atari Falcon 030 port of *Super Castlevania IV* (US), following the
approach of the other SNES ports (see `AGENTS.md`): the game's 65816 program is
translated to 68030 code from the ROM, with a PPU/DSP renderer and runtime
shared with the siblings. No source listing of this game exists, so the code is
found by tracing the ROM from its vectors (`tools/sciv_trace.py`,
[analysis/trace-disassembly.md](analysis/trace-disassembly.md)).

Status: the translated game runs in the patched Hatari through the intro, the
title menu, name entry and stage 1 (see [ROADMAP.md](ROADMAP.md)). Speed on a
real Falcon has not been measured.

## Build and run

Put the ROM at `assets/Super Castlevania IV (USA).zip` (MD5
`094f035993e9724647b61ddcba1e9a7a`; see [assets/README.md](assets/README.md)).

```sh
make sciv-trace        # the tracer's listing: build/sciv_trace/
make scivx             # translate + build build/scivx/SCIVX.TOS
make run-scivx         # run it in the patched Hatari (../../../F030Arcade)
```

The default renderer sends raw VRAM pages directly over SSI/DMA; the DSP
converts and decodes them and returns the composed frame by DMA. Stage 1-1
keeps its HUD/playfield selection and indexed half-subscreen blend.
The CPU-planned list compositor remains experimental (`SCIVX_LC=1`): its
dirty planning, WRAM snapshots and page copies caused a severe stage-1
regression. Use `make check-lc` for host checks or `make test-lcd` for its
kernel checks. The remaining CPU work and verified limitations are in
[analysis/list-compositor-sciv.md](analysis/list-compositor-sciv.md).

Keyboard as joypad: arrows = D-pad, X = B, Z = Y, S = A, A = X, Q = L, W = R,
Return = Start, Space = Select, Esc = quit. For scripted input and snapshots,
`make scivx SCIVX_INPUT="3000:1000,3006:0000" SCIVX_FRAMES=4800
SCIVX_SNAP_EVERY=400`, then `make check-snaps` renders the F*.BIN snapshots.

On Windows, use MSYS2 with its `m68k-atari-mintelf-gcc` on PATH and override
`CROSS_CC` accordingly. DSP assembly accepts DOSBox-X or DOSBox Staging;
set `DOSBOX` to the executable path if it is outside PATH. For a headless
Staging build, set `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy`.
See `analysis/porting-notes.md` for the verified Windows rebuild setup.
The required patched Hatari executable is in `third_party/hatari/build/src/`
of F030Arcade (`hatari.exe` on Windows); the runner handles the Windows
build's lack of Unix command FIFOs.

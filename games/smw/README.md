# F030SMW

An Atari Falcon 030 port of *Super Mario World*, following the approach of
the sibling [F030SMK](../smk) port: the original 65816 program is
statically translated to 68030 code from the user's ROM and a pinned
disassembly, so gameplay, logic and screen design stay the original's. The
display is 256×224 in eight Videl planes (256 colours): every pixel is its
CGRAM index and the Videl palette is CGRAM, so no colours are lost.

## Sources

- `third_party/SMWDisX`: [IsoFrieze/SMWDisX](https://github.com/IsoFrieze/SMWDisX),
  pinned as a submodule (`30643c7`, 2026-08-19). Asar source for every
  release; `SMW_U.sym` holds the U.S. label addresses.
- `assets/`: the user's ROM, untracked (see `assets/README.md`).

## State (2026-09-24)

The translation runs: `make smwx` places all 62,465 SMWDisX instructions on
the ROM (every label checked against `SMW_U.sym`), translates 61,911 of them
and links SMWX.TOS. In the patched Hatari the game boots, uploads its sound
program, samples and music, and runs its title screen and attract demo
without a trap. See [analysis/smw.md](analysis/smw.md) for what SMW needed
on top of F030SMK's translator, and `ROADMAP.md` for what is left (the
renderer first: the C reference needs seconds of Falcon time per frame).

| Part | Files | State |
|---|---|---|
| Asar source walker | `tools/smkx/asar.py`, `listing.parse_smwdisx` | done, checked against the symbols |
| 65816 → 68030 translator | `tools/smkx/` | from F030SMK; LoROM, `ExecutePtr` tables, WRAM OAM routine |
| TOS runtime | `falcon/smkx_rt.s`, `smkx_rt.i` | LoROM banks, SRAM, SMW's APU handshake, keyboard pad |
| Reference renderer | `src/snes_ppu.c` | F030CT's (2026-09-30): modes 0-7, sprites, HDMA, IRQ writes, main-screen windows, the sub screen where `ppu_sub_cfg` allows it, exact colour math in `ppu_render_rgb`; LoROM bus added here. The fast composer (`ppu_fast.c`) has no windows or sub screen, `make check-snaps` reports those rows instead of failing |
| 256-colour display path | `src/ppu_fast.c`, `smkx_video.c`, `falcon/smkx_video.s` | tile-row composer + merge-network c2p, byte-exact with the reference; Mode 7 rows via the reference |
| Adaptive 16-colour palette | `src/palette.c`, `tools/smkx/oklab.py` | from F030SMK; no longer on the display path |
| DSP Mode 7 | `falcon/dsp/m7c2p.asm`, `src/smkx_m7.c`, `m7_plan.c`, `falcon/smkx_m7dsp.s` | from F030SMK |
| Reference interpreter | `tools/ref65816.c`, `scripts/smw_refcompare.py`, `smw_wramdiff.py` | the translation matches it frame for frame (demo, gameplay path) |
| Checks | `tools/ppu_snap.c`, `c2p_test.c`, `palette_test.c`, `src/m7test.c`, `falcon/m7test.s` | snapshot and c2p checks for the 256-colour path |
| List compositor (blitter) | `tools/lc/`, `falcon/lcplay.s`, `scripts/run_lcplay.py` | F030CT's model and Hatari player on SMW frames: exact on 337 consecutive level frames; no in-game producer yet ([analysis/list-compositor-smw.md](analysis/list-compositor-smw.md)) |

File and symbol names keep their `smkx` origin so fixes can move between
the two projects; `analysis/translator.md` is the SMK design document the
translator follows.

## Controls

Keyboard, by key position on a QWERTY layout (Atari scancodes): arrows =
D-pad, X = B, Z = Y, S = A, A = X, Q = L, W = R, Return = Start, Space =
Select, Esc = quit. `make smwx SMWX_INPUT="2000:1000,2008:0000"` adds
scripted input: from frame 2000 the pad word `$1000` (Start), from 2008
nothing (`tools/smkx/input_script.py`).

## Build

```sh
make smwx-source      # translation: build/smwx/*.s and report.txt
make smwx             # SMWX.TOS
make run-smwx         # in the patched Hatari
make test-palette     # host: adaptive palette
make test-m7          # DSP Mode 7 kernel vs the C reference
make test-c2p         # host: merge-network c2p vs bit-by-bit packing
make check-ref        # translation vs the reference 65816, frame for frame
make check-snap       # SNAP.BIN: fast composer vs reference, Falcon planes vs host
```

Toolchain as in F030SMK: `../../../F030Arcade/third_party` (vasm, vlink, TOS
4.04, ASM56000 for the DSP), `../../../cross-mint` (m68k-atari-mintelf-gcc) and
the patched Hatari. See `ROADMAP.md` for the order of work.

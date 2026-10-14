# F030DKC

An Atari Falcon 030 port of *Donkey Kong Country*, following the approach of
the sibling [F030SMK](../smk) port: the original 65816 program is
statically translated to 68030 code from the user's ROM and a pinned
disassembly, so gameplay, logic and screen design stay the original's. The
display is 256×224 in eight Videl planes: every pixel is its CGRAM index
and the Videl palette is CGRAM itself (from F030SMW).

Cartridge: HiROM FastROM, 4 MiB, no coprocessor.

## Sources

- `third_party/Donkey-Kong-Country-1-Disassembly`: [Donkey-Kong-Country-1-Disassembly](https://github.com/Yoshifanatic1/Donkey-Kong-Country-1-Disassembly),
  pinned as a submodule (c2080f4, 2021-11-21). Yoshifanatic1 SNES ROM
  framework v1.2.0, the same framework as SMK's disassembly; the routines
  are in `DKC1/Routine_Macros_DKC1.asm`.
- `assets/`: the user's ROM, untracked (see `assets/README.md`).

## State (2026-09-24)

`make dkcx` places all 53,624 listing instructions on the ROM, translates
50,770 of them (0 unsupported nodes, 0 traps) and links DKCX.TOS (8.4 MB).
In the patched Hatari the game boots, uploads its sound loader, driver and
samples, shows "Nintendo presents" and the intro cutscene, and with
scripted Start presses reaches the file select and the player-mode menu,
without a trap. Every snapshot matches the host: the fast composer
against the reference renderer pixel for pixel, the Falcon's planes byte
for byte. What DKC needed on top of the shared code is in
[analysis/dkc.md](analysis/dkc.md): direct-page tracking, a runtime that
takes the NMI at `WAI` (DKC runs its whole game in the NMI), table jumps
as tail calls, DKC's APU loader protocol. `ROADMAP.md` has what is left.

| Part | Files | State |
|---|---|---|
| Listing adapter | `tools/smkx/listing.py` | DKC's macros, conditionals, 64 banks; all instructions ROM-checked |
| 65816 → 68030 translator | `tools/smkx/` | from F030SMK; D register, NMI at WAI, tail-call tables |
| TOS runtime | `falcon/smkx_rt.s`, `smkx_rt.i` | HiROM 4 MiB, no word view, DKC's APU protocol, keyboard pad |
| Display | `src/smkx_video.c`, `falcon/smkx_video.s` | 256 colours, 8 planes; the DSP renderer composes, `src/ppu_fast.c` (from F030SMW) where it cannot |
| DSP renderer | `falcon/dsp/ppu.asm`, `src/smkx_ppu.c`, `src/ppu_dsp_plan.c` | from F030CT, 93 VRAM page slots; `make check-dsp-model`, `make test-ppu` |
| Reference renderer | `src/snes_ppu.c` | the checker; modes 0, 1, 7, sprites, HDMA |
| DSP Mode 7 | `falcon/dsp/m7c2p.asm`, `src/smkx_m7.c` | from F030SMK via F030CT, 4-plane output (not used: DKC has no Mode 7 scenes so far) |
| Checks | `tools/ppu_snap.c`, `tools/c2p_test.c`, `src/m7test.c` | host vs Falcon |

File and symbol names keep their `smkx` origin so fixes move between the
ports; `analysis/translator.md` is F030SMK's design document and
`analysis/porting-notes.md` collects what the other ports found.

## Controls

Keyboard, by key position on a QWERTY layout (Atari scancodes): arrows =
D-pad, X = B, Z = Y, S = A, A = X, Q = L, W = R, Return = Start, Space =
Select, Esc = quit. `make dkcx DKCX_INPUT="3600:1000,3610:0000"` adds
scripted input: from frame 3600 the pad word `$1000` (Start), from 3610
nothing (`tools/smkx/input_script.py`).

## Build

```sh
make dkcx-source      # translation: build/dkcx/*.s and report.txt
make dkcx             # DKCX.TOS
make run-dkcx         # in the patched Hatari
make test-c2p         # host: chunky-to-planar packing
make test-m7          # DSP Mode 7 kernel vs the C reference
make check-dsp-model  # host: the DSP renderer's model vs the reference, on F*.BIN
make test-ppu         # the DSP renderer in Hatari on build/dkcx/pputest/*.BIN + random frames
make check-snaps      # F*.BIN: composer vs reference, Falcon vs host
```

Switches: `DKCX_FRAMES` (quit after n frames, write SNAP.BIN),
`DKCX_SNAP_EVERY`, `DKCX_INPUT`, `DKCX_DIAG=0`
(release build). Toolchain as in F030SMK: `../../../F030Arcade/third_party`
(vasm, vlink, TOS 4.04, ASM56000 for the DSP), `../../../cross-mint`
(m68k-atari-mintelf-gcc) and the patched Hatari, which alone is used for
runs (see `AGENTS.md`).

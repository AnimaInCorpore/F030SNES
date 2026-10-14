# F030SMAS

An Atari Falcon 030 port of *Super Mario All-Stars* (Super Mario Bros.,
The Lost Levels, Super Mario Bros. 2 and Super Mario Bros. 3 behind one
title screen), following the approach of the sibling [F030SMK](../smk)
port: the original 65816 program is statically translated to 68030 code
from the user's ROM and a pinned disassembly, so gameplay, logic and screen
design stay the original's.

Cartridge: LoROM, 2 MiB, 8 KiB SRAM, no coprocessor.

## Sources

- `third_party/SMW-SMAS-SMASW-Disassembly`:
  [SMW-SMAS-SMASW-Disassembly](https://github.com/Yoshifanatic1/SMW-SMAS-SMASW-Disassembly),
  pinned as a submodule (a3c93ea, 2021-03-04). Yoshifanatic1's SNES ROM
  framework (Asar), the same framework as the SMK, DKC and Yoshi's Island
  disassemblies. The ROM map is `SMAS/RomMap/ROM_Map_SMAS_U.asm`; the code
  is in `SMAS/`, `SMB1/`, `SMBLL/`, `SMB2U/` and `SMB3/`
  (`Routine_Macros_*.asm`). The same repository also holds SMW and the
  SMAS + SMW cartridge.
- `assets/`: the user's ROM, untracked (see `assets/README.md`).

## State (2026-09-24)

Set up from F030SMW's code (commit `17baedb` there: LoROM runtime, the
256-colour display path, keyboard pad and input scripts, DMA fast paths),
which itself comes from F030SMK. Nothing is translated yet: the listing
placement for this disassembly is step 1 of `ROADMAP.md`, and the ROM is
not here yet.

| Part | Files | State |
|---|---|---|
| Listing adapter | `tools/smkx/listing.py` | SMW's (Asar walker, `parse_smwdisx`); `parse_smas` to write |
| 65816 → 68030 translator | `tools/smkx/` | from F030SMW / F030SMK |
| TOS runtime | `falcon/smkx_rt.s`, `smkx_rt.i` | LoROM, 2 MiB image, keyboard pad, input scripts |
| Display | `src/ppu_fast.c`, `smkx_video.c`, `falcon/smkx_video.s` | 256 colours, 8 planes (from F030SMW) |
| Reference renderer | `src/snes_ppu.c` | the checker; modes 0, 1, 7, sprites, HDMA |
| DSP Mode 7 | `falcon/dsp/m7c2p.asm`, `src/smkx_m7.c` | from F030SMK (SMAS has little Mode 7) |
| Checks | `tools/ppu_snap.c`, `tools/c2p_test.c`, `src/m7test.c`, `tools/ref65816.c` | host vs Falcon; reference 65816 (SMW's, to adapt) |

File and symbol names keep their `smkx` origin so fixes move between the
ports; `analysis/translator.md` is F030SMK's design document and
`analysis/porting-notes.md` collects what the other ports found.

## Controls

Keyboard, by key position on a QWERTY layout (Atari scancodes): arrows =
D-pad, X = B, Z = Y, S = A, A = X, Q = L, W = R, Return = Start, Space =
Select, Esc = quit. `make smasx SMASX_INPUT="3600:1000,3610:0000"` adds
scripted input: from frame 3600 the pad word `$1000` (Start), from 3610
nothing (`tools/smkx/input_script.py`).

## Build

```sh
make smasx-source     # translation: build/smasx/*.s and report.txt
make smasx            # SMASX.TOS
make run-smasx        # in the patched Hatari
make test-c2p         # host: chunky-to-planar packing
make test-m7          # DSP Mode 7 kernel vs the C reference
make check-snaps      # F*.BIN: composer vs reference, Falcon vs host
```

Switches: `SMASX_FRAMES` (quit after n frames, write SNAP.BIN),
`SMASX_RENDER` (0: no rendering), `SMASX_SNAP_EVERY`, `SMASX_INPUT`. Toolchain as in
F030SMK: `../../../F030Arcade/third_party` (vasm, vlink, TOS 4.04, ASM56000 for
the DSP), `../../../cross-mint` (m68k-atari-mintelf-gcc) and the patched Hatari,
which alone is used for runs (see `AGENTS.md`).

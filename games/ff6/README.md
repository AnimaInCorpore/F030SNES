# F030FFVI

An Atari Falcon 030 port of *Final Fantasy VI* (Super Famicom, Japanese
v1.0; the same game as *Final Fantasy III* on the SNES), following the
approach of the sibling [F030SMK](../smk) and [F030CT](../ct)
ports: the original 65816 program is statically translated to 68030 code
from the user's ROM and a pinned disassembly, so gameplay, logic and screen
design stay the original's. The DSP is to compose the picture as in F030CT:
tile scenes in eight Videl planes with the SNES palette as the 256 colours
(`falcon/dsp/ppu.asm`), Mode 7 scenes in four planes with 16 colours per
frame chosen adaptively in OKLab (`falcon/dsp/m7c2p.asm`), optionally the
true-colour renderer (`FF6X_TC=1`).

Cartridge (header at `$C0FFC0`): HiROM FastROM, 3 MiB image (the header says
4 MiB / 48 Mbit), 8 KiB battery SRAM, no coprocessor.

## Sources

- `third_party/ff6`: [everything8215/ff6](https://github.com/everything8215/ff6),
  pinned as a submodule (8130132, 2026-09-14). A ca65/ld65 disassembly that
  builds the Japanese v1.0 and both U.S. releases (`make LANG=jp`); the
  ROM data (graphics, text, tables, sound) is ripped from the user's own ROM
  with `make rip` and is not in the repository. The Japanese ROM it builds
  differs from the original in unused regions only (CRC32 `059728E2`
  instead of `45EF5AC8`). The code is in `src/` by module (`field`,
  `battle`, `btlgfx`, `menu`, `world`, `event`, `cutscene`, `sound`, `spc`, …);
  `cfg/ff6-jp.cfg` is the memory map. Its `tools/romtools` is a nested
  submodule that only `make rip` and the build need.
- `assets/`: the user's ROM, untracked (see `assets/README.md`).

## State (2026-09-29)

Set up from F030CT's code (its commit `359f0df`, which carries the shared
code of F030SMK, F030SMW, F030DKC and F030CT up to that day: the
translator, runtime, the DSP tile and Mode 7 renderers, the true-colour
renderer, the SPC700 interpreter, the checks and benchmarks). The
translator places the ca65 disassembly on the ROM (`listing.parse_ff6`, all
110,998 instructions checked against it) and translates 3,783 procedures. In
the patched Hatari the port boots, runs the title and the opening (intro text,
the Mode 7 Magitek march, the credits) and shows the first dialogue at the
Narshe gate, its picture equal to the host render. Not reached yet: the first
battle, the menu, the world map. `ROADMAP.md` lists what is still Chrono
Trigger's and what comes next.

| Part | Files | State |
|---|---|---|
| Listing adapter | `tools/smkx/listing.py`, `ff6src.py` | `parse_ff6`: builds the disassembly out of tree, places from ld65's debug file; CT's `parse_diz` is unused |
| 65816 → 68030 translator | `tools/smkx/` | from F030CT; runs on FF6 (`make ff6x-source`, ~11 s); CT's interrupt trampoline address and LZ still in `analyze.py` |
| TOS runtime | `falcon/smkx_rt.s`, `smkx_rt.i` | HiROM, CT's; `ct_lz.s` is CT's and off |
| Display | `src/snes_ppu.c`, `smkx_video.c`, `smkx_ppu.c`, `smkx_tc.c`, `falcon/dsp/` | from F030CT |
| Sound | `src/spc700.c`, `smkx_apu.c` | SPC700 interpreter from F030CT; FF6's driver (`src/spc`) not tried |
| Checks | `tools/`, `scripts/tc_bench/` | from F030CT; frame lists in the Makefile are CT's |

File and symbol names keep their `smkx` origin so fixes move between the
ports; `analysis/translator.md` is F030SMK's design document and
`analysis/porting-notes.md` collects what the other ports found.

## Build

```sh
make ff6x-source      # translation: ROM + listing -> build/ff6x/*.s (the first run also builds the
                      # disassembly, ~1 min: needs cc65, git and network for romtools/numpy)
make ff6x             # FF6X.TOS
make run-ff6x         # in the patched Hatari (FF6X_APU_ECHO=1 for first runs: no SPC700 driver;
                      # FF6X_INPUT="4000:0080,4010:0000" presses A on the title at frame 4000)
make test-palette     # host: adaptive palette
make test-m7          # DSP Mode 7 kernel vs the C reference
make test-spc         # host: the SPC700 interpreter on the game's sound driver
python3 -m tools.smkx.disasm 7E5000 30   # ROM / WRAM-program code
```

Toolchain as in F030SMK: `../../../F030Arcade/third_party` (vasm, vlink, TOS
4.04, ASM56000 for the DSP), `../../../cross-mint` (m68k-atari-mintelf-gcc) and
the patched Hatari, which alone is used for runs (see `AGENTS.md`). The
disassembly needs `ca65`/`ld65` (cc65) to be built. See `ROADMAP.md` for the
order of work.

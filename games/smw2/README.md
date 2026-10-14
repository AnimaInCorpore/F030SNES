# F030SMW2

An Atari Falcon 030 port of *Super Mario World 2: Yoshi's Island*, following
the approach of the sibling ports ([F030SMK](../smk),
[F030SMW](../smw)): the original 65816 program is statically
translated to 68030 code from the user's ROM and a pinned disassembly, so
gameplay, logic and screen design stay the original's. The cartridge's
SuperFX (GSU-2) program is translated too, to C, with an interpreter as
the reference. The display is 256×224 in eight Videl planes (256 colours,
the Videl palette is CGRAM), taken over from F030SMW.

## Sources

- `third_party/Yoshi-s-Island-Disassembly`: [Yoshi-s-Island-Disassembly](https://github.com/Yoshifanatic1/Yoshi-s-Island-Disassembly),
  pinned as a submodule (7be13eb, 2021-06-21), Yoshifanatic1 SNES ROM
  framework v1.1.0. The 65816 code is in `YI/Routine_Macros_YI.asm`, the
  SuperFX code in `YI/SuperFX/SuperFXCode_YI.asm`.
- `assets/`: the user's ROM, untracked (see `assets/README.md`).

## State (2026-10-01 addendum, see analysis/list-compositor-smw2.md)

The default build (`YIX_GSU=xlat`) stops at game mode 7 (the translated SuperFX code differs from the core at run 3); `make yix YIX_GSU=interp`
runs through the logo, intro and title to the file menu and into game mode `$38`, where it waits on the APU port `$2140` (no sound
CPU yet). The DSP renderer and the list compositor are in; the compositor covers only the logo frames.

## State (2026-09-24)

The translated game boots in the patched Hatari: it uploads its sound
program (echo model, no sound yet), runs its SuperFX code, takes NMIs and
IRQs and reaches game mode 7 with the main loop running, without traps.
See [analysis/yi.md](analysis/yi.md) for what YI needed and
[ROADMAP.md](ROADMAP.md) for what is next (SuperFX speed, then the
picture).

| Part | Files | State |
|---|---|---|
| Listing placement | `tools/smkx/listing.py` (`parse_yi`) | all 165,229 instructions ROM-checked |
| 65816 → 68030 translator | `tools/smkx/` | from games/smk/F030SMW; D register, WRAM code, YI's jump idioms (`jumps.txt`) |
| SuperFX core (reference) | `src/gsu.c`, `src/gsu.h`, `src/yix_gsu.c` | runs YI's boot and loading work |
| SuperFX → C translator | `tools/smkx/gsu2c.py` | 48,333 nodes, being checked against the core |
| TOS runtime | `falcon/smkx_rt.s`, `smkx_rt.i` | YI's memory map, GSU registers and RAM, interrupts |
| Display path | `src/ppu_fast.c`, `src/smkx_video.c`, `src/snes_ppu.c`, `falcon/smkx_video.s` | from F030SMW (256 colours): the exact DSP tile renderer (`falcon/dsp/ppu.asm`, `src/smkx_ppu.c`) draws the game, the CPU composer is the fallback |
| List compositor | `src/lc_plan.c`, `src/smkx_lc.c`, `falcon/smkx_lcput.s`, `tools/lc/` | from F030SMW; covers only YI's logo frames so far: [analysis/list-compositor-smw2.md](analysis/list-compositor-smw2.md) |

File and symbol names keep their `smkx` origin so fixes move between the
ports; `analysis/translator.md` is the SMK design document and
`analysis/porting-notes.md` collects what the other ports found.

## Build

```sh
make yix-source           # translation: build/yix/*.s and report.txt
make yix                  # YIX.TOS (YIX_GSU=xlat by default)
make yix YIX_GSU=check    # translated and interpreted GSU, compared per run
make yix YIX_GSU=interp   # interpreter only
make run-yix              # in the patched Hatari
make test-gsu             # host: one SuperFX routine through the core
make test-ppu test-lcd    # the DSP renderer and the compositor's program in Hatari (real YI frames in a snapshot directory, SNAPS=)
make check-dsp-model check-lcd check-lc-model check-lc-play   # host models on snapshots (analysis/list-compositor-smw2.md)
make check-snap           # SNAP.BIN: fast composer vs reference, Falcon planes vs host
```

`SMKX_CHECK_STACK=1 make yix-source` adds a stack-balance check at every
call (debugging). Toolchain as in F030SMK: `../../../F030Arcade/third_party`
(vasm, vlink, TOS 4.04, ASM56000 for the DSP), `../../../cross-mint`
(m68k-atari-mintelf-gcc) and the patched Hatari.

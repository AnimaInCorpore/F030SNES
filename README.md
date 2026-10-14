# F030SNES

Super Nintendo games on the Atari Falcon030: each game's 65816 program is
statically translated to 68030 code from the user's ROM and a pinned
disassembly (or, where none exists, a trace of the ROM), so gameplay, logic
and screen design stay the original's. The SNES picture is composed by the
Falcon's DSP56001 (tile modes in eight Videl planes with CGRAM as the 256
colours, Mode 7 in four planes with 16 colours chosen per frame in OKLab), the
sound program runs on an SPC700 interpreter.

This project is developed with AI coding agents (Claude Code): much of the
translators, runtimes, tools and documentation was written by them under the
author's direction. Correctness is checked by testing rather than by trust:
the translation is placed and checked instruction by instruction against the
listing, and the Falcon pictures are compared byte for byte with a host
reference renderer.

| game | directory | cartridge | state |
|---|---|---|---|
| Super Mario Kart | [games/smk](games/smk/README.md) | LoROM + DSP-1 | title screen (the origin of the shared translator, runtime and renderers) |
| Super Mario World | [games/smw](games/smw/README.md) | LoROM | title screen and attract demo |
| Super Mario World 2: Yoshi's Island | [games/smw2](games/smw2/README.md) | LoROM + SuperFX 2 | logo, intro, title, file menu (`YIX_GSU=interp`) |
| Donkey Kong Country | [games/dkc](games/dkc/README.md) | HiROM | intro, file select, player-mode menu |
| Chrono Trigger | [games/ct](games/ct/README.md) | HiROM | intro, title, attract scenes, menu shell |
| Final Fantasy VI (Japan v1.0) | [games/ff6](games/ff6/README.md) | HiROM | title, opening, first dialogue at Narshe |
| Super Mario All-Stars | [games/smas](games/smas/README.md) | LoROM | set up; translation not started |
| Super Castlevania IV | [games/sciv](games/sciv/README.md) | LoROM | intro, title, name entry, stage 1 |

The states are measured in the patched Hatari (see below); speed on a real
Falcon has not been measured for most of them. Each game's `README.md`,
`ROADMAP.md` and `analysis/` have the details.

## Download

Releases are packed with `make release VERSION=<v>` (see
[Release](#release)): one self-contained TOS program per game, the game's code
and data built in, and a `README.TXT` with the controls. Requirements: a
Falcon030 with 14 MB of RAM (or Hatari).

The ROMs are not part of this repository. To build from the sources, put the
ROM named in each game's `assets/README.md` into `games/<game>/assets/`.

## Controls

The same for every game: the keyboard is pad 1, by key position on a QWERTY
layout (Atari scancodes): cursor keys = D-pad, X = B, Z = Y, S = A, A = X,
Q = L, W = R, Return = Start, Space = Select, Esc = quit.

## Layout

    games/<game>/        one folder per port, all alike:
      README.md          state, controls, build commands
      ROADMAP.md         what is left, in order
      AGENTS.md          the port's working rules
      Makefile           <game>x-source (translation), <game>x (the TOS), run-<game>x
      tools/smkx/        the 65816 -> 68030 translator (shared code, one copy per port)
      falcon/            the TOS runtime (smkx_rt.s), video, DSP programs (falcon/dsp/)
      src/               reference renderer (snes_ppu.c), fast composer, palette, DSP driver
      scripts/           Hatari runners, profilers, benchmarks
      analysis/          findings; porting-notes.md collects what the other ports found
      third_party/       the pinned disassembly (git submodule)
      assets/            the user's ROM (untracked; assets/README.md)
      build/             generated code and the .TOS (untracked)
    tools/release.py     packs a release
    Makefile             build / run / release for all games

The translator, runtime and renderer started in Super Mario Kart and were
copied into each later port; each copy has moved on with its game. Merging
them into one shared engine is planned; until then a fix to shared code goes
into the port it was found in and is recorded for the others (see
[AGENTS.md](AGENTS.md)).

## Requirements

- macOS or Linux with `make`, a C compiler and Python 3.
- A checkout of [F030Arcade](https://github.com/AnimaInCorpore/F030Arcade)
  next to this one (`../F030Arcade`): its `third_party/` provides vasm,
  vlink, the DSP56000 assembler, TOS 4.04 and the patched Hatari that every
  run, screenshot and measurement uses.
- An m68k-atari-mintelf cross toolchain in `../cross-mint/bin/` (or set
  `CROSS_CC`), for the C parts of the runtime.
- The submodules: `git submodule update --init`.

## Quick start

```sh
git submodule update --init
make list                 # the games and their targets
make smw                  # translate and build games/smw/build/smwx/SMWX.TOS
make run-smw              # run it in the patched Hatari
make all                  # build every game
```

## Release

```sh
make release VERSION=0.1  # release/F030SNES-0.1/ and release/F030SNES-0.1.zip
```

`tools/release.py` builds every game with its default settings, checks that
each program is a TOS executable, and writes a `README.TXT` with the
controls, each game's state and the SHA-256 of every file.

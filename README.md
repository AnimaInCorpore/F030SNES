# F030SNES

Super Nintendo games on the Atari Falcon030: each game's 65816 program is
statically translated to 68030 code from the user's ROM and a pinned
disassembly (or, where none exists, a trace of the ROM), so gameplay, logic
and screen design stay the original's. The SNES picture is composed by the
Falcon's DSP56001 (tile modes in eight Videl planes with CGRAM as the 256
colours, Mode 7 in four planes with 16 colours chosen per frame in OKLab), the
sound program runs on an SPC700 interpreter.

**Highly experimental.** In their current form the ports are far too slow to
play, the rendering above all: they show how far each translation gets, not
a game to sit down with.

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

**Screenshots and trademarks.** The pictures in `games/smk/docs/` show Super
Mario Kart's graphics, © 1992 Nintendo. The game titles named here are
trademarks of their owners: Nintendo (Super Mario Kart, Super Mario World,
Yoshi's Island, Donkey Kong Country, Super Mario All-Stars), Square Enix
(Chrono Trigger, Final Fantasy VI) and Konami (Super Castlevania IV). This
project is not affiliated with or endorsed by any of them.

## Download

Releases are packed with `make release VERSION=<v>` (see
[Release](#release)): one TOS program per game and a `README.TXT`. No game is
included: bring your own ROM. Each program reads it from next to itself at
start (`SMW.SFC` for `SMWX.TOS` and so on; a `.SMC` with a copier header
works too; Super Mario Kart also reads the cartridge's DSP-1 dump as
`DSP1.BIN`), checks it against the exact release it was translated from, and
says on its info screen when a file is missing or wrong. Requirements: a
Falcon030 with 14 MB of RAM (or Hatari).

The ROMs are not part of this repository either. To build from the sources,
put the ROM named in each game's `assets/README.md` into
`games/<game>/assets/`.

## Controls

The same for every game: the keyboard is pad 1, by key position on a QWERTY
layout (Atari scancodes): cursor keys = D-pad, X = B, Z = Y, S = A, A = X,
Q = L, W = R, Return = Start, Space = Select, Esc = quit. Every program
shows them on its info screen at start, with the game's state and the check
of the player's files.

## Layout

    games/<game>/        one folder per port, all alike:
      README.md          state, controls, build commands
      ROADMAP.md         what is left, in order
      AGENTS.md          the port's working rules
      Makefile           <game>x-source (translation), <game>x (the TOS), run-<game>x
      tools/smkx/        the 65816 -> 68030 translator (shared code, one copy per port)
      falcon/            the TOS runtime (smkx_rt.s), video, DSP programs (falcon/dsp/);
                         smkx_info.s: the info screen, reads and checks the ROM (alike in all ports)
      src/               reference renderer (snes_ppu.c), fast composer, palette, DSP driver
      scripts/           Hatari runners, profilers, benchmarks
      analysis/          findings; porting-notes.md collects what the other ports found
      third_party/       the pinned disassembly (git submodule)
      assets/            the user's ROM (untracked; assets/README.md)
      build/             generated code and the .TOS (untracked)
    tools/games.py       the games: titles, states, the player's files, release settings
    tools/port_info.py   writes each build's info.i (info texts, file names, checksums)
    tools/release.py     packs a release
    Makefile             build / run / release for all games

The translator, runtime and renderer started in Super Mario Kart and were
copied into each later port; each copy has moved on with its game. Merging
them into one shared engine is planned; until then a fix to shared code goes
into the port it was found in and is recorded for the others (see
[AGENTS.md](AGENTS.md)).

## Requirements

- macOS or Linux with `make`, a C compiler and Python 3.
- The tools in `../F030Arcade/third_party/`, where the Makefiles look for
  them (F030Arcade's development tree is not public yet; the tools are):
  - `vasm/vasmm68k_mot` and `vlink/vlink`: [vasm](https://github.com/Leffmann/vasm)
    and [vlink](https://github.com/Leffmann/vlink);
  - `asm56k/`: Motorola's DSP56000 assembler (`ASM56000.EXE`, `CLDLOD.EXE`,
    `DOS4GW.EXE`, `ioequ.inc`), your own copy, run under
    [DOSBox-X](https://dosbox-x.com/) (`tools/smkx/dsp_asm.py`);
    [ASM56000](https://github.com/AnimaInCorpore/ASM56000) is a portable
    reconstruction of it;
  - `hatari/build/`: the patched Hatari that every run, screenshot and
    measurement uses, [AnimaInCorpore/hatari](https://github.com/AnimaInCorpore/hatari);
  - `tos/tos404.img`: a TOS 4.04 image, your own (it is Atari's and not
    distributed here).
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

`tools/release.py` builds every game with its default settings (plus the
make variables in `tools/games.py`), the info screen held for 10 seconds and
the version in its first line. It checks that each program is a TOS
executable and that no 32-byte block of the ROM (or the DSP-1 dump) occurs in
it, and writes a `README.TXT` with the player's files, the controls, each
game's state and the SHA-256 of every program. The translated program is
derived from the game's code, but none of the game's bytes are in it.

A local build shows the info screen only for a moment; `INFO_WAIT=<seconds>`
on any game's `make` holds it (`make -C games/smw smwx INFO_WAIT=10`).

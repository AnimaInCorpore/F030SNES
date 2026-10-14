# AGENTS.md

Working agreement for AI agents (and humans) in this repository. These rules
are mandatory. Each port also has its own `games/<game>/AGENTS.md`; read it
before working in that port. Where the two differ, the port's file is more
specific and wins for that port.

## Repository

- This repository is the development home of the SNES ports. They were
  developed in separate repositories (F030SMK, F030SMW, F030SMW2, F030DKC,
  F030CT, F030SMAS, F030FFVI, F030SCIV) until 2026-10-14 and were imported
  here with a fresh history; those repositories are frozen. Do not develop
  in them.
- The repository is prepared for publication. Never commit a ROM, anything
  extracted or derived from one (graphics, samples, translated code, ripped
  tables, snapshots, captures), or a build product. They live in
  `games/<game>/assets/` and `games/<game>/build/`, both ignored. Check
  `git status` before every commit for files that should not be there.
- The disassemblies are submodules under `games/<game>/third_party/`, pinned.
  Change a pin only on purpose and say so in the commit message and the
  game's README.

## Shared code

Every port carries its own copy of the shared code (translator
`tools/smkx/`, runtime `falcon/smkx_*`, renderers and DSP programs,
`src/snes_ppu.c` and the scripts). Until the copies are merged into one
engine:

- Fix shared code in the port where the problem was found, then record the
  fix in `analysis/porting-notes.md` of every *other* port, as described in
  the ports' `AGENTS.md` ("Sharing findings between the SNES ports"). The
  sibling paths there (`../smk`, `../ct`, ...) are relative to
  `games/<game>/`.
- Do not change another port's code as a side effect; each port takes fixes
  over itself, with its own Hatari check.
- Other sessions may work in other ports at the same time. Re-read a notes
  file right before editing it and append rather than rewrite. Never
  `pkill`/`pgrep` Hatari by pattern; stop only your own PIDs.

## Tools and emulator

- The toolchain and emulator come from `../F030Arcade/third_party` (vasm,
  vlink, ASM56000, TOS 4.04, the patched Hatari) and `../cross-mint`. Use
  only the patched Hatari for runs, screenshots, debugging and
  measurements; never a system or Homebrew Hatari.
- Keep emulator timing apart from measurements on a real Falcon. Name the
  Hatari build beside every throughput figure.
- Hatari workflows and debugging recipes are in F030Arcade's `DEBUGGING.md`;
  general porting findings in its `PORTING.md`.

## Documentation

- Keep the docs in step with every change, in the same commit: build or run
  commands in `games/<game>/README.md`, the plan in `ROADMAP.md`, findings
  and measurements in `analysis/`. The game table in the root `README.md`
  and the states in `tools/release.py` follow each game's state.
- A finding that helps every SNES port goes into the other ports'
  `analysis/porting-notes.md`; one that helps any Falcon port also goes to
  F030Arcade's `PORTING.md` or `DEBUGGING.md`.

## Release

- `make release VERSION=<v>` builds every game with its default settings and
  packs `release/F030SNES-<v>.zip`. A game is in the release only when
  `tools/release.py` lists it; add it there with an honest state line.
- Publishing a release or changing the repository's visibility needs the
  owner's explicit approval.

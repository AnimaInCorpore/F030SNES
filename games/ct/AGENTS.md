# Project working rules

- Use **only** the patched Hatari at
  `../../../F030Arcade/third_party/hatari/build/src/hatari` for emulator runs,
  screenshots, debugging and performance comparisons. Never substitute a
  system/Homebrew Hatari. The run targets use this exact binary and fail
  when it is missing.
- Keep emulator timing distinct from measurements on a physical Falcon. Record
  the Hatari build identity beside any throughput figure.
- The ROM and everything derived from it (`assets/*`, `build/`) stay
  untracked.
- `src/snes_ppu.c` is the reference pixel/byte implementation for Falcon
  and DSP work (`ppu_m7_pixels()` for the Mode 7 kernel). Update the
  snapshot and kernel tests when changing planar or snapshot bytes.

## Sharing findings between the SNES ports

Eight Falcon ports follow the same approach, each with its own copy of the
shared code (translator `tools/smkx/`, runtime `falcon/smkx_*`, renderer,
palette and DSP driver in `src/`, the DSP kernel, the scripts):

- `../smk`: Super Mario Kart, the origin of the shared code
- `../smw`: Super Mario World
- `../smw2`: Super Mario World 2: Yoshi's Island, SuperFX 2
- `../dkc`: Donkey Kong Country
- `../ct`: Chrono Trigger (this project)
- `../smas`: Super Mario All-Stars (SMB1, Lost Levels, SMB2, SMB3)
- `../ff6`: Final Fantasy VI, Japanese v1.0 (HiROM, 3 MiB, no coprocessor)
- `../sciv`: Super Castlevania IV (LoROM, 1 MiB, no coprocessor)

- Read this project's `analysis/porting-notes.md` at the start of a
  session: it collects what the other ports have found.
- Whenever something turns up that helps porting — a bug in shared code,
  a listing or disassembly pitfall, a hardware or runtime behaviour, a
  diagnosis method, a measurement that changes plans — add an entry to
  `analysis/porting-notes.md` in every *other* port. An entry says where
  and when it was found, what happened and how it was recognised, the
  fix and the files it changed, whether that port's copy already has it
  ("in this copy" / "not yet in this copy: take over <files>"), and what
  it means for that game in particular, if anything.
- Fix shared code here and name the changed files in the entry; each port
  takes fixes over itself. Change other ports' notes only, not their
  code, and don't let copies drift silently.
- Other sessions may be working in the other ports at the same time:
  re-read a notes file right before editing it and add to it rather than
  rewriting it. They also run Hatari on this machine: never `pkill`/`pgrep`
  by pattern (`run_smkx`, `hatari`); stop and wait for your own PIDs only.

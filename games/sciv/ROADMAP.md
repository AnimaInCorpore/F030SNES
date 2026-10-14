# F030SCIV roadmap

1. (Done 2026-10-01, see `analysis/source-verification.md`: ROM verified, no full
   disassembly exists, so placement is by tracing from the vectors.) Verify the ROM (US, LoROM, 1 MiB) and pick a version-matched disassembly
   or listing to place the program on; record it in
   `analysis/source-verification.md`.
2. (Done 2026-10-01) Copy the shared code from the most recent sibling (F030FFVI): translator
   `tools/smkx/`, runtime `falcon/smkx_*`, `src/` renderer/palette/DSP
   driver, DSP kernel, scripts, `Makefile`. Strip the game-specific parts.
3. Adapt for LoROM and no coprocessor; Mode 7 (rotating rooms, tunnels) uses
   the DSP Mode 7 kernel.
4. Run to the title screen, then stage 1, comparing against reference
   snapshots.

## State of the copied code (2026-10-01)

Taken from F030FFVI's committed HEAD `38e3647` (its working tree had
uncommitted edits to the Makefile, `falcon/smkx_rt.s`, `falcon/smkx_video.s`,
`falcon/smkx_m7dsp.s`, `falcon/dsp/ppu.asm`, `src/smkx_ppu.*`,
`src/ppu_dsp.h`; they are not in this copy). Removed: FF6/CT-only files
(`ff6_lz.py`, `ff6src.py`, `ct_lz.py`, `falcon/ct_lz.s`, `scripts/ff6x_*`,
`tools/ff6x_run.py`); the address lists in `tools/smkx/*.txt` are emptied.
Makefile targets are renamed `scivx*`. Still FF6-shaped and to be replaced:

- `tools/smkx/listing.py` `parse_ff6`, `__main__.py` (ROM path, `LISTING`,
  the `ff6.sfc` output name, which the Makefile and C tools still use),
  `analyze.py` (HiROM mapping and `WRAM_OVERLAYS`), `disasm.py`,
  `input_script.py`, and the `-DCT_LZ*` flags in the Makefile (unused here).
- `GAME_SOURCE` in the Makefile points at a placeholder path until a
  disassembly is chosen (step 1).
- LoROM needs its own bank/address mapping in the translator and runtime.

## Status 2026-10-01 (later)

The translated game runs in Hatari to the intro screen (the tombstone). Fixes
that got it there: run-time resolution of `lda $BB0000,x`, an idle mark at the
NMI wait (`$008550`), and a re-entrant NMI in the runtime (see
`analysis/porting-notes.md`, "From F030SCIV"). Next: input and the title menu,
the tracer's open problems (`analysis/trace-disassembly.md`), speed.

## Status 2026-10-01 (evening)

With a scripted Start press (`make scivx SCIVX_INPUT="3000:1000,3006:0000,..."`,
`SCIVX_SNAP_EVERY=400`, `make check-snaps`) the translated game shows, in Hatari
and rendered by the host reference identically: the intro, the title menu
(START / CONTINUE / OPTION), the name-entry screen and stage 1 at the castle
gate. About 4,450 frames in, the game switches to BG mode 7 and only sprites
(Simon, moon, candles) show on a dark background; whether that is the game's own
scene or a missing Mode 7 layer is not known without a reference emulator.
`make build/s9x_ref` fetches snes9x 1.63 from GitHub for that (not done yet).
Also fixed: `jsr ($00EC,x)` (a RAM pointer) is dispatched at run time.

## Resolved: the black "Mode 7" scene (2026-10-01)

DSP compositor update: a captured initial stage 1-1 frame sets Mode 1 at
visible row 0, adds BG1/BG2 at row 31 and enables half-subscreen addition
at row 191. Its initial Mode 7 register is not a visible Mode 7 region.
The fixed LCW path now covers these changes; genuine mixed tile/Mode 7
frames use disjoint LC/LC7 passes. See analysis/list-compositor-sciv.md.

It was stage 1-1 with its HDMA channels never programmed: `sub_008263` fills the
DMA channel registers with `sta $00,x` where D = 0 and X = `$43x0` (a full 16-bit
address through the direct page), which the emitter turned into WRAM stores. The
sites are listed in `tools/smkx/wide_index.txt` and resolved at run time. The
frame (HDMA switching `$2105`/`$212C` per scanline) now matches snes9x. The
reference tooling that found it: `build/s9x_ref` with scripted input
(`S9X_INPUT`, keyed by NMIs since the game enabled them), VRAM/WRAM dumps and a
per-frame PPU write trace (a patched local snes9x under `build/`), and
`scripts/s9x_lock.py`, which runs port and snes9x on the same input and
compares the game's state variables at equal NMI counts.

## DSP list compositor (2026-10-01)

Integrated CT's retained-ring compositor, including the LCW window kernel,
and initially enabled it by default. Host, snapshot, live checker and actual DSP
kernel checks pass; see [analysis/list-compositor-sciv.md](analysis/list-compositor-sciv.md).
The sampled later attract scenes still use fallback for colour math and
ring-layer per-row scroll. Extending that coverage and measuring game and
display time separately remain necessary before claiming a gameplay speed-up.

Stage-1 runtime comparisons exposed a severe CPU-planning regression despite
fewer composed groups. The default now uses whole-frame DSP rendering
(`SCIVX_LC=0`) and direct raw-VRAM page DMA, with byte-order conversion and
Mode 7 packing on the DSP. The list compositor remains experimental. CPU
PPU replay/page discovery and DSP-owned retained dirty planning remain work;
this is not yet the complete graphics work-sharing target in AGENTS.md.

## Next: bit-flag change tracking (2026-10-02)

Rule recorded in `AGENTS.md` ("The CPU shall not read, write or copy larger
data"): tile-map and character-data changes are tracked as dirty bit arrays
set by the VRAM write hook and by DMA descriptors, and the bitmaps/descriptors
(not data) go over SSI/DMA to the DSP, which owns the VRAM mirror and dirty
planning. First step: survey every runtime path where the CPU reads or copies
VRAM-bound data, and check that the packed VRAM mirror fits in DSP memory.

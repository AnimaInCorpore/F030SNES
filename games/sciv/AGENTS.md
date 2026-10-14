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

## Graphics work sharing between CPU and DSP

- The target graphics data flow is **raw VRAM -> SSI/DMA -> DSP -> SSI/DMA
  -> RAM composition result -> CPU walks result -> screen buffer**.
- The DSP owns graphics computation: tile/sprite decoding, graphics planning,
  dirty-region decisions, visibility and occlusion, priority and window masks,
  colour/effect work, and composition. Do not add these calculations to the
  CPU to reduce the DSP's drawing count; the CPU is already at its limit.
- CPU graphics work is limited to unavoidable control and transfer setup,
  walking the DSP's composition outcome, and plainly drawing/copying or
  clearing the specified data in the screen buffer. The walker executes the
  result; it must not recompute which pixels, tiles, layers or effects belong
  there. Use DMA/blitter for transfers where applicable.
- Avoid memory copies at all cost beyond those terminal draw/clear operations
  and transfers that cannot be avoided. Send raw VRAM from its existing buffer;
  perform byte-order conversion and packing on the DSP. Do not introduce CPU
  staging buffers, bulk WRAM/VRAM snapshots, framebuffer copies or page-mirror
  repairs as routine parts of graphics planning. Explain any unavoidable copy
  and its measured cost.
- **The CPU shall not read, write or copy larger data at all** where it can be
  avoided: the Falcon's 68030 caches are tiny (256 B each) and bulk accesses
  thrash them. Track change with **bit-sized arrays, not data**:
  - Set dirty bits when the game writes tile maps or character data. The
    VRAM write hook sets a bit per entry/block; a DMA to VRAM records one
    descriptor (source, VRAM destination, length) and sets a bit *range*
    with word-wide operations. The CPU does not copy or scan the data, and
    never diffs VRAM against a shadow copy.
  - The bitmaps and descriptors are what travels over SSI/DMA, in place of
    data. The DSP owns the VRAM mirror, the bitmaps and the dirty-group
    planning. Pick a block granularity that justifies a DMA setup (likely
    256 B–1 KiB, not one tile) and coalesce descriptors into few contiguous
    ranges: sound DMA is a single stream and shares the engine with audio.
  - A character-data change dirties every map cell using that tile: resolve
    it on the DSP (reverse lookup) or fall back to a coarser region. Scroll,
    mode and base-address changes are not map writes and need their own
    flags; a map base change invalidates its whole range.
  - Clear bits only after the DSP has consumed them; with queued jobs,
    double-buffer or tag the bitmaps per job.
  - Open checks: whether the VRAM mirror fits in packed DSP memory alongside
    the Mode 7 plane and tables, and which runtime paths still read or copy
    VRAM-bound data on the CPU (the list this work must eliminate).
- Preserve the game's layer selection, windows, priorities and effects.
  Performance work must not silently disable them. Check against the reference
  renderer and measure total CPU/transfer/DSP/presentation cost; fewer drawn
  groups alone do not establish a speed-up.
- This is the implementation target, not a claim that the current renderer
  already meets it. Identify remaining CPU graphics computation and copying
  explicitly when documenting or changing the renderer.

## Sharing findings between the SNES ports

Eight Falcon ports follow the same approach, each with its own copy of the
shared code (translator `tools/smkx/`, runtime `falcon/smkx_*`, renderer,
palette and DSP driver in `src/`, the DSP kernel, the scripts):

- `../smk`: Super Mario Kart, the origin of the shared code
- `../smw`: Super Mario World
- `../smw2`: Super Mario World 2: Yoshi's Island, SuperFX 2
- `../dkc`: Donkey Kong Country
- `../ct`: Chrono Trigger
- `../smas`: Super Mario All-Stars (SMB1, Lost Levels, SMB2, SMB3)
- `../ff6`: Final Fantasy VI, Japanese v1.0 (HiROM, 3 MiB, no coprocessor)
- `../sciv`: Super Castlevania IV, US (LoROM, 1 MiB, no coprocessor; this project)

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

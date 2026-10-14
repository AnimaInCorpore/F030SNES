# F030SMW roadmap

Done (2026-09-24, details in analysis/smw.md):

1. SMWDisX listing adapter (`tools/smkx/asar.py`, `listing.parse_smwdisx`).
2. LoROM in the translator, runtime and HDMA bus; DSP-1 parts dropped.
3. `OAMResetRoutine` (code built in WRAM) as a runtime helper;
   `JSL ExecutePtr(Long)` tables as static table jumps.
4. First runs: boot, SPC uploads, title screen and attract demo, no traps.
5. Input: keyboard as pad 1, scripted input for tests.

6. DMA fast paths (VRAM, OAM, CGRAM); forced-blank frames not composed.
7. Scripted input reaches a level: title -> file select -> player select ->
   intro level (`GameMode` $14), no trap.

Next, in order:

1. **Display speed.** The 256-colour path (below) is byte-exact and has a
   tile-row cache; it needs about 0.74 s of Falcon time per full frame
   (composing ~0.59 s, c2p ~0.15 s, emulated). Next: 68030 assembly for
   the row decoder/painter and c2p (the C code spills registers to the
   ST-RAM stack), then composing and c2p on the DSP (its own zero-wait RAM;
   VRAM upload by dirty pages as for Mode 7) with the planes streamed over
   the SSI.
2. **Game logic speed.** Done for now: the precise flag liveness (checked
   against the reference interpreter) brings level scenes to 44-50 game
   frames per 50 VBLs with rendering off. Further: `rt_note_caller` on every
   generic memory access, fast paths for `[dp],Y` pointer accesses.
3. **Verification.** Done: `tools/ref65816.c` and `make check-ref`. Extend
   the scripted paths (overworld, more levels, Mode 7 boss rooms) as the
   port reaches them; every translator change should pass `check-ref`.
4. **PPU**: windows (circle and keyhole transitions via HDMA, clip to
   black), colour math (translucent layer 3), mosaic, per-line CGRAM.
5. **Sound**: the SPC700 engine and BRR samples (deferred as in F030SMK).

## Display path (decided 2026-09-24: 256 colours, 8 planes)

Videl in eight bitplanes; the Videl palette is CGRAM scaled by INIDISP
brightness, loaded at the VBL that shows the new screen. `src/ppu_fast.c`
composes rows of CGRAM indices a character row at a time (layers painted
back to front in priority order, sprites from per-frame row buckets) and
packs 32 pixels at a time into planes (`ppu_c2p8`). `tools/ppu_snap.c`
checks every snapshot byte for byte against the reference renderer with an
identity map. Not yet: per-line CGRAM changes (HDMA), Mode 7 rows on the
DSP (they fall back to the reference), windows and colour math.

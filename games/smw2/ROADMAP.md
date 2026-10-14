# F030SMW2 roadmap

Done (2026-09-24, details in analysis/yi.md):

1. Listing placement (`parse_yi`): every instruction ROM-checked.
2. Analysis for YI: SuperFX memory map, WRAM code, the D register,
   inline-argument calls, push-and-return and pointer jumps, parked return
   addresses; contexts reduced to one per entry.
3. SuperFX core (`src/gsu.c`) and the synchronous GSU protocol in the
   runtime; the GSU RAM as host memory.
4. First runs: boot to game mode 7 without traps.
5. SuperFX → C translator (`tools/smkx/gsu2c.py`) with the interpreter as
   fallback and checker.

Next, in order:

0. **State 2026-10-01** (analysis/list-compositor-smw2.md): the game draws through the DSP stack and the list compositor (taken over from F030SMW).
   With `YIX_GSU=xlat` (default) it stops at game mode 7 with a stack jump to `$000FCACB`; **that is the translated SuperFX code, not
   `jumps.txt`**: `YIX_GSU=check` reports the first difference at run 3 and `YIX_GSU=interp` runs the same game through the logo, the
   intro and the title (Start held 100 frames) to the file menu at frame ~29,000 and into game mode `$38` at ~33,000, where it waits
   for ever on the APU port `$2140` (the level's sound upload: the echo model does not answer; F030CT's SPC700 interpreter is the way,
   item 5). No level has been seen.
1. **GSU check.** Run with `YIX_GSU=check` until every GSU call matches
   the interpreter over boot, title and a level; then make `xlat` the
   default path for speed and measure.
2. **Scripted input** to reach the title and a level (F030SMW's
   `input_script.py` is in place); the joypad `$4016` rule from F030DKC.
3. **Picture.** Check snapshots against the host render (`check-snaps`);
   YI uses mode 1 with colour math, windows and HDMA effects heavily, and
   mode 7 in a few places; take over F030SMW's composer work as it lands.
4. **Speed of the 65816 side**: the V-only CCR saves (195k of 391k
   nodes), F030SMK's page tables and dispatch cache.
5. **Sound**: F030CT's SPC700 interpreter runs the game's own driver (the
   same Nintendo driver family); audio output later.

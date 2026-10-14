# F030FFVI roadmap

Set up on 2026-09-29 from F030CT's code (commit `359f0df`, all shared fixes
up to that day). The ROM is in `assets/`; nothing is translated yet.

What the ROM and the disassembly tell so far:

- HiROM FastROM, no coprocessor, 8 KiB SRAM; the image is 3 MiB
  (`$C00000-$EFFFFF`, `cfg/ff6-jp.cfg`), the header claims 4 MiB.
- Reset at `$C0FF00` (`src/field/header.asm`); the NMI and IRQ vectors are
  `JML $001500` and `JML $001504` at `$C0FF10`/`$C0FF14`, so the engines
  install their handlers in low WRAM as F030CT's do (there `$000500`).
- The field engine (`src/field`, bank C0), the battle engine (`src/battle`,
  C2; graphics `src/btlgfx`, C1), the menu (`src/menu`, C3), the world map
  (`src/world`), the event and cutscene code (`src/event`, `src/cutscene`;
  the cutscene program is LZ-compressed in C2 and runs from WRAM at
  `$7E5000`, `cutscene_wram` in the linker config, like F030CT's intro).
  Sound is an SPC700 driver (`src/spc`, `cfg/ff6-spc.cfg`) uploaded to the
  APU at boot; the ROM's LZ is `decompress_code` at `$C2FC6D`
  (`tools/ff6_lzss.py` reads it).

Done (2026-09-29):

- **Listing placement**: `listing.parse_ff6` (`tools/smkx/listing.py`) with
  `tools/smkx/ff6src.py`. The disassembly is built out of tree
  (`build/ff6src`: a copy of the submodule, `tools/romtools` cloned at the
  commit the submodule records, numpy in `build/venv` when the Python in
  use lacks it, `extract_assets.py`, `make LANG=jp`, ~1 minute, once per
  disassembly commit and ROM) and the placement is read from the debug
  file ld65 writes: every source line that produced bytes, with segment and
  offset of each piece, and every label with its value and the lines that
  mention it. Lines are classified by their text (mnemonic, directive,
  macro), the macro invocation lines are skipped in favour of their bodies,
  and every instruction is decoded from the user's ROM and must match the
  source mnemonic, the size the assembler gave it and the rebuilt ROM's
  bytes: 110,998 instructions (242,371 bytes) and 2,875,669 bytes of data
  placed with no mismatch. The uncovered 27,688 bytes are the stale/fill
  regions of the original ROM and the LZ-compressed cutscene program
  (`$C2686C`, 8,692 bytes, the `cutscene_lz` segment, which the second link
  adds without debug info). The only data that differs from the rebuilt ROM
  is padding (`.res curr_pad_size`, 11,513 bytes).
  `listing.taken` (code labels the source uses as data, from the linker's
  reference lists: 1,981) replaces `analyze.address_taken`'s name scan.
- **First translation**: `make ff6x-source` runs in ~11 s: 2,954 procedures,
  87,648 of the 110,998 listed instructions reached from the vector and the
  address-taken labels alone, 245 findings in `build/ff6x/report.txt`
  (stack discipline of the battle code at bank C2, jump tables, about a dozen
  places where the analysis's M/X widths differ from the assembler's:
  `$EE43CD` and others), 7 BRK/COP traps. `WRAM_OVERLAYS` and
  `PATCHED_CALLS` (CT's) are emptied.

- **What was still Chrono Trigger's in the translator** (step 2, analysis part):
  - the NMI/IRQ trampoline is `$1500/$1504` (`analyze.wram_trampoline`) and
    `interrupt_targets.txt` holds the 19 handlers the engines install there
    (field, battle, menu, world map, train, airship, chocobo, ending, The End,
    and the cutscene program's two in WRAM), found from the installers in the
    listing; nothing is reported as unsupported any more;
  - the cutscene program is a WRAM overlay: `tools/smkx/ff6_lz.py` (the
    game's LZSS) decompresses `$C2686C` to `$7E5000`, and the result is
    byte-identical to the program the disassembly assembles (11,301 bytes,
    checked against `build/jp/temp_lz/cutscene.bin`); `WRAM_OVERLAYS` names it,
    `disasm.py` reads it;
  - `YIELD_ROUTINES` (CT's menu coroutines) is empty: FF6's menu tasks run to
    completion in `ExecTasks`; `PATCHED_CALLS` is empty; FF6 has no `SED`;
  - dispatch: `pointer_idiom_sites` takes the whole `.addr` run of a table the
    listing knows (the event commands are 203 words; the old cap of 64 targets
    left the rest of them untranslated), `JSR (table,X)` sites take a per-site
    list when `dispatch_targets.txt` has one (UpdateSpriteAnim: its table starts
    with `.addr 0`, which as a target is the start of bank $EE), table words
    that name no listed code are left out and listed in `report.txt`; the
    menu's `JMP (zTaskCodePtr)` and CheckBattleEnd's `JMP (BattleEndTbl-2,X)`
    have their targets in `dispatch_targets.txt` (`scripts/ff6x_menu_tasks.py`
    finds the tasks); `tables.txt` audited, nothing needed; `probes.txt` filled;
  - result: `make ff6x-source` reaches 109,215 of 110,998 listed instructions
    (98.4%) with no traps and no unsupported nodes, and `make ff6x` assembles
    and links `FF6X.TOS` (not run yet). The unreached 1,783 are mostly unused
    code (`DebugUnusedEvent`, `UnusedDMA`, ...) and menu/ending tasks whose
    pointers are not passed to `CreateTask` directly (`MosaicTask`, ...): the
    runtime's dispatch report (`scripts/ff6x_seed_loop.py`) will name them.
  Known and left as they are, for the first runs: the 233 findings are mostly
  the battle engine's non-local exits (`PLA PLA : BRA BattleEnd_06`, `PLA :
  JMP RestartBattle`: the analysis follows them as stack escapes), the field
  object script's `BMI @7924` at `$C078E1` that pulls Y it never pushed
  (`ObjCmd_d0`: an original bug; whether the game ever takes that path is for
  the first runs), and
  a handful of places where the assembler assumed M=16 after a call that
  returns in M=8 (`SetParentMap` at `$C01973`, `$C01982`, `$C01ABA`, `$C01AC9`:
  on the path where the parent-map flag is set the original runs those bytes in
  8-bit mode too).

- **First runs** (step 3, 2026-09-30): `make ff6x FF6X_APU_ECHO=1
  FF6X_INPUT=...` boots, initialises the field, loads the cutscene program
  into WRAM and runs the title (clouds and lightning, the FINAL FANTASY VI
  logo with its gradient, the Terra/Esper silhouette, "(C) 1994 SQUARE"),
  then the opening: the Japanese intro text over the Vector city, Narshe and
  the cliffs, the Mode 7 Magitek march with the staff credits, and the
  Narshe gate, where Biggs's first dialogue box appears ("the frozen
  Esper is deeper in"). 30,000+ frames without a trap or a dispatch miss;
  every snapshot's Falcon planes equal the host render (`make
  check-snaps`: 0 words differ; Mode 7 rows and palettes equal too). The
  pad is `FF6X_INPUT` frame:word pairs (`$4218` low byte: A = `$0080`, X =
  `$0040`; high byte: B = `$8000`, Start = `$1000`, Up `$0800`, Down
  `$0400`, Left `$0200`, Right `$0100`). What works: **A** on the title
  (frame ~4000; Start does nothing there, the loop tests `$06 & $80`) starts
  the game; A during the march skips the credits; at the gate a direction
  press starts the event, then A advances the dialogue. Not reached yet: the
  first battle (after the dialogue the party walks into the gate scene and
  the run sits there with no dialogue box up: probably another press
  or a longer wait; look at it with `scripts/hatari_calls.py`). What it took:
  - the translator's `TDC`/`PHD` overwrote the carry when D is unknown
    (`sub.l` sets the 68k X flag): a 24-bit pointer built by `ADC` across
    `shorta0` lost its carry, the map loader decompressed from the wrong
    bank and overwrote low WRAM. Found with the Hatari debugger
    (`scripts/hatari_break.py`, `scripts/hatari_calls.py`); the runtime now
    prints the host address of WRAM at start;
  - the SPC700: the game uploads its driver and then the title song
    through it, and waits on the driver's timers; time per poll now grows
    while a poll loop waits (`src/smkx_apu.c`), and `FF6X_APU_ECHO=1` leaves
    the driver out (ports echo) for first runs: the interpreted driver needs
    minutes for the title theme. Sound waits that need the driver's own
    answers do not work in that mode. The real sound is step 7;
  - dispatch: the cutscene program's task scheduler
    (`scripts/ff6x_overlay_tasks.py`), idle marks at the field, menu,
    battle-graphics and cutscene frame waits (`marks.txt`); the game's NMI
    then comes once per pass instead of once per ten VBLs (now ~55% of the
    VBLs);
  - speed: about 60 s of host time per 3,000 frames (Hatari, fast forward);
    the game runs at roughly a tenth of the SNES's speed (the title takes
    ~2,500 frames to finish, the credits ~5,500 more) because the VBL
    handler and the DSP renderer's work take most of the frame (step 5).

Next, in order:

1. **Image size**: 3 MiB image, but the runtime masks with `ROM_SIZE-1`
   (4 MiB) and `analyze.ROM_MASK` is `$3FFFFF`. Decide what `$F0-$FF`
   returns (open bus or a mirror; check that the game never reads it),
   and pad or mirror the image in `ff6.sfc` accordingly, so a stray read
   cannot run past the incbin.
2. **What is still Chrono Trigger's** (runtime and build; the translator part
   is done, see above):
   - the frame lists for `test-ppu`, `check-tc` and friends in the Makefile
     (`PPUTEST_FRAMES`, `TCTEST_FRAMES`) name CT's snapshots (`field1`,
     `palace1`, ...): regenerate from FF6's own runs;
   - `tools/smkx/marks.txt` has idle marks for the field, menu,
     battle-graphics and cutscene frame waits; the world map's and the
     battle's are missing, so are the main-loop marks, and `rt_mark_vwait`
     (CT's `BattleSys_WaitScanline`) has no FF6 counterpart yet; `natives.txt`
     is empty;
   - `falcon/ct_lz.s` and `tools/smkx/ct_lz.py` (CT's Gfx_DecompressEntry as
     a native routine; `FF6X_LZ=0`). FF6's `Decompress` (`$C0xxxx` in
     `src/field/reset.asm`, copies in the world and menu banks) is the
     candidate for the same treatment: it runs for every map and picture;
   - the comments in `falcon/smkx_rt.s` that say "CT" are CT's observations,
     not FF6's (the runtime was not changed apart from the trampoline
     addresses' comments).
3. **First runs**, what is left: the first battle (see above), the menu
   (Start in the field), the world map, scripted input recipes for them;
   `probes.txt` names the entry points (`BattleMain`, `WorldMain`,
   `CheckMenu`). Then the game's frame counts for the Makefile's frame lists.
4. **Input timing and speed**, taken over from F030CT: the NMI held back
   until the main loop has finished its pass, the DMA fast paths, page
   tables and the dispatch cache are in this copy already. FF6's field and
   battle engines set the budget.
5. **Picture**: the tile renderer on the DSP and the Mode 7 kernel are in;
   what FF6 needs beyond them is to be found with `ppu_census` /
   `win_census` on real frames: HDMA-heavy screens (window shapes, gradients,
   battle backgrounds), colour math and windows (the true-colour renderer
   has exact colour math, `FF6X_TC=1`), the Mode 7 world map and airship,
   interlace or hi-res text (check `SETINI`), and the many sub-palettes
   (`src/palette.c`).
6. **Sound**: the SPC700 interpreter (`src/spc700.c`, from F030CT) running
   FF6's own driver; `make test-spc` uses CT's driver on a CT image and
   needs a run against FF6's; then a real S-DSP on the Falcon DSP as in
   F030CT's roadmap, item 6.

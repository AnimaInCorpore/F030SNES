# F030SMAS roadmap

Set up on 2026-09-24 from F030SMW's code (commit `17baedb`), which comes
from F030SMK. The ROM is not here yet (`assets/README.md`).

Next, in order:

1. **Listing placement** (`listing.parse_smas`, called from
   `tools/smkx/__main__.py`). `SMAS/RomMap/ROM_Map_SMAS_U.asm` places the
   code bank by bank in two styles, both seen in sibling ports:
   - SMAS's own routines as `%ROUTINE_SMAS_*($addr)` /
     `%DATATABLE_SMAS_*($addr)` macros inserted at an address under a
     namespace (`%InsertMacroAtXPosition`): F030SMW2's `parse_yi`
     (`tools/smkx/listing.py` there) handles this style for Yoshi's Island;
   - the four games as per-bank macros (`%SMB1Bank03Macros(...)`,
     `%SMB2UBank19_00Macros(...)`), some inside `if` blocks on the ROM
     version: F030DKC's `parse` (bank macros, `CONDITIONS`, unsized
     operands, `%InsertGarbageData`) handles this style.
   The U.S. build is `!ROM_SMAS_U`; `if` conditions name the versions
   (`!Define_Global_ROMToAssemble&(!ROM_SMAS_U|...)`). Alternative: extract
   the assets with the framework's `ExtractAssets` step, assemble with
   Asar to get a symbol file, and place with F030SMW's Asar walker
   (`asar.py`, `parse_smwdisx`), which checks every label against the
   `.sym`. Either way every instruction is decoded from the ROM at its
   offset and must match the source mnemonic.
2. **Shared fixes this copy does not have yet** (F030SMW's state of
   `17baedb`), see `analysis/porting-notes.md`:
   - long,X with a bank base (`emit.py`, `loc`; F030SMK, 2026-09-24);
   - `rt_entry_regs` before the NMI/IRQ (F030CT, F030SMK);
   - liveness join node (F030DKC) and `all_callers` (check `liveness.py`);
   - page tables and dispatch cache (F030SMK, runtime speed);
   - the flag merge rule of F030CT (SMW's copy has its own variant,
     `keep = m68(need) | 0x10`; compare).
2b. **Renderer and compositor** (2026-10-01): taken over from F030SMW and checked on random frames
   and on real frames of the reference run (`tools/ref65816.c`, `make ref-snaps`), not yet run in the
   game: see `analysis/list-compositor-smas.md`, with what to check first once the translation runs.
3. **Game-specific runtime lines** still SMW's (found with the reference run: **SRAM must mirror every
   8 KiB in banks `$70-$7D`**, else the copy detection at `$00:803B` shows the "serious crime" screen
   and loops; `$4016` reads 1 is done in `rt_io_read`; the page tables of step 2 must mirror SRAM too): the APU return-to-loader
   request in `rt_apu_write` state 2 (SMW: `$FF` on port 1), the D
   register assumption (SMW never sets D; check SMAS and the four games
   for `TCD`/`PLD`, F030DKC tracks D), `SRAM_BYTES`, the joypad reads
   (`$4016`, F030DKC). SMAS switches between five programs (the menu and
   four games), each with its own NMI/IRQ handlers behind SMAS's vectors:
   check how the vectors dispatch (`ROUTINE_SMAS_VBlankRoutine`,
   `ROUTINE_SMAS_IRQRoutine`, `GameInitAndMainPointers`).
4. **First runs** to the title screen, then scripted input into each game;
   `check-snaps` against the host render.
5. **Picture and speed**: the 256-colour composer (F030SMW), measured per
   game; SMB3 and SMB2 use many sub-palettes.
6. **Sound**: F030CT's SPC700 interpreter runs the game's own driver.

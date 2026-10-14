# Translating Yoshi's Island (tools/smkx on the Yoshifanatic1 disassembly)

Notes specific to *Super Mario World 2: Yoshi's Island*. The translator's
design is in [translator.md](translator.md) (written for SMK); F030SMW's
LoROM work was taken over as the base (analyze/emit/liveness, runtime,
256-colour display path, from its commit `bd4389d`). This file records
what YI needed on top. Emulator for every run below: F030Arcade's patched
Hatari v2.6.1-devel, submodule `639fadf6`, binary SHA-256 `b3279c6d…`;
Falcon, TOS 4.04, 14 MB, `--cpu-exact`, `--fast-forward`. Frame counts are
emulator VBLs and say nothing about Falcon speed.

## ROM

U.S. v1.0: header version 0, checksum `$132C`/`$ECD3` (the byte sum
agrees), MD5 `cb472164c5a71ccd3739963390ec6a50`. That is the hash the
framework itself checks (`!Define_Global_BaseROMMD5Hash` in
`RomMap/ROM_Map_YI_U1.asm`); the readme's `bb9c2f66…` is not. The
translator accepts the ROM, or a ZIP holding it, on all three.

## Placement (`listing.parse_yi`)

All 165,229 instructions of `YI/Routine_Macros_YI.asm` are placed and
decoded from the ROM with matching mnemonic and length (2 s). Handled:
`if/else/endif` on the ROM version (U2 blocks skipped), debug switches
and `!CurrentBank`; `%ROUTINE_YI_*`/`%DATATABLE_YI_*` macros inserted at
an address under a namespace (bank `$00` in v1.0, `$0F` in v1.1);
`%InsertNextPreCompiledCodeBlock` (SuperFX code) and `%InsertGarbageData`
as unsized data; absent `incbin`s resynchronised at the next
address-bearing label; `a<TAB>: b` statement separators; immediates
without a size hint sized by value; the SuperFX map (`yi_offset`: LoROM
at `$8000` of banks `$00-$3F`, linear ROM in `$40-$5F`). Eleven label
names disagree with where their code sits (typos in the names, e.g.
`DATA_08DD98` at `$06DD98`); the ROM check confirms the placement.

## Memory map and WRAM code

- The reset code copies `YI_MainRAMCodeBlock` (`$00C000-$00FFFF`) to
  `$7EC000` and the interrupt trampolines (`$00813F`) to `$0100`. The NMI
  and IRQ handlers and the routines that start the SuperFX run there
  (the 65816 must not touch ROM while the GSU runs). Nothing writes into
  the copies, so they are decoded from the ROM at their WRAM addresses
  (`analyze.RAM_CODE`, `code_index`); the NMI vector `$0108` is a `JML`
  into the copy.
- The GSU's RAM (32 KiB, mirrored in banks `$70-$7D`) and its first 8 KiB
  at `$6000-$7FFF` of banks `$00-$3F` are host memory (`rt_gsuram`): YI
  keeps its sprite tables there (`!EXRAM_...|!EXRAMBankMirror`) and, with
  D = `$7960`, its sprite code's direct page. The emitter places static
  addresses there directly; with the data bank unknown, `DBR:$6000-$7FFF`
  goes through `rt_dbr6k` (the host address of that window, kept by
  `rt_setdbr`).
- The 4 MiB big-endian ROM word view is gone (it did not fit next to the
  2 MiB ROM and 5 MiB of code); 16-bit ROM loads rotate like WRAM loads.
  The TOS is linked without symbols (`vlink -s`; 8 MB of them).

## Program patterns

- **Direct page.** 34 `TCD`s: D = `$0000`, `$7960` (sprite code, many
  callees), `$2100`, `$420B`/`$4300` (register blocks) and a few computed.
  D is part of the analysis state and context (`dp`): constant after
  `LDA #imm : TCD`, through `PHD/PLD` pairs, and at reset through
  `KNOWN_D` (`LDA #$00 : PHA : PHA : PLD`). Known pages are folded into
  the address; an unknown D uses `a2`, the host address of D
  (`rt_setdp`).
- **Decimal mode** only inside `SED ... CLD` windows without calls (five
  BCD score updates); interrupts clear D. Contexts are formed with D
  clear, which removed most of the analysis' duplication.
- **Contexts.** Entries reached with more than one data bank or D are
  analysed once with it unknown, in passes until nothing changes:
  3,919 contexts over 3,794 entries, 391k nodes (from 16,288 and 1.15M).
- **Inline arguments**: `JSL CODE_00BE39 : dl ..., ... : dw ...` (8 bytes,
  55 sites) and `CODE_00BE71` (6 bytes); the callees advance their return
  address with `LDA $02,S`. The call continues behind the data
  (`INLINE_ARGS`); stack-relative addressing is supported.
- **Push-and-return dispatch** (`jumps.txt` "stack"): the game-mode
  dispatcher `CODE_008150` (`PHB`, the entry's bank for a later `PLB`,
  the entry minus one, `RTL`), ambient-sprite and sprite-state tables
  (`LDA tbl,y : PHA : RTS`).
- **Pointer jumps** (`jumps.txt` "ptr" or inferred from the `DATA_` label
  of the load): the sprite routine tables (`JMP [$7960]` after `PHY :
  PLB`, `dl` entries, the entry's bank as data bank), `JMP ($7960)` in
  many sprites, `PER/PEA ret-1 : JMP ($7960)` as calls, split lo/mid/hi
  tables, and `CODE_008365`, which is SMW's `ExecutePtr` byte for byte.
- **Tables addressed below their start** (`JSR (DATA_x-$01,x)`, up to
  `-$45E`): byte-offset jump tables, located via the operand's label.
- **Parked return addresses**: `PLA : STA $00 : JSL X : LDA $00 : PHA :
  RTS` (sprite wrappers, so that `X`'s `PLA : PLY : RTL` escape leaves the
  sprite routine too): see `unpark` in the analysis and the park slots in
  the emitter.

## SuperFX (GSU-2)

- **Protocol.** The 65816 writes the screen base/mode, PBR and R15 from
  WRAM code (`YI_BeginSuperFXProcessingRt` and variants, ~800 call sites),
  then polls SFR's GO flag; two variants serve requests in between
  (the GSU stops, the 65816 fetches `$7F:xxxx` for it and restarts it).
  The runtime runs the GSU synchronously to STOP when R15's high byte is
  written, which serves all of these.
- **Core** (`src/gsu.c`): instruction set, the one-byte pipeline (delay
  slots on every R15 write, operand bytes of a delay slot from the target
  stream), ROM/RAM buffers, pixel caches with PLOT/RPIX, as bsnes models
  them; no timing. `make test-gsu` runs a routine on the host.
- **Speed.** Interpreted, the loading phases run 0.5-1M GSU instructions
  per call at ~40k instructions per emulated second on the 16 MHz 68030.
- **Translation** (`tools/smkx/gsu2c.py`, `make ... YIX_GSU=xlat|check`):
  all GSU code is in banks `$08-$0B`; from every `CODE_` label of
  `SuperFXCode_YI.asm` the ROM is decoded along control flow with the
  prefix state (ALT1/2, B, Sreg, Dreg) per node, since YI jumps into
  prefixed instructions with the prefix in the delay slot. 48,333 nodes,
  C with the registers and flags in locals, cut into 180 functions of
  ~250 nodes (gcc chokes on one function per bank); 2.2 MB of 68030 code.
  Jump tables built from the pipeline (`WITH R15 : ADD R5` with an `IWT
  R15` delay slot taking its operand from the table) are left to the
  interpreter: the driver (`src/yix_gsu.c`) enters translated code
  whenever the next opcode is a translated node and single-steps the
  interpreter otherwise. `YIX_GSU=check` runs every GSU call both ways
  from the same state and reports the first run that differs.

## First runs (2026-09-24)

- Boot, SPC700 uploads (echo model), GSU initialisation and
  decompression, NMIs; game modes 0 -> 7 with the main loop counting.
- Bugs found on the way (all fixed): `RTI` did not restore the I flag
  (IRQs masked after the first NMI; YI does its frame work in an IRQ in
  NMI mode 8); stack-jump and table states dropped D; the parked return
  address; a push-and-return site whose other path skips the push.
  `SMKX_CHECK_STACK=1` located each within a few calls.

## Update 2026-10-01 (the DSP stack and the compositor; analysis/list-compositor-smw2.md)

- **The stack jump to `$000FCACB` at game mode 7 is the translated SuperFX code.** `YIX_GSU=check` reports the first difference at run 3;
  with `YIX_GSU=interp` the same game passes game mode 7 without a trap (the sprite status the `$0FBF7F` stack site reads is wrong with
  the translated code). Not a `jumps.txt` gap. The interpreter build runs 5-20 times slower: the loading mode 5 takes 9,000 frames.
- With it: logo (frames ~1,500), Nintendo patchwork screen (~10,600), title with the Mode 7 island (~12,300), file menu (~29,000, Start held
  100 frames every 400), then game mode `$38` at ~33,000, where the game waits for ever on the APU port `$2140` (the sound upload of the next
  scene; the echo model does not answer). No level yet.
- `rt_io_write`'s VMADDH was stored unmasked (fixed `09258e3`): a long run ended with `rt_frames` corrupted.
- Frames: the intro has the frame IRQ at line 13, 15 and 199 (INIDISP `$8F` from line 199 to the next line 13, the whole CGRAM rewritten
  after it, OAM in four segments); the title has BGMODE `$10` (mode 0, BG1 16x16) with seven HDMA channels (BGMODE, TM, BG1/BG2 scroll, CGRAM,
  Mode 7 A/B per line).

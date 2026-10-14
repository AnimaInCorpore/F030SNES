# ROM and source verification

## ROM (2026-10-01)

`assets/Super Castlevania IV (USA).zip` holds `Super Castlevania IV (USA).sfc`:
1,048,576 bytes, MD5 `094f035993e9724647b61ddcba1e9a7a`, no copier header.
Header at `$00:FFC0` (file offset `$7FC0`): title `SUPER CASTLEVANIA 4`,
map mode `$20` (LoROM, SlowROM), cartridge type `$00` (ROM only, no
coprocessor), ROM size `$0A` (1 MiB), SRAM `$00` (none), country `$01` (USA),
version `$00`; checksum `$0325`, complement `$FCDA` (they sum to `$FFFF`).

The ROM has no battery RAM. The game keeps no save data.

## Disassembly search (2026-10-01)

No complete, version-matched disassembly or assembler listing of the US ROM
was found (searched GitHub and romhacking.net). What exists:

- `RasmusKarlsson/SuperCastlevaniaIvSNESRecomp` and `snesrecomp-sciv`: a
  recompilation of this ROM built on a generic framework. Not a labelled
  source listing; useful as a cross-check of entry points and hardware use.
- `bogaa/OtherCastle2`, `bogaa/Super-Castlevania-IV-Practice`,
  `bogaa/Super-Castlevania-IV-addons`: ROM-hack patches with some RAM/ROM
  addresses; partial.
- `VitorVilela7/fastrom`: documents the SlowROM → FastROM patch (the game
  has slowdown; relevant to timing).

Consequence: the port cannot place the program from a listing the way the
other ports do. The translator has to find code by tracing from the reset,
NMI and IRQ vectors (`$7FE0-$7FFF`: reset `$00:8000`, NMI `$00:81B8`; COP, BRK, ABORT and IRQ
all point at `$00:82BE`) and following jumps, with the code/data boundary
settled by the 65816 M/X flag tracking in `tools/smkx/analyze.py`. Record
every address list that results (`tools/smkx/*.txt`) with how it was found.

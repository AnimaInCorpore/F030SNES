# Trace-based disassembly (2026-10-01)

No source listing exists for Super Castlevania IV (see
`source-verification.md`), so `tools/sciv_trace.py` finds the program by
recursive descent from the vectors. `make sciv-trace` writes `build/sciv_trace/`:

- `disasm.txt`: whole-ROM listing, labels (`sub_`, `loc_`), `.db` runs for
  everything not reached as code.
- `coverage.txt`, `problems.txt`, `tables.txt` (every guessed jump table),
  `reasons.txt` (why each address was first queued: the path back to a vector).

## What it does

- LoROM mapping; `$80+` banks fold onto `$00+`.
- Tracks M/X/E through REP/SEP/PHP/PLP/CLC;XCE. Interrupt vectors start with
  widths unknown (they resolve at the first REP/SEP).
- Calls are traced with the caller's widths and the code after the call
  continues with the widths the callee *returns* in (e.g. a routine ending
  `sep #$20 / rts`). Visited states include the routine context.
- Tracks DBR for the idiom `lda #n / pha / plb` (and `phk / plb`, `mvn`).
- Indirect transfers: `jsr/jmp (abs,x)` tables, and the game's RAM-pointer
  idiom `lda table,x / sta $00 / jmp ($0000)` (2-byte entries) and
  `jml [$0000]` (3-byte entries, bank < `$20`). Table lengths are guessed: an
  entry is taken while it points at ROM and a 40-instruction straight-line
  decode of it hits no BRK/COP/STP/WDM/RTI and no jump out of ROM.

## Result of the first run

91,251 code bytes (8.7% of the ROM), 38,656 instructions, 133 tables
(981 entries). Code lives in banks `$00` (92%), `$02` (55%), `$03` (73%),
`$05`, `$06`, with small amounts in `$04` and `$0C`; banks `$01`, `$07`-`$1F`
have none reached, which fits graphics, level and music data (bank `$01` is
the data bank the game selects with `lda #$01 / pha / plb`).

Open items (`problems.txt`):

- `$0286D4-$0286F7`: the same code is reached with M=0 (via the `$02D6EB`
  dispatcher) and M=1 (via `$02864A`); one of the two is wrong and the
  listing shows whichever was decoded last. Needs a look at the callers.
- `$06968B`, `$0696A0`: `jsr ($00EC,x)` in bank `$06` has no ROM table.
  Probably data decoded as code from an over-long table; check `tables.txt`.
- `$0CD816`: `jml [$0903]`, same suspicion.
- Table lengths are guesses: confirm each in `tables.txt` against the code
  that indexes it before trusting a table's tail, and put confirmed lists in
  `tools/smkx/dispatch_targets.txt`.
- Not handled yet: `pea`/`rts` dispatch, code in WRAM (uploaded routines),
  inline data after `jsr`, and `jmp`/`jml` through pointers set up in ways
  other than the load/store idiom above.

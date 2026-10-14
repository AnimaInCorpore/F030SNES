# Faster 68030 code from the translator (2026-09-29)

Review of `tools/smkx/emit.py` and the analysis, with changes and
measurements. **Nothing here ran on the patched Hatari or on a Falcon**: this
session had the ROM and the disassembly but not `../../../F030Arcade` (private), so
the checks are a 68030 model (Musashi, through `machine68k`) with no cache and
no bus wait states. Its cycle counts are relative figures. The ST-RAM bus of a
real Falcon makes code *size* count almost as much as instruction count, and
the new code is bigger, so the real gain may differ in either direction.
Measure on Hatari (`make check-snaps`, the fps scripts) before trusting a
number below.

## Result

Same game, same scene (the menu shell, reached by every build after ~3,000
NMIs, V-blank delivered as soon as the game idles, rendering off, sound
deferred, release build), cycles per NMI on the model, profile sampled every
100 cycles over 2,000 NMIs:

| | translated code | runtime helpers called from it | per-frame runtime work | total |
|---|---:|---:|---:|---:|
| before | 6,654 | 10,831 | 10,470 | 27,956 |
| after | 6,618 | 3,214 | 10,365 | 20,198 |

Game code (the first two columns) 17,485 → 9,832 cycles, **about x1.8**; the
whole frame x1.38 because the per-frame runtime work (`rt_frame_start`,
`rt_oam_segment`: ~3% of a real 50 Hz frame of 320,000 cycles) is untouched.
Boot to NMI 250 is ~9% cheaper (72.5 → 66.1 M cycles; about half of boot is the SPC700 interpreter).
This scene has few register writes and no big loops, so busy scenes (NMI
uploads, event scripts, battle) should gain more or less depending on their
mix; that is unmeasured. Each piece in isolation (model cycles, one
instruction sequence): `CLC : ADC #1` 55 → 27, `SEC : SBC #5` 79 → 39,
`ADC dp` (N live) 62 → 50; `rt_dispatch` over a hit-heavy mix 743 → 416 per call.

Size: generated code text 1.69 → 2.10 MB (the inline lookups); the linked
program loads at 9.15 MB instead of 8.73 MB.

## What changed

Files: `tools/smkx/emit.py`, `falcon/smkx_rt.s`, `falcon/smkx_rt.i`,
`tools/smkx/analyze.py` and `listing.py` (opt-in only), tests in `tools/`.

1. **Operands the analysis cannot place** (pointers `(dp),Y` `[dp],Y` `(dp)`, an
   index register holding an address, an unknown data bank: ~12,000 static
   sites, all `jsr rt_rb/wb/rw/ww` costing ~30 instructions each) look the
   page up inline in the runtime's `rt_pages_r/w` (`paged_read/paged_write`).
   I/O pages, and a word on the last byte of a page, take the old helper out
   of line. X (the carry) is untouched; d7/d6, a1 and the flags but X are
   scratch. Exact for addresses that wrapped past bank `$FF`.
2. **A register reached through an unknown data bank** (`STA $4300` with DBR
   unknown: 4,000 static sites) tests bit 6 of the bank (`btst #22,d3`): a
   system bank is the register (direct path below), any other bank is
   WRAM/ROM through the generic path (`guarded`).
3. **Direct register paths** (`plain_write`, `SHADOW_R`): writes to registers
   whose runtime handler is only the shadow store (`$4201/2/4/5/7-A/C/D`, all
   `$43xx`, `$2100/1/5-C/1A/23-31/33`) are one `move`, 16-bit pairs included;
   `$21xx` ones test `rt_logging` first (the IRQ log). NMITIMEN stores its two
   bytes. `$4218-F` and `$43xx` reads come from the shadow. `$2180` byte
   accesses call `rt_w2180/rt_r2180`. `tools/emit_tests.py` (`plain_sets`) checks
   the sets against the runtime's `.w21`/`.w42` tables.
4. **`PLB` inline** (1,378 sites): `rt_setdbr` without the call and its stack
   traffic; proven identical to the real routine's registers, flags, `rt_dbr`.
5. **ADC/SBC**: `CLC : ADC` and `SEC : SBC` (single-predecessor pairs, no hook
   between) become `add`/`sub`/`addq`/`subq`; otherwise the Z-preset
   (`ori #4,ccr`) only when Z is live and the closing carry flip only when C
   is live. **Read-modify-write on memory**: no `tst` and no closing byte swap
   when N/Z are dead; the last `ror` already gives N/Z for 16-bit host stores.
6. **`rt_dispatch`** (6% of the frame): a 1,024-slot direct-mapped cache in
   front of the binary search, slot written with one `movem` (the VBL runs
   translated code that dispatches too).

Kill switch for bisecting: `SMKX_EMIT_OFF=guard,paged,plain,plb,w2180,adc`.

## How it was checked

* `make test-emit` (`tools/emit_tests.py`, 60,000+ cases, ~4 minutes): each
  test translates real 65816 instructions with the real `Emitter` (the
  flag liveness chosen per case, every subset of N/Z/C/V), assembles them with
  vasm, runs them on the 68030 model and compares registers, memory, X and
  the live flags with a 65816 reference. Covers ADC/SBC (8/16-bit, immediate,
  memory, folded pairs), RMW, the page tables (mapped, ROM, unmapped, page
  straddles, bank wrap), the register guard and plain stores with and without
  IRQ logging, PLB against the real `rt_setdbr`, `rt_dispatch` against the
  version in git HEAD. Mutations (dropping the straddle check, `lsr` for
  `ror`) are caught. The baseline emitter passes the same ADC/RMW tests.
* Everything translated (`python3 -m tools.smkx`, 15 s), every generated file
  assembles, and the program links (vlink) with the runtime and the C objects
  (cross gcc `m68k-linux-gnu-gcc`, DSP images as placeholders).
* `tools/ctx_run.py`: the whole linked runtime plus translated program from
  boot on the model, V-blank injected by the harness (also every 320,000
  cycles when the game never idles; stepping past IPL 7 windows, which
  otherwise alias with the idle loop for some chunk sizes), hardware
  addresses reading 0. 20,000 NMIs without a stall in every timing tried.
  The visible state (VRAM, CGRAM, OAM) is identical between builds in the
  scenes reached, **but those are static screens**; a strict A/B of WRAM is
  not possible because the game's state depends on V-blank timing (the same
  build gives different WRAM checksums for different injection points). Run
  `make check-snaps` and an attract cycle on Hatari.

## Found but not done, in order of expected value

* **Static data bank** (opt-in, not the default). CT sets the bank with
  `LDA #$00 : PHA : PLB`; the analysis pushed unknown bytes for `PHA`, so
  after the boot's own `PLB` (`$FDC00B`) 136,597 of 202,303 nodes ran with the
  bank "as on entry" and 3,407 of 3,664 procedures with an unknown-bank
  context. `SMKX_PHA_CONST=1` makes `PHA` push a constant loaded by the
  instruction before it (no label between): 5,141 procedures, 311k nodes, and
  every routine entered with a known bank gets direct WRAM/ROM/register
  addressing. Cost: text 2.10 → 2.87 MB, data 0.57 → 1.09 MB (continuation
  tables, 96 exit ids x 4 bytes per multi-return call site). In the menu
  scene it adds only x1.49 → x1.53 on top of the changes above, so it is off;
  worth trying on NMI-heavy scenes. `SMKX_DISPATCH_BANK=1` additionally hands
  a known bank from a pointer-dispatch site to its handlers (the dispatch key's
  bits 4-7 carry the bank class): text 3.39 MB, 12+ MB loaded, untested at
  run time. Remaining leaks: 1,254 call returns whose callee ends with an
  unknown bank, `TDC : PHA : PLB` (16 sites, bank = low byte of D), `LDA dp :
  PHA : PLB` (bank from data).
* **Per-frame runtime work**: `rt_oam_segment` copies 544 bytes at every
  frame start and IRQ, `rt_frame_start` ~1 KB; ~20,000 cycles per NMI together
  on the model. Copy OAM only when it changed (F030SMK does), copy shadows
  only when written.
* **Byte swaps**: 43,000 `ror.w #8` (9% of all instructions). `LDA dp16 :
  STA dp16` swaps four times; a lazy-swap state per register within a block
  removes most.
* **CMP with a live carry** that is not fused costs `move ccr / table read /
  move ccr`; fuse across `BEQ/BNE` (`CMP : BEQ : BCS`).
* **65816 frame writes at calls**: `JSL` writes 3 bytes nobody reads; skip
  them for procedures that never read the frame (careful: CT's coroutine
  stacks, `rt_setsp`).
* Remaining helper calls after this: generic register accesses (APU ports,
  `$2118/$2119`, CGRAM `$2122`, `$4200` reads), `rt_dma`, `MVN`.

## Reproducing the model runs

Needs `vasm` (vasmm68k_mot) and `vlink` (`$VASM`, `$VLINK`),
`pip install machine68k`, and for whole-program runs a cross gcc
(`make CROSS_CC=m68k-linux-gnu-gcc`, then remove `.note.GNU-stack` from the C
objects with `m68k-linux-gnu-objcopy`) and placeholder `m7c2p.bin ppu.bin
ppu7.bin ppuw.bin` (the DSP assembler is not available without
`../../../F030Arcade`). Build with `CTX_RENDER=0 CTX_DIAG=0`, then
`python3 -m tools.ctx_run build/ctx --nmis 3000 --every 250`.

## For the other ports (their `analysis/porting-notes.md`)

Not written into them: those checkouts were not present. Take over
`emit.py` (`paged_*`, `guarded`, `plain_write`, ADC/SBC/`folded_carry`, RMW,
PLB) once `PLAIN21/42` and the runtime's register tables agree, the
`rt_dispatch` cache (F030SMK's has one; keep the history in the slow path),
and `tools/emit_bench.py` + `emit_tests.py` as the way to check emitter changes
without an emulator. Finding to pass on: an analysis that leaves the data bank
unknown sends every absolute operand in `$2000-$7FFF` (all the hardware
registers) through the generic path; count `jsr rt_wb` sites by operand before
optimising anything else.

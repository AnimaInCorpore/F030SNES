# 68030 translator instruction selection — 2026-10-01

`tools/smkx/emit.py` now uses:

- `BFEXTU Dn{8:16},Dn` for the page index `(address >> 8) & $ffff`,
  replacing `ROR.L #8` / `ANDI.L #$ffff`. The following low-byte mask stays
  `ANDI.L`; a lone mask is not a good bit-field replacement.
- `BFEXTU d7{26:2},d6` (or destination d7) for P bits 5..4 in PLP width
  dispatch. PLP still installs the pulled flags and truncates X/Y when needed;
  the external-width dispatch still restores the original CCR.
- `BTST #bit,d0` for single-bit immediate BIT. SNES immediate BIT changes
  only Z. BTST preserves N and X, so a live N no longer needs a CCR merge.

Offsets count from the most significant bit. BFEXTU preserves X but sets N
from the source field, not the zero-extended long result. Extraction is used
only where those intermediate flags are overwritten/restored. Byte/word loads,
endianness, I/O side effects and page-end slow paths are unchanged.
Instruction semantics were checked against the Motorola/NXP reference:
https://www.nxp.com/docs/en/reference-manual/M68000PRM.pdf

`SMKX_EMIT_OFF=bitfield` restores all three old sequences for bisection.
No runtime or renderer source changed in this work.

## Assembled SCIV size

Same ROM, tracing, analysis and liveness; 92,882 nodes, 36 generated code files,
zero unsupported nodes in both translations. Vasm 1.9d, `-Felf -m68030`.

| Measure | Before | After |
|---|---:|---:|
| Generated code-file text-section bytes | 1,074,350 | 1,055,448 |
| Instruction lines (directives excluded) | 330,189 | 325,094 |
| Page-index rotate/mask sequences | 3,947 | 0 |
| New single-bit BIT tests | 0 | 560 |

Text reduction: 18,902 bytes (1.76%). Text includes in-chunk dispatch tables;
branch relaxation and chunk placement also affect the assembled difference.
SCIV emits no dynamic PLP width selectors here; that change serves the shared
translator and is independently tested. ROM/assets and all generated output
remain under ignored `build/` directories.

## Correctness

The emitter suite passed 82,945 cases on machine68k 0.4.1's M68030 model
(82,665 existing/new cases plus 280 additional page-field cases). Final BIT
code was rechecked after rejecting the slower multi-bit candidate.

New tests cover both accumulator widths, every live-flag subset, masks including
zero/sparse/contiguous ones, all 256 P bytes with all 32 CCR inputs for both
width-dispatch forms, both scratch registers, both carry values, poisoned upper
address bytes, mapped/unmapped pages and bank $FF. Existing paged tests exercise
byte/word loads/stores, ROM writes and page-end helper fallbacks. The 22,464 BIT
and width cases also pass with `SMKX_EMIT_OFF=bitfield`.

`tools/emit_bench.py` caches assemblies by complete source to make these sweeps
practical, defines rt_lastp for the width tests and recognises the Windows vasm
`.exe` suffix. Run `python -m tools.emit_tests` with machine68k installed and
the assembler's DLL directories on PATH; VASM may override its location.

A separately linked executable with the final generated code completed 6,000
runtime frames with rendering requested only at the final snapshot
(`SMKX_RENDER_EVERY=60000`, no input). The final 28,672 framebuffer words all
match `ppu_snap`'s reference. This is an intro/attract smoke check, not a
gameplay-throughput comparison. Existing runtime/C objects and DSP assets were
used; the normal build and its flags were left alone.

## Patched Hatari microbenchmark

Required binary: `../../../F030Arcade/third_party/hatari/build/src/hatari.exe`;
source HEAD `3eea7b07`, Hatari v2.6.1-devel compiled Oct 1 2026 12:05:19,
SHA256 `600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf`.
Falcon, 16 MHz, cpu-exact/compatible enabled, 14 MiB ST-RAM, TOS 4.04,
headless fast-forward through `scripts/run_smkx.py`. No physical Falcon data.

Each source sequence repeats 2,000,000 times, with `SUBQ.L #1,d2` / `BPL`
loop control, timing from TOS `$4ba` (200 Hz). Both runs returned identical
tick counts. These include loop control, cache/pipeline placement and interrupts;
they are not isolated instruction latency or full-game speedup figures.

| Sequence | Before ticks | After ticks |
|---|---:|---:|
| MOVE.L a0,d7; ROR.L #8; ANDI.L #$ffff → MOVE.L + BFEXTU | 716 | 581 |
| MOVE.W d7,d6; LSR.W #4; ANDI.W #3 → BFEXTU | 447 | 202 |
| MOVE.W d0,d7; ANDI.W #$80 → BTST #7,d0 | 268 | 226 |
| MOVE.W d0,d7; ANDI.W #$3c → BFTST d0{26:4} (rejected) | 318 | 552 |

The contiguous-mask BFTST candidate saved two bytes but slowed its loop, so it
was removed. Fewer instructions alone do not establish faster execution.
Microbenchmark source/binary are in `build/emit-perf/perf.s` / `PERF.TOS`;
logs are `build/scivx/emitperf{,-repeat}-stderr.log`. Baseline/final generated
sources and objects are in `build/emit-before` and `build/emit-after`; smoke
log is `build/scivx/emitgame-stderr.log`, snapshot `build/emit-after/SNAP.BIN`.

# True-colour renderer: a route toward 2x

2026-09-28. Source and existing-listing review; no kernel changes and no
new emulator run. Reviewed CT commit `cd053a5`, `falcon/dsp/tc.asm` SHA-256
`8d466290bc4d5c0c4c653d987f571d159b542331370d37f6420548c3e7cd7d0b`.

The recommendation is small, specialised span kernels in internal program
RAM, with fewer intermediate pixel writes. A larger output ring is not
the first candidate for the reported compute-bound scenes. The existing
renderer already converts tiles during TPAGE and has direct one-screen
paths and colour lookup tables; those are not new optimisations to propose.

## Budget and scope

The user's reported Mode 7 profile is 353 ms, including 32 ms in `tx_isr`.
Its raw profile was not recovered in this review. The approximately 146 ms
RGB565 output floor and 300-550 ms field renders are also recorded in
`analysis/ct.md`. These are emulated figures, not physical Falcon results:
patched Hatari `v2.6.1-497-g639fadf6`, build 2026-09-22 21:37:14, binary
`../../../F030Arcade/third_party/hatari/build/src/hatari`, SHA-256
`b3279c6de6893a666e8e3c87b65eb4cf69f10f296a098b39272f7757731f1f7d`.

- 2x for that Mode 7 frame means 176.5 ms, only about 30.5 ms above
  the output floor. Wire time overlaps DSP execution; do not add it again.
- Even removing all 32 ms of transmit service gives only
  `353 / (353 - 32) = 1.10x` under the reported compute-bound assumption.
- With service unchanged, the approximate budget for other DSP work is
  `176.5 - 32 = 144.5 ms`, versus `353 - 32 = 321 ms`: about 2.22x faster.
  This is a planning budget, not a measured projection of any patch.
- Frames already near the output floor cannot double with unchanged
  transport and payload. Kernel time and live picture cadence also differ.

## First experiment: internal program RAM

The existing listings place TC7 `m7_inside` at P:$0532 and `colour` at
P:$0933; the tile build's `px_dmax` is at P:$0A6F. They are outside the
512-word internal P memory. Startup, initialisation, command handling and
part of conversion currently occupy that scarce internal space.

Keep the loader and SSI vectors/handler valid, move initialisation out
where possible, and place the frequently executed colour, sampling and
merge loop bodies below P:$0200. Reusing bootstrap space after loading is
another option, with a bounded overlay and explicit size checks. Measure
relocation alone before changing algorithms. It is not a 2x prediction.

External code fetches and external data accesses share the bus. Motorola's
[DSP56000 family manual, section 8.2](https://www.nxp.com/docs/en/reference-manual/DSP56000UM.pdf#page=171)
describes the extra instruction cycles for multiple external accesses.
The local patched Hatari also accounts for P/X/Y conflicts in
`src/falcon/dsp_cpu.c`, `dsp56k_execute_instruction`. Merely moving two
external arrays into different X/Y spaces does not give independent buses.

## Larger savings: specialise the work

1. Split a row at the two windows' boundaries, giving at most five spans
   of constant window membership. Dispatch by the actual clip/math state
   in each span. `colour` currently sends the entire row to `pixel` when
   `CASEW` is false. Extend the existing lookup paths to spans; avoid
   rebuilding the full colour tables at every boundary. This targets the
   general window path, not a Mode 7 frame already using `_sf` throughout.
2. For layers on both screens, decode once and update MAIN and SUB with
   their own masks and priority comparisons, avoiding LK and its two
   later merge passes. Preserve sprite ordering and the 32-sprite and
   34-sliver limits. The direct one-screen path already exists.
3. Use the converted SOLID flags in specialised direct tile loops to
   remove transparency tests; `px_dmax` currently tests every pixel.
   Eliminate priority comparisons only where the span's coverage/depth
   proves them unnecessary. For common Mode 7 layouts, prototype fusing
   the sampled background key with the already resolved sprite/sub-screen
   keys and final colour lookup, avoiding an intermediate MAIN pass.
   Account for register pressure and internal-P size before committing to it.

Count time by colour path, merge and sampler on fixed representative
snapshots. Compare each isolated change with resident pages, then repeat
with uploads and the live game. Check against `ppu_render_565`,
`make check-tc`, `make test-tc`, runtime snapshots and banded frames.
Keep output-starvation and `room` waiting separate from drawing cycles.

Prefetching another frame can only hide transfer against drawing when
there is separate safe storage for its commands/pages; it cannot overwrite
pages still used by the current frame. DSP-side conversion still competes
for the same execution cycles. Measure exposed upload/conversion time
before redesigning that pipeline, especially with the existing 74-slot
pressure. No 2x result is established by this review.

# Mode 7: what reaching 25 pictures/s requires

2026-09-26. Investigation of the current four-plane, 256x224 renderer.
25 pictures/s means a complete picture every 40 ms, including preparation,
transfers and presentation in steady state. This investigation does **not**
establish that an exact full-resolution renderer can meet that target.

The current DSP needs a substantial redesign. The newest TILE0 cache is
useful, but the frozen one-/two-player pictures still take 212/201 ms with
output. Even the Mode 7 sample loops alone exceed 40 ms in the instruction
budget below. Optimising only shifts, waits or c2p will not close this gap.

## Reproducible baseline

All timings here are **emulated**, using only
`../../../F030Arcade/third_party/hatari/build/src/hatari`,
`v2.6.1-497-g639fadf6`, compiled 2026-09-22 21:37:14. Binary SHA-256:
`b3279c6de6893a666e8e3c87b65eb4cf69f10f296a098b39272f7757731f1f7d`.
The profiler reports 32,084,988 DSP clocks/s; 40 ms is 1,283,400 clocks.
These are not physical Falcon measurements or sustained game frame rates.

The working tree was already modified. Sources were frozen in
`build/m7-audit-20260926/`, with `manifest.json`; no production renderer
source was edited by this investigation. Concurrent work added `T0V`,
`t0_set` and the shorter TILE0 loop after the first capture, so that source
was frozen and tested separately under `current-cache/`.

| Frozen source | `ppu.asm` SHA-256 |
| --- | --- |
| Initial capture, already using multiply-based shifts | `628660425bc3463b9cc40534982b1a8c5db9741272429bb67ce62c51a27a3149` |
| Concurrent TILE0 cache, captured separately | `5dc62831bc648cb443a233c6bf43a3d6b0d99a641831617cf1ac3359ee30f9ba` |

Snapshots in `build/racesnap/`:

- `R1P.BIN`: `765c6277e72dc45a21638db63aa9f5e323918ab0835f7481b07cc478743f485d`.
- `R2P.BIN`: `eac2330e6aa45d4b01e8c8675b687a901814f1dc629eac7c32c8f72962563208`.

PPUTEST was built with `PT_SNAP` and `PT_PROF`, using a local benchmark
copy with all four feature-removal variants enabled and an additional
`pt_prof_end` marker immediately after the first frame's PING. Profiles
start at `pt_prof_start` and stop at this marker, excluding VRAM upload,
CPU stream construction and the slow CPU reference comparison.

| Test (ms) | Initial 1P | Initial 2P | TILE0 cache 1P | TILE0 cache 2P |
| --- | ---: | ---: | ---: | ---: |
| Full picture, SSI output | 227.968 | 220.182 | 211.588 | 201.067 |
| Full picture, output disabled | 204.140 | 195.625 | 186.979 | 176.614 |
| Output disabled, no sprites | 144.166 | 147.656 | 127.213 | 128.880 |
| Output disabled, no BG1 | 121.276 | 121.406 | 121.328 | 121.432 |
| Output disabled, neither | 65.026 | 77.864 | 65.572 | 77.942 |

The full output frames all have **zero differing plane words** against
`ppu_render`/`ppu_planar`. The snapshot benchmark does not compare histogram
bins; its random-frame counterpart does. Random DSP tests, gameplay,
uploads in flight and a physical Falcon were not rechecked here.

These are single frozen-frame tests, not averages. The first no-output
frame includes table initialisation (127,298 profiled clocks, about 4 ms);
the subsequent SSI frame is warm. All commands in this benchmark use the
**host port**; “with SSI” describes output, not production DMA command
input. Do not infer SSI input bandwidth or live picture rate from it.
Feature-removal differences are not additive stage timings.

Logs: `build/smkx/m7audit-{r1p,r2p}-stderr.log` and
`build/smkx/m7audit-current-cache-{r1p,r2p}-stderr.log`.
Profiles: `build/m7-audit-20260926/{r1p,r2p}-profile.txt`.

## Where the clocks go

The initial no-output profiles contain 6,409,036 / 6,140,554 clocks after
excluding the host receive wait at P:$006A. The complete measured window
contains 6,549,940 / 6,276,928 clocks. This is a roughly fivefold compute
gap to 40 ms, before accounting for output service.

| Address regions, initial source | 1P equivalent ms | 2P equivalent ms |
| --- | ---: | ---: |
| Shared Mode 7 / BG-cell overlay | 76.86 | 75.04 |
| Winner selection and compose setup | 27.17 | 28.51 |
| c2p group loop | 23.77 | 23.77 |
| Sprite list, slivers, decode, clearing | 31.04 | 21.62 |
| VRAM read/unpack helpers, shared by BG and sprites | 7.63 | 6.80 |
| Mode 7 setup, validation and segment planning | 17.08 | 13.80 |
| BG setup and row cache, excluding overlay | 4.49 | 8.80 |

These are disjoint address-region sums, not inclusive call costs. The
remaining clocks include initialisation, frame dispatch and other glue.

**Overlay trap:** `rn_cells` and `m7ins` occupy the same internal P region
starting at $0097. A whole-frame address profile combines different
instructions there. Its disassembly can even show a current immediate
operand as an instruction that executed earlier. Keep this region grouped
unless profiling one overlay at a time. Do not label its entire cost as
Mode 7 sampling. Similarly, stop before the CPU reference renders: the DSP
then spends a long time waiting for commands.

The hot loops are already in internal P; only 2 of its 512 words are free.
Internal relocation of these same loops is not an optimisation. A dedicated
race program would free generic BG code and the borrowed interrupt vectors.

## Exact reuse, measured from the input

`tools/m7_sampling_audit.c` replays the existing frame loop, runs
`m7_plan`, and compares every segment's fetched pixel against
`ppu_m7_pixels`. It counts source-address reuse and sprite coverage; it
does not approximate sampling or predict cache execution cost.

| Frozen view | Mode 7 pixels | TILE0 pixels | Adjacent same texel address | Groups without opaque OBJ | Rows with C=0 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1P perspective | 21,504 | 10,647 | 9,983 | 1,203 / 1,344 | 0 / 84 |
| 1P map | 26,880 | 8,228 | 0 | 1,376 / 1,680 | 105 / 105 |
| 2P upper | 21,504 | 11,250 | 9,972 | 1,162 / 1,344 | 0 / 84 |
| 2P lower | 21,504 | 9,680 | 9,975 | 1,165 / 1,344 | 0 / 84 |

Thus 85.3% / 86.6% of Mode 7 groups need no sprite winner comparison.
Perspective views repeat exact texel addresses on about 46% of pixels;
the downsampled 1P map does not. Caching the last texel must therefore be
conditional, not an unconditional per-pixel tax. Inside-map cell reuse is
about 95% in these perspective views but only 38% in the 1P map.

Only 7.8% / 6.7% of Mode 7 pixels are covered by an opaque priority-1..3
sprite, so skipping sampling behind front sprites alone is a small gain.
OBJ0 still requires BG transparency. Tile 0 is not a solid colour in
either capture: its 64 texels contain CGRAM entries 7, 8 and 9.

Additional validation: all 876 available `build/smkx/**/F*.BIN` captures
were accepted; 17,025,792 Mode 7 pixels compared, zero differences. These
include duplicate frames and non-race scenes, and do not add course/item
coverage beyond the existing captures. Detailed CSVs are in the audit
build directory. Their overall TILE0 share is only 14.4%: the two frozen
pictures are not representative of every camera position.

## What to implement next

1. **A fused race Mode 7 path is the main experiment.** Sample, select the
   winning CGRAM index, map it and accumulate four output planes without
   the current `LB0 -> IB -> four colour-table reads` sequence. Dispatch
   16-pixel groups by an opaque sprite mask; most groups take a direct
   background path. Retain the general renderer for unsupported rows or
   states. The separate winner/c2p passes currently cost about 51-52 ms;
   fusion must perform their necessary operations more cheaply, not merely
   move them inside another loop. Preserve raw colour-zero transparency,
   OBJ0 ordering and the winning CGRAM index for the histogram.

2. **Specialise source traversal.** Keep the new TILE0 cache. For INSIDE
   runs, reuse the map tile base until the source cell changes, and reuse
   exact texels only where the increments make it profitable. Add a C=0
   sampler for the 1P map: calculate the vertical tile row/fine row once
   per line. These are guarded cases of the reference transform, not
   lower-resolution rendering. Keep wrap, TILE0/FILL, flips, negative
   increments and per-line matrix changes exact.

   Initial loops cost approximately 44 DSP clocks per INSIDE pixel and
   56 per TILE0 pixel, including indexed-address/immediate instruction
   costs but excluding setup. The cache's TILE0 loop costs about 22.
   Applied to the audited pixels, even the new loops consume approximately
   **53.4 ms (1P) / 44.6 ms (2P)** by themselves at this Hatari's clock.
   This is an instruction-count estimate, independent of the ambiguous
   overlaid profile. The fused sampler must also become much cheaper.

3. **Prepare sprites once per OAM segment and cache decoded rows.**
   `obj_draw` currently scans the sprite list every row; `sl_draw` reads
   packed VRAM through `vwn`/`xl_read` and expands the same graphics again.
   Build row membership or enter/leave events once per OAMSET, keep OAM
   ordering and the 32-sprite/34-sliver limits, and cache raw decoded
   character rows/opaque masks by VRAM generation. Apply palette and
   priority separately. A race program can store the active sprite pages
   directly instead of unpacking every read from spare Mode 7 word bits.

4. **Overlap output with drawing and test the 32 MHz crossbar route.**
   Output is currently polled from `cp_loop`. A bounded transmit ring can
   drain while the next row is drawn. Reserve SSI vectors, protect ring
   ownership and include interrupt service in compute cost. CT's ring is
   a reference, not a free transplant into SMK's occupied vectors.

   The current payload is 28,672 plane bytes + 512 histogram bytes =
   29,184 bytes. The old budget's 32,256 bytes is obsolete. Ideal wire
   times are **37.10 ms at 25.175 MHz** and **29.18 ms at 32 MHz**, from
   master clock / 4 / 8. Atari specifies the 1 MB/s SSI/DMA limit in the
   [Developer Support Package, Hardware p.21](https://www.atarimania.com/documents/Atari-Falcon030-Developer-Support-Package.pdf#page=49).
   These are bounds, not measured transfers. Raising the crossbar clock
   does not raise the DSP compute clock; four tracks are already selected.
   Input handshake throughput and simultaneous play/record need their own
   deterministic benchmark. Do not add full wire time to fully overlapped
   compute, or subtract it from the polled measurement as if overlap
   already existed.

5. **Close the separate 68030 budget.** This capture's fast stream builder
   alone takes about 21 ms, excluding palette work, game code and uploads.
   The most recent recorded live CPU render cost is about 51 ms/picture
   (`analysis/translator.md`); it was not retimed here. A compact race
   command containing the matrix arrays, view state and OAM segments can
   remove most CPU per-line replay. Palette clustering must also get
   cheaper or follow an explicitly chosen update policy: unchanged CGRAM
   alone does not imply unchanged histogram weights. Current code already
   polls completion during game work and can submit again immediately;
   recommending those features as new fixes would be stale advice.

## Small experiment and acceptance gates

A frozen-source experiment hoisting the three small TILE0 masks into
modifier registers reduced full-output times to 224.973 / 216.171 ms,
with zero differing plane words: just 3.0 / 4.0 ms. It remains under
`build/m7-audit-20260926/tile0/`, not in the production renderer. The
concurrent cache supersedes it. An earlier failed variant also put the
24-bit multiply constants in modifier registers; those are only 16 bits.
Its nonzero pixel differences were rejected, not counted as a speed-up.

For repeatable isolated builds, put the assembly glue in the same isolated
directory as its `incbin` assets. Vasm resolved source-relative assets
before the supplied `-I` directory here; an initial probe silently linked
the original kernel and the wrong snapshot. Hash the source, binary and
snapshot and confirm the embedded sprite counts before comparing timings.

First prove a fused Mode 7 strip, including packing and sprite selection,
against the reference. Then benchmark full 1P/2P pictures, aiming first
below 80 ms, then 50 ms. For 25 pictures/s, demonstrate compute **including
output service** under roughly 32-35 ms and overlapped output under 40 ms,
while game work plus renderer CPU work also fits its 40 ms budget. Report
warm steady-state intervals and cold/upload spikes separately. Require
plane and histogram equality, dirty-page and OAM-segment tests, mode
transitions and broad race coverage before replacing the general path.
Measure on a physical Falcon before claiming hardware throughput.

The new host audit builds without changing the shared Makefile:

```sh
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Isrc \
  tools/m7_sampling_audit.c src/snes_ppu.c src/m7_plan.c \
  -o build/m7_sampling_audit
build/m7_sampling_audit build/racesnap/R1P.BIN build/smkx/smk.sfc \
  build/racesnap/R2P.BIN
```

On this workstation the compile additionally uses
`-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`, as the
Makefile does for its host tools. `src/renderer.c` remains unchanged as
the byte-format reference.

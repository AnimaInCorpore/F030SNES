# True-colour optimisation review, 2026-09-28

Verdict: keep the changes. They improve the fixed real-frame test set by
1.12x, with a 1.51x improvement on the strongest field case. They do not
establish the original 2x target or a live-game frame-rate improvement.

## Frozen comparison

Baseline: `cd053a5e9920311d0d429a53191fe7ffcd8e687c`, before the current
uncommitted `falcon/dsp/tc.asm` changes. Candidate source SHA-256:
`906904dc6d46a237409132aeb6d4e2c59c2399c8f346cae4f41ed02f42e28e26`.
The candidate source stayed unchanged during the comparison. Its freshly
assembled tile and TC7 binaries match the existing `build/ctx` binaries.

Each version was assembled and linked in its own directory with frozen,
identical test inputs and the same TCTEST object. The assembly glue sits
beside its own assets to avoid source-relative `incbin` selecting the
wrong images. Both versions ran sequentially through `scripts/run_smkx.py`
with unique tags and only that script's own emulator process control.

Emulated results, not physical Falcon measurements: patched Hatari
`v2.6.1-497-g639fadf6`, build 2026-09-22 21:37:14, executable
`../../../F030Arcade/third_party/hatari/build/src/hatari`, SHA-256
`b3279c6de6893a666e8e3c87b65eb4cf69f10f296a098b39272f7757731f1f7d`.

The figures are TCTEST's render+record interval, after sending the stream.
They exclude page upload/conversion and CPU planning. They are one matched
run per version; small sub-millisecond differences are not meaningful.
Group speedup is total baseline time divided by total candidate time.

| Group | Frames | Before, mean ms | After, mean ms | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Title, mode 2 | 8 | 314.27 | 294.91 | 1.066x |
| Field, mode 1 | 9 | 405.53 | 337.53 | 1.201x |
| Mode 7 | 4 | 268.65 | 239.50 | 1.122x |
| All real frames, including six blank/simple cases | 27 | 303.15 | 270.43 | 1.121x |
| Random frames | 24 | 852.44 | 746.58 | 1.142x |

Real test 19: 545.70 -> 360.83 ms, 1.512x (33.88% less time).
Real test 23, the slower Mode 7 case: 357.81 -> 314.58 ms, 1.137x.
The original 2x target for that exact case would be 178.91 ms, so the
candidate would need another 1.76x improvement to reach it.

## Correctness and implementation

- Both fresh runs: all 51 full frames and 17 banded reruns have zero
  differing words; no test timeout.
- The 12 recent runtime F*.BIN snapshots (selected after the candidate
  kernel build, copied before checking) also match `ppu_render_565`
  pixel for pixel. This covers the recent short game run, not a new
  complete attract cycle.
- Hot code is copied over the completed stage-2 loader while SSI's
  vectors/handler remain resident. The tile build ends hot code at
  P:$01E5, leaving 27 words below P:$0200; TC7 ends at P:$00DC.
  Assembly guards check the internal-P and temporary-ring capacities.
- The F4 SOLID path skips transparency tests, and the fresh-line flag
  allows stores without reading/comparing existing backdrop keys.
  Later layers retain priority comparisons.
- WSEG groups window membership into runs for layer merges. The
  colour-window path still branches from `colour` to `_slow` and calls
  `pixel` 256 times. Thus the larger colour-span optimisation from the
  earlier advice is still outstanding. Two-screen layers also retain
  LK followed by separate MAIN and SUB merges.

No correctness defect was found in the reviewed patch and tested cases.
The measured gain belongs to the combined patch; this comparison does
not attribute savings separately to relocation, SOLID or WSEG changes.
Profile the candidate before choosing the next change, especially the
general colour path versus the already fast Mode 7 colour path.

Evidence remains untracked in `build/tc-review-rdfz7t19/`: `manifest.json`,
`comparison.json`, `base/hatari.log`, `current/hatari.log`, the frozen
sources/listings/binaries, and `runtime-snapshot-check.log`.

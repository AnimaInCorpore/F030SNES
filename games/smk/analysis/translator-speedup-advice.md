# Translator speedup assessment, 2026-09-27

Twice the speed is a plausible target for selected translated routines, and
an ambitious target for the translated instruction portion as a whole. It
is not yet supported as a prediction for the entire game. This is a source
review and a host-only emission experiment, not a new Falcon or Hatari
performance measurement. No translator/runtime implementation was changed.

## What the current evidence says

The recorded release race budget is roughly 37 ms per main-loop pass
including NMI/IRQ work (see `translator.md`, "Game code towards 50 fps").
Those are **emulated measurements, patched Hatari 639fadf6**, not physical
Falcon measurements. Its timer profile also has a documented sampling bias
at the wait loop; use timed work, not that apparent wait share, for budgets.

If an optimized portion occupies fraction p of execution time and becomes
k times faster, the total speedup is `1 / (1 - p + p/k)`. For illustration,
p = 0.4 and k = 2 gives only 1.25x overall. Halving approximately 37 ms to
approximately 18.5 ms requires savings across the runtime and native DSP-1
work too. Rendered picture throughput has its own DSP budget.

There is already evidence against equating static instruction counts with
speed: moving V out of the CCR removed 175,643 emitted instructions but
changed the recorded race time from 72.0 to 68.8 ms per pass (patched
Hatari 639fadf6, emulated; a historical configuration, not today's baseline).

## Host-only comparison of precise flag liveness

Ran SMK's analysis and emission in memory, then repeated emission with the
current F030SMW `liveness.py`, imported against SMK's opcode definitions
with FAST enabled. No generated files were written. The candidate was not
assembled or checked against the reference interpreter.

| Static emitted quantity | Current SMK | SMW precise pass |
|---|---:|---:|
| Counted indented instruction lines in text sections | 427,185 | 426,964 |
| CCR captures (`move.w ccr,...`) | 19,102 | 19,002 |
| Saves reported by `emit_node` for ordinary instructions | 2,030 | 1,946 |
| `rt_cmpfix` lookups | 3,199 | 3,196 |
| `rt_x2c` lookups | 1,686 | 1,684 |
| `svs rt_vf` | 1,724 | 1,722 |

The counted-line decrease is 221, about 0.052%. This excludes instructions
sharing a line with a label and is not an assembled-byte or dynamic count.
It strongly lowers the priority of this particular pass as a large SMK
speedup; it does not prove the execution-time effect is small. SMW's later
note validated its pass after fixing the carry merge bug, withdrawing the
earlier warning. SMK already stores V separately, so SMW's historical
near-doubling cannot be carried over as an expectation.

Other static counts: 56,122 `ror.w #8` lines, 504 calls to `rt_dispatch`,
1,133 adjacent binary CLC/ADC pairs and 484 adjacent binary SEC/SBC pairs
whose second node has one ordinary predecessor. Counts include duplicated
procedure contexts and do not establish a transformation's full safety.

Working-copy identities: SMK HEAD `6184dbb`, SMW HEAD `7a3b64e`.
SHA-256 of the files actually inspected:

- SMK `emit.py`: `c2f844fc92136b6053b6ab1351794c23486af93f5f503a011a633c52167be156`
- SMK `liveness.py`: `119943ef81f015533a7af27f6c6ccd4c2c0a3e6af98dfe597a3f9e65a3e5965b`
- SMW `liveness.py`: `f1187522328f548342aef0be8466d45fac6afc92b0b6ebf0d8e961e77cbb82a3`

## Recommended order

1. **Instruction selection and basic-block fusion.** Introduce a small
   optimization stage between analysis and assembly emission, with explicit
   flag inputs/outputs and byte/word register values. Start with binary
   CLC/ADC, SEC/SBC, immediate one-bit BIT, and dead flag work. At $80A5CC,
   CLC followed by ADC #$00C0 currently emits:

   ```asm
   andi    #$EF,ccr
   move.w  #$C0,d7
   ori     #$04,ccr
   addx.w  d7,d0
   svs     rt_vf
   ```

   With the carry known clear and no entry between the instructions, this
   can become `addi.w #$C0,d0; svs rt_vf`. A binary SEC/SBC can use SUB,
   correcting the inverted borrow only where the SNES carry is needed.
   For unknown carry, keep ADDX/SUBX; omit their Z preseed only if Z is
   dead. The immediate BIT #$4000 at $80A609 currently expands into a copy,
   AND and CCR save/merge to retain N. `btst #14,d0` changes just Z and
   preserves the other flags. Check all alternate entries and widths.

2. **Keep hot memory paths short.** Propagate pointer provenance and index
   ranges through blocks/loops. Resolve a proven WRAM/ROM pointer once and
   reuse its host pointer. Use one guard for a proven range when possible,
   retaining out-of-line generic paths for I/O and boundary crossings.
   `inline_reads` currently repeats the page lookup at each unresolved
   access; merely inlining more was already worth less than a millisecond
   in the recorded experiment (patched Hatari 639fadf6, emulated).
   Preserve SMK's real $7F->$80 and $FFFF,Y crossing cases.

3. **Track register values and byte order across instructions.** Current
   liveness tracks flags only. Add A/B/X/Y liveness and endian state to
   avoid restoring a rotated value immediately overwritten by the next
   load, forward repeated loads, and fold load/store constants. Keep
   memory in its canonical SNES byte order. An 8-bit A write preserves B;
   arbitrary calls, aliases and asynchronous observers must invalidate or
   synchronize cached values. Instruction count alone does not justify
   globally changing the WRAM layout.

4. **Reduce call and dispatch overhead where proved safe.** The hit path
   of `rt_dispatch` still hashes, accesses memory, saves registers and
   returns to a second indirect call. Prefer statically translated tables
   for proven target sets or a small guarded cache per hot call site.
   Consider specialized leaf call conventions, omitting guest return-address
   stores only when nobody can inspect/pop those bytes. SMK has deliberate
   return-frame manipulation, so a blanket removal is wrong. Have `guard`
   account for which live flags its body actually clobbers; V lives outside
   the CCR and does not itself require a CCR save.

5. **Optimize the emitted layout for the 030.** Arrange hot blocks as
   fall-through, move exceptional paths out of loops, and select inlining
   by executed cost and cache footprint. The instruction cache is just
   256 bytes and direct-mapped; a loop plus its frequently called helpers
   must be considered together, including conflicts. Avoid indiscriminate
   unrolling/context specialization. Work from final assembled addresses
   and short encodings, not source line count. Verify the actual cache
   settings before attributing an improvement to cache behavior.

The hardware basis is Motorola's
[MC68030 manual](https://www.nxp.com/docs/en/reference-manual/MC68030UM-P1.pdf)
(cache organization) and
[programmer's reference](https://www.nxp.com/docs/en/reference-manual/M68000PRM.pdf)
(ADD/ADDX, SUB/SUBX, BIT/BTST and condition codes).

## Recurrent blocks and a whole-loop candidate

A second host-only CFG census followed single fall-through edges whose
destination has one ordinary predecessor, deduplicating by SNES instruction
addresses rather than procedure contexts. These are syntactic candidates,
not dynamically executed counts or proof that every instance is replaceable:

| Pattern | Distinct address sequences |
|---|---:|
| LDA / STA / LDA | 848 |
| LDA / CLC / ADC / STA | 134 |
| LDA / SEC / SBC / STA | 48 |
| LDA / AND / STA | 84 |
| LDA / ORA / STA | 112 |
| INX / INX | 334 |
| INY / INY | 172 |
| ASL / ASL | 152 |
| LSR / LSR | 181 |
| DEC / BNE | 61 |

There are also 183 distinct immediate single-bit BIT addresses. Longer
runs can produce overlapping pairs in the counts above. The load/store
patterns include I/O and must be classified before applying RAM transforms.
Repeated shifts can use a multi-bit shift; repeated increments can use one
increment by two while preserving SNES carry where live. The final N/Z,
register width, branch entries and observation boundaries still matter.

**Concrete whole-loop example: CODE_81E5A0**, in
`third_party/Super-Mario-Kart-Disassembly/SMK/Routine_Macros_SMK.asm`, line
30639 (current listing). In its 16-bit entry context it establishes bank
$7F, zeros X/Y, and for each tile copies eight words from $7F4400+Y to
$7FA000+X, then appends eight zero words. Y stops at $2000: 512 repetitions
of copying 16 bytes and clearing 16 bytes. Source $7F4400-$7F63FF and
destination $7FA000-$7FDFFF do not overlap or cross banks.

The current translation performs a page-table lookup for each source word,
rotates words for loads/stores, increments each index twice, and maintains
the loop counter as a little-endian WRAM word at $1C. A specialized native
inner loop could use four `move.l (a0)+,(a1)+`, four `clr.l (a1)+`, and a
register counter with DBRA. Raw byte copies need no endian conversion.
Keep or reproduce the routine's prologue/epilogue, and reconstruct A=$0008,
X=$4000, Y=$2000, the zero $1C counter, final carry and bank/stack/flag effects.
Before removing intermediate counter/register updates, verify that the
runtime cannot deliver a guest observer inside this region. This is a
candidate, not an implemented or validated replacement. It looks like
graphics preparation; profile its frequency before claiming a race gain.

**A contrasting boundary to respect:** CODE_808E01, listing line 1943,
walks six-byte transfer descriptors and writes VRAM/DMA registers for each.
It is recurrent work, but a bulk replacement must reproduce ordered DMA,
register shadows, address increments, dirty marks and logging semantics.
Treat it as a runtime operation with an explicit contract, not memcpy.

For the first implementation, use a small library of semantic templates
with checked preconditions and retain normal translation as the fallback.
Use basic blocks first, then loop regions after proving trip counts,
aliasing and exits. Differentially check all live exit registers and flags,
every affected memory byte, and ordered I/O when present. Unknown entry
points, return-address manipulation and changes to interrupt scheduling
need separate analysis. Whole-block replacement is a standard compiler
technique; recognizing a mnemonic string without those proofs is insufficient.

## How to decide whether 2x is achievable

Profile the same paced race frames in a release build with rendering
disabled or rare, then profile with rendering enabled separately. Rank
hot translated blocks together with helpers called from them; diagnostic
caller tracking is useful for attribution, but time the release build.
Preserve the matching executable, map, generated sources and settings.
Use only `../../../F030Arcade/third_party/hatari/build/src/hatari`, recording its
build identity, and keep eventual physical-Falcon results separate.

First implement the small instruction-selection candidates and benchmark
their hot blocks. For each semantic change, compare the paced reference
over both demo races (`make check-ref REF_FRAMES=7000`) plus menus and
scripted play, and test arithmetic/flags and memory boundaries directly.
Renderer snapshot equality alone cannot validate translated game logic.
Budget a whole-game 2x only after the measured savings sum to roughly half
the original game CPU time; local 2x results are credible earlier.

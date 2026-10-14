# 65816 → 68030 translator (tools/smkx)

The port now runs the original program, statically translated to 68030
code, instead of reimplementing routines by hand. This keeps SMK's gameplay,
timing logic and screen design; the only intended change is the reduction to 16
colours per frame imposed by the four-plane display path (an adaptive
OKLab palette, see "Palette" below).

Everything the translator writes (`build/smkx/`) is derived from the ROM and
stays untracked, like the ROM itself.

```sh
make smkx-source          # translate: ROM + pinned listing -> build/smkx/*.s
make smkx                 # assemble and link build/smkx/SMKX.TOS
make run-smkx             # run in the patched Hatari, print the runtime log
make smkx SMKX_FRAMES=600 # build that quits by itself after 600 frames
scripts/smkx_pc.py OFFSET # map a logged "pc +"/"from +" offset to 65816 code
```

## Pipeline

1. **Placement** (`listing.py`). The listing gives code/data boundaries,
   labels and mnemonics; the ROM gives every byte. All 33,729 listing
   instructions are placed at a file offset and must decode, from the ROM
   byte at that offset, to the listing's mnemonic and length. Asar details
   handled: `a : b` statement separators, `NOP #n` repeats, `BRA.w`
   assembling to a 2-byte `BRA`, and `incbin` files absent from the
   checkout (resynchronised at the next address-bearing label).
   Twelve label names disagree with where their code sits; they are listed
   in `build/smkx/report.txt`. Seven labels are entries into the middle of
   an instruction and are decoded separately.
2. **Discovery** (`analyze.py`). Starting from the ROM vectors (reset
   `$00FF70`, NMI `$00FF85`, IRQ `$00FF8A`), procedures are analysed per
   entry context: M, X, decimal flag and data bank. Each address may carry
   several states. Features:
   - an abstract stack resolves `PHK/PLB`, `PEA/PLB` and `PHP/PLP`; the data
     bank is tracked symbolically (`IN`, the caller's bank) where the entry
     bank is unknown;
   - routines that return in different widths are followed per exit
     context; callers continue once per context;
   - routines that pull their caller's return address (`PLA/RTS`,
     `PLX/JMP`) return out of the caller as well;
   - jump tables come from the listing's `dw` blocks; pointer dispatch
     through RAM (`JSR ($0000,x)`, `JML [$0FDC]`, ...) is seeded with the
     code addresses the listing uses as data, each analysed once in the
     16-bit context all such sites use;
   - a decode that disagrees with the listing is reported, not fatal. Two
     such cases were checked by hand and the listing is wrong in both
     (`$85B8BE` is reached from 8-bit code; `$84E6EE` follows a 16-bit
     `ADC` the listing itself shows).
3. **Flag liveness** (`liveness.py`) over all procedures, flowing into
   callees and from returns to every continuation.
4. **Emission** (`emit.py`), one vasm source per ~12,000 lines.

Current output: 3,287 procedure contexts over 1,895 entries, 32,662
instruction addresses (31,844 of the listing's instructions), 105,547
translated nodes. Seventy-nine nodes are unsupported (stack-relative
addressing, a few unknown widths) and 744 are traps (mostly `BRK`/`COP`
reached on glitch paths the listing marks). Both stop the program with the
65816 address if they are ever executed.

## Register and flag mapping

| 65816 | 68030 |
|---|---|
| C (A + B) | `d0.w`; `XBA` = `ror.w #8,d0` |
| X, Y | `d1.w`, `d2.w`, upper words kept zero for `(bd,An,Xn.l)` |
| DBR | `d3.l` = bank << 16; `a4` = host of bank:$0000, `a3` = bank:$8000 |
| S | `a6`, a host pointer into WRAM; stack bytes are real |
| return addresses | `a7`, the 68k stack shadowing JSR/JSL frames |
| D | always 0 in SMK; direct page = `(dp,a5)`, `a5` = WRAM |
| carry | 68k X flag (`addx`, `subx`, `roxl`, `roxr`; untouched by `move`/`ror`) |
| N, Z, V | 68k CCR, computed only where live |
| M, X, decimal | static per translated node |

Memory stays little-endian. 16-bit loads use `ror.w #8`; 16-bit ROM loads
read a big-endian word view built at startup. The exit of every routine
sets `a0` to an exit-context number; call sites with several continuations
jump through a table indexed by it, which leaves the flags intact.

### Fused compares and runtime operands (fixed 2026-09-24)

A `CMP`/`CPX`/`CPY` directly followed by `BCC`/`BCS` is fused: the branch
tests the 68k borrow in C instead of converting it to the 65816 carry in
X. When the compare's operand needs a runtime call (`jsr rt_rb` for
`CMP ($dp),Y` and similar), the instruction is wrapped in a flag merge,
and that merge took the carry from X, dropping the borrow: the branch then
always went the same way. The demo race hung in the first frame on
`$84DC06` (`INY : CMP ($16),Y : BCS`). The merge now keeps the 68k C bit
for fused compares (309 generated lines changed).

### Code the listing keeps as data

Some routines are reached only through words in data tables, written as
raw numbers rather than labels, so the analysis never sees them as
address-taken. `tools/smkx/dispatch_targets.txt` lists them, each group
with its table:

- object scripts at `$80F6DB` (run by CODE_80F879),
- frame-step scripts in bank `$81` (a bank-`$81` label plus a raw
  bank-`$80` word),
- the object-script handlers at `$85DD74-$85DF8D`, which the listing keeps
  inside DATA_85DA00 as byte-swapped `dw` words (the bytes verify either
  way). Scripts at `$85DC00-$85DD73` name them; CODE_85E0C0 calls them
  through `JSR ($0000,x)`.

The runtime reports each miss with its 24-bit target and the dispatch
site; a target inside a data label is the sign of this case.

A script can also carry a routine of its own: every handler ends in
`INY : INY : TYX : JMP ($0000,x)`, so the next script word is the next
routine, and at `$85DC3E` that word is `$DC44`, code inside the script
area (`LDX $0A : LDA $A0,x : CMP #$000C : BNE : ... JMP ($0000,x) : LDX
$B4 : RTS`). It showed up as a dispatch miss at frame 9,858, after the
demo race. A scan of the script area for words pointing into it found no
other one that decodes as code (the rest point at script cells, which
start with handler words).

## Runtime (falcon/smkx_rt.s)

- Startup under TOS 4.04, supervisor mode; word view and bank tables.
- The Falcon VBL stands in for the SNES NMI. Before it, the frame's
  V-count IRQs run in line order: a handler that sets VTIME to a later line
  runs again there, up to 8 per frame. SMK's race screen chains four
  (lines 108, 115, 212, 220: blank gap, second view, status, OAM upload).
  PPU register writes in IRQs are logged with their line; OAM is recorded
  after each IRQ as a segment used from its line on; CGRAM is copied at
  frame start so logged CGRAM writes replay on the right base. Data-port
  writes (OAM, VRAM) aren't logged: they land in their memories.
- VRAM port accesses use a long index: word addresses from `$4000` on
  used to sign-extend and hit memory before `rt_vram` (fixed 2026-09-24;
  DMA's word copy was never affected).
- Runtime memory helpers for I/O, SRAM, DSP-1 and pointer accesses.
- PPU ports (VRAM, CGRAM, OAM), general DMA, multiply/divide, WRAM port.
- APU: no sound. A handshake model of the S-SMP IPL: `$AA/$BB` ready,
  `$CC` start, echoed indices, execute on port 1 = 0, then a driver state
  that echoes and returns to the IPL on `$1F` (the value SMK writes before
  its second upload; see CODE_81F4FD's callers).
- DSP-1: the chip's own program, translated (below). Without the firmware
  the build falls back to a geometric model (`falcon/smkx_dsp1.s`) whose
  projection results are not the chip's.
- Diagnostics over NatFeats: periodic status (frames, NMIs, DMA bytes, APU
  and DSP-1 writes, sampled PC, last runtime I/O and its call site), and a
  message plus exit for every trap.

## First runs (2026-09-24)

Emulator: F030Arcade patched Hatari v2.6.1-devel, submodule `639fadf6`,
binary SHA-256 `b3279c6d…1f7d`; Falcon, TOS 4.04, 14 MB, `--cpu-exact`,
`--fast-forward` on, `--natfeats`. Frame counts are VBLs of that emulator
and say nothing about Falcon speed.

- Reset code runs; about 130 KB of DMA by frame 500.
- The first sound-program upload completes (about 108,000 port writes).
- About 3,500 DSP-1 writes, then NMI is enabled and 41 frame handlers run.
- The second upload (after `$1F`) completes; loading continues from bank
  `$85` with further DMA. The 420 s timeout ended the run near frame 1,500
  without a trap.

Startup is slow because decompression goes byte by byte through the
generic runtime paths; the fast paths for `[dp],Y` and I/O come after
correctness.

## Demo race (2026-09-24)

Same emulator build as above (patched Hatari `639fadf6`). With the fused
compare fix, the IRQ chain and 35 seeded dispatch targets, the title's
attract demo (a two-player race) runs from about frame 4,800 to the
8,000-frame limit without a trap: intro flyover, Lakitu's start lights,
then racing with the timer running and the DSP-1 in constant use. Both
views' Mode 7 rows (168 per frame) come from the DSP kernel; all 16
snapshots over the race match the host render byte for byte, with the
next palette and the split path. Rendering every 100th frame averaged
about 618 ticks of the 200 Hz timer per render (about 3.1 s of emulated
time, title frames included); the CPU vs DSP profile is next.

## DSP-1 (uPD77C25)

`assets/dsp1.bin` is the DSP-1 dump from caitsith2.com (8,192 bytes,
MD5 `4865ac61cd758b0f9383fe3d4d3b8694`, SHA-256 `5f2e5ed0…a0de349`):
2,048 × 24-bit program words and 1,024 × 16-bit data-ROM words, big-endian.
The DSP-1A in later carts has the same program and data. The file is
untracked like the ROM.

- `tools/smkx/upd7725.py` is a reference interpreter and disassembler.
  Checked against arithmetic with known answers: multiply is exact, the
  triangle command is within one unit of true sine and cosine (the chip's
  own tables, e.g. cos 0 = 32766), and command 28h returns 495 for a
  (300, 400, 0) vector, matching the disassembly's note that it is bugged
  on the DSP-1A.
- `tools/smkx/upd2m68k.py` translates the program to 68030 code
  (`build/smkx/upd_code.s`, ~14,700 lines). A, B, K, L, DP and RP stay in
  registers; flags are stored only where a flag-liveness pass says a branch,
  a carry-in or SGN can read them; M and N are formed from K×L where an ALU
  operation selects them. A `JRQM` on itself hands control back to the host.
- `falcon/smkx_upd.s` runs it synchronously: every host access to DR or SR
  first runs the program to its next poll, then applies the chip's DR/SR
  handshake (8/16-bit transfers, DRS, RQM).
- `make test-dsp1` builds UPDTEST.TOS, which replays 4,296 host accesses
  (SMK's 128-byte reset, projection setup, 192 streamed raster lines ended
  the way SMK ends them, object projections and other commands with random
  operands) and compares the 3,192 bytes read against the interpreter.
  2026-09-24, patched Hatari `639fadf6`: 3,192 of 3,192 bytes match.

## Palette

The four-plane path shows 16 colours; SNES frames use up to 256 CGRAM
entries. `src/palette.c` picks the 16 per frame, identically on the
Falcon and the host (integer-only, deterministic):

- The renderer counts, for every CGRAM index, the pixels that showed it
  (`PpuInput.hist`) and writes pixels as palette slots via `PpuInput.map`.
- Before the next render the counts and CGRAM go to `palette_update`.
  Equal BGR555 values merge. A signature over the colours and a coarse
  (log2) weight class per colour decides whether anything changed; if not,
  the palette and its slots stay, so small movement doesn't flicker.
- At most 16 distinct colours: each keeps its slot, exact colours.
- More: four rounds of pixel-weighted k-means in OKLab, seeded from the
  previous centroids (stable slots) or, the first time, by weighted
  farthest-point picks. An empty cluster takes the worst-served colour.
  A slot's output colour is the weighted sRGB mean of its members.
- Every CGRAM index then maps to its nearest centroid, so colours absent
  this frame still have a slot.
- Brightness (INIDISP) scales the 16 Videl CLUT entries; forced blank loads
  black. Fades don't touch the quantizer.

OKLab comes from a 32,768-entry table (`tools/smkx/oklab.py`, L/a/b as
int16 × 4096, 192 KB) linked into SMKX.TOS. The palette is one frame late:
frame n renders with the palette from frame n−1's counts.

SNAP.BIN v2 stores the palette used for the frame and the one computed for
the next; `make check-snap` renders the frame on the host with the first
and recomputes the second, both byte for byte. `make test-palette` covers
exact small sets, slot stability and hysteresis.

Not yet: HDMA CGRAM changes within a frame use the frame-start mapping, and
Mode 7 lines drawn by the DSP kernel aren't counted.

## Mode 7 on the DSP

The kernel `falcon/dsp/m7c2p.asm` (see its header for memory and commands)
samples Mode 7, maps CGRAM indices to palette slots and does the c2p;
each line's 64 plane words go over the SSI into DMA record (handshake
mode, four tracks, 25.175 MHz). `make test-m7` checks it against
`ppu_m7_pixels()` and `ppu_planar()` on random VRAM and lines.

In the runtime (`src/smkx_m7.c`, `falcon/smkx_m7dsp.s`):

- The kernel boots with `Dsp_ExecBoot` before the runtime enters
  supervisor mode; the SSI → DMA record route is set there too. Without
  the route (sound system locked) or with `SMKX_M7_DSP=0` the CPU renders
  Mode 7 as before.
- `ppu_render` takes an optional `PpuM7Sink`. For a Mode 7 row with BG1
  on it hands the sink the row's line and writes `PPU_EXT` where the
  stage's pixel shows; sprites that win are composed as slots. Priority-0
  sprites need the one texel under them, which the CPU computes.
- One record window per rendered frame, from the first Mode 7 row to row
  223, straight into the back buffer. CPU rows after the first Mode 7 row
  get a FILL line from the kernel and are overwritten once the window is
  full; then the CPU lays its pixels over the DSP rows.
- Mode 7 VRAM goes up in 256-word pages: every VRAM write the runtime
  handles marks its page. The colour tables go up when the slot map
  changes.
- The histogram for Mode 7 rows counts every 8th pixel with weight 8, on
  the host and the Falcon alike, so the adaptive palette stays
  reproducible.
- The screen is double-buffered: the record window writes the frame being
  composed, never the one on display.

`build/ppu_snap` renders every snapshot twice, whole and through a host
sink standing in for the DSP, and checks that both agree pixel for pixel
and count for count. `make smkx SMKX_SNAP_EVERY=n` writes `Fnnnnnnn.BIN`
every n frames; `make check-snaps` checks them all and writes a PNG of
each frame.

## Render profile (2026-09-24)

`make smkx SMKX_PROFILE=1` renders every frame twice from the same inputs
(the DSP Mode 7 path, then the CPU alone), times each stage from
`_hz_200` + MFP Timer C (`src/smkx_prof.h`, 26 us units), replays the
frame's lines through the kernel alone, and compares both frames word
for word; `scripts/smkx_render_profile.py` summarises the log. Emulated
time, patched Hatari `639fadf6` (SHA-256 `b3279c6d…1f7d`); not Falcon
figures. 30 demo-race frames (168 Mode 7 rows each), frames 4550-6000:

| stage | DSP path | CPU only |
|---|---|---|
| palette update | 33 ms | 33 ms |
| HDMA | 86 ms | 87 ms |
| BG layers (56 sky rows) | 839 ms | 839 ms |
| Mode 7 texels | DSP | 385 ms |
| sprites | 244 ms | 245 ms |
| per-pixel compose | 407 ms | 548 ms |
| feeding the kernel | 47 ms | – |
| overlay / planar | 145 ms | 216 ms |
| total | 1.83 s | 2.37 s |

The kernel alone needs 140 ms for the frame (836 us per Mode 7 row with
planning and feeding) and never made the CPU wait. DSP and CPU frames
were identical in all 120 renders. The reference renderer is therefore
not a display path: 25 fps (40 ms) needs a tile-based planar renderer on
the 68030, a kernel 4-5x faster, and a measured budget for the game code
(translated 65816 plus the translated DSP-1).

## Game-code budget (2026-09-24)

Timing marks (`tools/smkx/marks.txt`: `$808060 main_start`, `$808056
main_end`, emitted as `jsr rt_mark_<name>`) time SMK's main loop without
the interrupts; the runtime also times the translated NMI, the IRQs, the
DSP-1 firmware (`upd_run_timed`) and rendering. All in 1/38400 s from
`_hz_200` and MFP Timer C (`rt_now`, run with interrupts masked).
`scripts/smkx_game_time.py` prints per-frame figures for one main state.
Emulated time, patched Hatari `639fadf6`, rendering every 1,000 frames:

| per frame | title (state 4) | demo race (state 2) |
|---|---|---|
| main loop, interrupts excluded | 15.9 ms | 101.8 ms per main-loop frame |
| NMI | 13.4 ms | 1.8 ms |
| IRQs | 0.1 ms | 31.0 ms per VBL |
| DSP-1 firmware (inside the above) | – | 48 s over 2,250 VBLs, ~21 ms per VBL |
| game code per frame | 29.3 ms (34 fps at most) | far over the 40 ms of 25 fps |

In the race 2,250 VBLs saw only 198 main-loop frames: the IRQ path (31 ms
per VBL) leaves the main loop little time.

**Correction (same day):** the DSP-1 row above is wrong. Its clock kept
running while the VBL handler interrupted a DSP-1 access, so whole VBLs
(renders included) were charged to it; the clocks now subtract the VBL
time that passes meanwhile. See 'Where the race's time goes' below.

While adding the clock, a race between it and the VBL crashed the game
at a different frame each run (see porting notes): runtime helpers that
translated code calls with interrupts enabled must not leave temporaries
in d1/d2, because the VBL runs the translated NMI and IRQs on the
interrupted registers. CPU exceptions are now caught and reported with
registers and stack.

Findings from the other ports are in `analysis/porting-notes.md`.

## Where the race's time goes (2026-09-24)

A second PC sampler on MFP Timer C (200 Hz, IPL 6) interrupts every
context, the VBL handler included, so it sees the translated NMI and IRQs
and the renderer too (`scripts/smkx_profile.py --timer`; charge samples to
every label of the map, not only `rt_*`, or everything linked after the
game code is lumped together). Emulated time, patched Hatari `639fadf6`.

Before: 79% of all samples in runtime helpers, 37% in `rt_io_write`. The
translated DSP-1 firmware was 1%.

- OAM (and CGRAM) uploads went through general DMA's byte path: each byte
  through `rt_io_write`'s whole dispatch. `rt_dma` now copies mode-0
  transfers to `$2104` and `$2122` directly with the ports' address,
  latch and wrap rules (CGRAM only while no IRQ logs its writes). The
  first version clobbered `rt_dma`'s channel number and mask (d0/d1) and
  hung the boot in an endless multi-channel DMA.
- `rt_frame_start` copied the whole 24 KiB I/O shadow every VBL; it now
  copies only what the renderer reads ($2100-$21FF, $4200-$421F,
  $4300-$437F).

After, demo race, rendering every 250 frames: IRQs 29.7 → 5.8 ms per VBL,
one main-loop frame per 9.6 VBLs instead of 31 (3.2× faster game time),
main loop 102 ms per main-loop frame. All 16 snapshots still match the
host. Samples: runtime 53% (`rt_io_write` 11%, `rt_dma` 9%, `rt_resolve`
5%, diagnostics `rt_note_caller` 4% and `rt_now` 3%), C renderer 29%,
translated 65816 10%, DSP-1 firmware 8%.

The DSP-1's raster command, made native (`src/dsp1_hle.c`, exact; see the
hook in `tools/smkx/upd2m68k.py` and `make test-dsp1-hle`), turned out to
run only while a race loads (12,529 lines: the Mode 7 perspective tables).
In the race SMK sends about ten `project` and one `parameter` per
main-loop frame. The command mix while loading and while racing differ
completely: profile the window that matters.

## Runtime speed and native DSP-1 regions (2026-09-24, later)

Each step checked on the demo race (all snapshots byte-identical to the
host render); emulated time, patched Hatari `639fadf6`:

| build | main loop per main-loop frame | VBLs per main-loop frame |
|---|---|---|
| OAM/CGRAM DMA fast paths, small frame-start copy | 102 ms | 9.6 |
| release build (`SMKX_DIAG=0`) | 78 ms | 6.5 |
| page tables, dispatch cache, CGRAM logging fast path | 67 ms | 5.8 |
| DSP-1 `project` native (upd2c) | 62 ms | 5.4 |

- `tools/smkx/upd2c.py` compiles DSP-1 regions to value-level C; raster
  and project are exact against 12,529 and 1,265 recorded vectors
  (`make test-dsp1-hle`) and UPDTEST on the Falcon. The translated
  firmware hooks them at their heads and continues where they return.
- C called from translated code runs with the VBL masked (IPL 4): gcc
  uses a5/a6 as scratch and the VBL runs the translated NMI and IRQs on
  the interrupted registers.
- Jump tables continue across DATA labels (`DATA_85E3DE`/`DATA_85E3E0`);
  only code words are targets.

Fixed (2026-09-24): the demo race ran into a crash near its end:
routines of `CODE_85E3D3`'s table (data bank `$7F`) wrote records over
`$7FFF00`, and `CODE_84E6BA` read `$FF10` there as a type index and jumped
wild. The cause was a long,X operand placed as WRAM, see "Long-indexed
operands with a bank base" below.

## Long-indexed operands with a bank base (fixed 2026-09-24)

`CODE_85E5E3` walks a zero-terminated list of 6-byte records in ROM with
`LDA.l $850000,x` (source: `!ContextDependentTable+(CODE_85E63D&$FF0000),x`),
X = the list address (`$E40A`, `$E4A7` ...). The emitter placed a long
address by its base alone: `$85:0000` lies in the low-RAM mirror, so it
became `move.w ($0,a5,d1.l),d0`, a WRAM read at `$7E:E40A`. The garbage
there never held the terminator, and the loop wrote records until Y
wrapped past `$7FFF00`.

How it was found: `SMKX_WATCH` on `$7FFF00`, then a Hatari memory
breakpoint on the store (`b ($host).w = $10FF :once :file onhit.txt`)
gave the PC in the record loop with X = `$E4CA`, far past the end of any
list. A PC breakpoint on the routine's entry with `d1 > $24` never fired:
`CODE_85E5E3` is also inlined into its callers (`CODE_85E412`,
`CODE_85E4AF`), so an entry breakpoint must cover every copy. Reading the
generated loop showed the `a5` operand.

Fix (`tools/smkx/emit.py`, `loc`): long,X with an offset below `$100` in
banks `$00-$3F`/`$80-$BF` is resolved at runtime (`lea ($bb00xx,d1.l),a0`
and the page tables), like the existing `$00xx,x` rule for absolute,X.
42 sites changed, all in banks `$81` and `$85`.


## Fast composer and the move of rendering to the DSP (2026-09-24, night)

F030SMW's row composer (`src/ppu_fast.c`: character rows painted back to
front, a tile-row cache per VRAM word address, per-frame sprite buckets)
now composes SMK's frames, mapped to the 16-colour slots with the same
histogram counts as the reference, so every snapshot is checked byte for
byte (`tools/ppu_snap.c`: fast composer vs reference, split path, Falcon
planes, next palette). On top of SMW's version:

- HDMA reads its tables through host pointers (`PpuInput.aptr`,
  `ppu_aptr_hirom`) instead of a bank decode per byte;
- sprites go into two byte lines (colour; `$80 | priority`) a sliver at a
  time with masked long writes; the priority passes pick matching bytes
  four at a time (exact zero-byte test: `~(((v & $7F7F7F7F) + $7F7F7F7F) | v)
  & $80808080`);
- Mode 7 rows for the DSP sample their histogram texels with running sums;
- `ppu_c2p4`: 16 pixels to four plane words with two shift-ors, one merge
  and three index-bit swaps (checked against `ppu_planar` on every
  snapshot);
- the runtime marks all 128 VRAM pages for the row cache (`rt_vram_dirty`).

Switches: `SMKX_REF_RENDER=1` composes with the reference (A/B timing),
`SMKX_RENDER_FROM=n` renders only from frame n (profiles of one scene).
`scripts/smkx_game_time.py` prints ms per render.

Demo race, emulated (patched Hatari `639fadf6`, SHA-256 `b3279c6d…1f7d`),
render every 50 frames: whole render 1,908 ms -> 710 ms (64 renders,
normal builds); stage profile of the DSP path (31 renders, frames
4500-6000), per frame:

| stage | reference | fast composer |
|---|---|---|
| HDMA | 86 ms | 70 ms |
| BG layers (56 sky rows) | 839 ms | 152 ms |
| sprites | 244 ms | 128 ms |
| compose | 407 ms | 174 ms |
| feed DSP | 47 ms | 64 ms |
| overlay | – | 50 ms |
| palette | 33 ms | 38 ms |
| DSP path total | 1.83 s | 0.71 s |

The feed shows ~14,000 host-port stalls per frame: while fed, the Mode 7
kernel (141 ms per frame) does hold the CPU. Game code in the same window
is 113 ms per main-loop frame (race loading included).

**Decision (user, 2026-09-24): the 68030 runs the game only; the DSP
renders.** The C composer stays as the reference and fallback. The race
screen's VRAM (from a snapshot's per-row state): rows 0-22 and 111-134 are
mode 0 with BG2-4 (2 bpp) and sprites, rows 23-106 and 135-218 Mode 7 with
sprites; VRAM `$0000-$3FFF` Mode 7, `$4000-$6FFF` sprite characters (two
tables, OBSEL `$02`/`$0A`), `$7000-$77FF` 2-bpp characters, `$7800-$7FFF`
the sky map. The DSP has 32K words (X = the lower 16K, Y the upper, P
over both) plus 512 internal words per space: X keeps Mode 7 as now, Y
takes `$4000-$7FFF` (12K words in use) and leaves ~4K for program,
tables and buffers. The plan is in ROADMAP.md.

## The 65816 V flag outside the CCR (2026-09-24, night)

`SMKX_FLAGSTATS=1 python3 -m tools.smkx ...` counts, per emitted
instruction, which live flags needed a CCR save. 61,660 of 63,690 saves
(97%) kept only V: nearly every 68k instruction (`move`, `and`, `or`,
`tst`, `cmp`) clears V, while the 65816 changes it only in ADC, SBC, BIT,
CLV, PLP, REP/SEP and RTI; and liveness keeps V live across most of the
program because every PHP reads all flags and returns it cannot follow
keep all flags live. Only 35 BVC/BVS in the listing read V.

V now lives in the byte `rt_vf` ($FF set), like the carry lives in X:
ADC/SBC store it with `svs` where it is live (decimal mode: from the
binary operation on copies), BIT through `rt_bit8/16`, CLV and REP/SEP
#$40 with `sf`/`st`, PLP with `btst #6` + `sne`; PHP puts it into bit 6;
BVC/BVS test the byte (saving N/Z around the test where live); the
emitter drops V from what an instruction clobbers. `rt_frame` saves and
restores `rt_vf` around the translated NMI and IRQs (RTI's P comes back
through the 68k `rte`). Files: `tools/smkx/emit.py`, `falcon/smkx_rt.s`,
`falcon/smkx_rt.i`.

Result: flag saves 63,690 -> 2,030 (N/Z around stores, C), emitted 68k
instructions 546,649 -> 371,006 (3.91 -> 2.65 per 65816 node). All
snapshots of the demo run still match the host. Game time, however, only
moved from 72.0 to 68.8 ms per main-loop frame (demo race, render every
50 frames, emulated): the saves were a large share of the code, not of
the time. See the timer profile below for where the time goes.

### Where the race's game time goes, attributed correctly (2026-09-24, night)

`scripts/smkx_profile.py` now names every sample by the nearest map
symbol (it used only `rt_*` labels, which charged the DSP-1 glue to
`rt_print_hex8` and the translated DSP-1 firmware and all C code to the
last translated procedure). Demo race, frames 3000-8000, no rendering,
status every 1,000 frames (`SMKX_STATUS_EVERY`), emulated:

| part | share of samples |
|---|---|
| translated 65816 | ~20% |
| DSP-1 emulation (native `project` 9.8%, firmware + glue ~12%) | ~22% |
| register I/O (`rt_io_write` 15.9%, `rt_io_read`, `rt_iowb`...) | ~22% |
| generic memory helpers (`rt_rb/rw/wb`) | ~12% |
| DMA, dispatch, OAM segments, frame start | ~13% |

`SMKX_IOSTATS=1` counts accesses per register (8,000 frames, 1.66 M in
all): DSP-1 ports `$6000/1`, `$7000/1` ~425,000; CGRAM data `$2122`
229,000 writes; APU ports `$2140/1` ~170,000; multiply/divide
(`$4203`, `$4204`, `$4216/7`) ~120,000; DMA registers. The addresses are
known at translation time, but every access goes through `rt_io_write`'s
shadow store, IRQ-log checks and table dispatch. Next: direct entries per
register from the emitter (DSP-1 ports straight to `dsp1_read/write`,
multiply/divide inline, CGRAM and APU ports to their handlers).

## The DSP renderer (design, 2026-09-24)

Decision: the DSP draws every line from its own copy of the VRAM; the
68030 runs the game and sends state. Constraints and layout:

- DSP memory: 32K words of 24 bits external (Hatari `dsp_cpu.c`: Y is
  the lower 16K and X the upper; P runs over both, so P:$0000-$3FFF is
  Y:$0000-$3FFF and P:$4000-$7FFF is X:$0000-$3FFF) plus 512 internal P
  and 256 internal X and Y. `Dsp_ExecBoot` loads only P:$0000-$01FF.
  (A first try put program at P:$7000: that is X:$3000, which the VRAM
  upload overwrote.)
- VRAM is 32K words of 16 bits, all of it used in the race: $0000-$3FFF
  Mode 7, $4000-$6FFF sprite characters (tables at $4000/$5000/$6000 by
  OBSEL), $7000-$77FF 2-bpp sky characters, $7800-$7FFF the sky map.
- Layout: X:$0000-$3FFF = VRAM $0000-$3FFF, one word each, in the Mode 7
  format (tile << 14 | texel; both bytes recoverable). Y:$0000-$2AAA =
  VRAM $4000-$7FFF packed: VRAM words 3k, 3k+1, 3k+2 in DSP words 2k,
  2k+1 as w0 << 8 | w1 >> 8 and (w1 & $FF) << 16 | w2 (k by a multiply
  with 1/3). Y:$2AAB-$3FFF = P:$2AAB-$3FFF (~5.4K words) and internal
  memory: program
  beyond the boot stage, colour tables (CGRAM index -> slot bit per plane),
  per-frame sprite lists, one line buffer (256 pixels, internal X).
- The Mode 7 coordinate tables of kernel v1 (6K words) go: v2 steps tile
  and in-tile coordinates incrementally.
- Output as now: each finished line's 64 plane words over the SSI into
  DMA record; one record window per frame for all 224 lines.
- Budget at 25 fps: 40 ms = ~640,000 DSP instruction cycles for 57,344
  pixels, ~11 per pixel for everything.

### DSP renderer: BG path measured (2026-09-24, night)

`make test-ppu`: 96 random BG lines exact after the rewrite to
cell-at-a-time drawing (unrolled `btst`/`rol` per pixel and plane,
rank-coded pixels so that the frontmost layer is the largest value, H-flip
by drawing backwards, 8-word margins instead of clipping). DSP-internal
bench (`BENCH`: one line's drawing and composing repeated 64 times, no
host traffic), emulated, patched Hatari `639fadf6`:

| part | per line | DSP cycles per pixel |
|---|---|---|
| one 2-bpp layer | 830 us | 52 |
| composing (four buffers, colour tables to the line buffer) | 552 us | 35 |

The pixel loop is ~10-14 cycles per pixel; the rest of a layer's cost is
per character cell (~300 cycles: packed-VRAM address arithmetic, scalars in
memory, REP shifts). At these rates the race's sky (56 rows, three 2-bpp
layers) would take ~140 ms per frame: the cell overhead and the composing
must come down several times before sprites and Mode 7 are added.

Measurement pitfall: `units * 1000000u / PROF_HZ` overflows 32 bits beyond
~112 ms (4,294 units of 1/38400 s); bench totals above that came out as
nonsense (an apparent odd/even layer pattern). `pputest.c` now converts
with `units * 625 / 24`.

### DSP renderer: BG path, second round (2026-09-24, night)

Same bench and Hatari build; the bench line now has SMK's sky layout
(characters at `$7000`, characters 0-255, maps at `$7800`/`$7C00`), rows
cached (seven of eight lines of a tile row). `make test-ppu` stays exact;
three of four test lines now continue the line before, so the cache hit
path is checked too.

| per line | before | now |
|---|---|---|
| one 2-bpp layer, drawing | ~580 us | ~110 us |
| composing, no / one / three / four layers | 552 us (always four) | 122 / 170 / 250 / 283 us |
| three 2-bpp layers, total (the race sky) | 2005 us | 598 us |

What it took, in order of effect:

- **Hatari's DSP cost model.** One external data access per instruction
  is free; a second one in the same instruction (an external program
  fetch counts) costs 2 clocks. Code in external P that touches external
  data therefore runs at about half speed. Only the boot image (P:$0000-
  $01FF) is internal: the hot loops live there, including the vector area
  P:$0008-$003F (no interrupt is enabled). An assembler guard fails the
  build when the boot image outgrows 512 words.
- **Pixels from tables.** GT0..GT3 (256 words each) give pixel k of a
  four-pixel group from the two plane nibbles; transparent entries are
  negative (`$800000`), so drawing is `load; add base; store` (two
  instructions per pixel) and compose clamps to the backdrop.
- **VRAM kept in the order the tables want.** The packed half stores every
  word with bits 7-4 and 11-8 exchanged (its own inverse): the stored word
  is `left index << 8 | right index`. VRAMSET/VRAMGET and tilemap reads
  swap back.
- **No division by 3.** `$5555-$7FFF` is one word per DSP word (bits
  15-0); `$4000-$5554` lives as two bytes in bits 23-16 of the same DSP
  words. SMK's sky (characters `$7000-$77FF`, map `$7800-$7FFF`) is in the
  fast range; sprite characters partly in the slow one.
- **Row caches.** Per layer, the 33 cells of the current tile row (character
  row-0 address with flips, base value) are rebuilt only when the key
  (map row, tile column, width, character base, depth, palette, the two
  ranks, a VRAM generation counted up by VRAMSET/LOAD) changes. A fast
  2-bpp cell then costs a cached word, `y:(r0+n0)` with `n0 = fy`
  (`n1 = 7 - fy` for v-flip), two masks and 16 table moves.
- **Compose in 16-pixel groups.** Pass 1 takes the maximum over the drawn
  layers only (`CN` variants, no buffer clears); pass 2 does the c2p from
  the colour tables straight into the accumulators, two planes per
  accumulator (`A = 2A + X` with X1/X0 = the plane bits at bit 8), so the
  1024-word line buffer is gone. Layer values are `rank << 16 | PL0 +
  CGRAM index`, so a winner's low 16 bits address its colour.

Pitfalls met on the way: DSP56001 address registers hold 16 bits, so a
24-bit base value parked in `r7` lost its rank; a subroutine setting `n4`
broke its caller's descriptor walk (`(r4)+n4`); a cached word combining
`CHR + chr << 4` (up to 17 bits) with flag bits needed masking first.

At 598 us per line the sky's 56 rows take ~33 ms of DSP time per frame.
Next: sprites (milestone c), then Mode 7 v2, then the runtime.

### DSP renderer: sprites (milestone c, 2026-09-24, night)

Exact against `ppu_ref_row` on 96 crowded lines (the 32-sprite and
34-sliver limits hit), `make test-ppu`.

- The host sends the frame's sprites once (OAMSET, two words each, in OAM
  order). It leaves out sprites wholly off the screen's sides or on no
  screen row: they never enter a line's list, so the 32/34 counts do not
  change. SMK parks unused sprites at Y = 240, so this also keeps the
  per-line range pass short.
- Per line: the first 32 sprites whose rows cover it (`(row - Y) & $FF <
  H`), then their slivers from the last one back, at most 34, listed as
  (buffer address, VRAM word | h-flip, base); an internal loop draws the
  list. An opaque pixel replaces what is there, so lower OAM indices,
  drawn later, win; afterwards the slivers go back to transparent.
- 4-bpp pixels come from the 2-bpp tables: with A = GT[planes 0/1] and
  B = GT[planes 2/3] (transparent = -2^23), `S = A + 4B` masked to 23 bits
  is the colour, 0 when transparent. `AND` sets Z from A1 alone, so the
  transparency test is free; BG rows write `TRN + base` there (still
  negative), sprites keep the old value (loaded minus base, `tne`, plus
  base). 8 and 9 instructions per pixel; the unrolled 14-instruction
  `pix4` is gone.
- DSP56001 pitfalls found here: after `mpy` the product's low bits stay in
  A0, so compare A1 only; `and` on A1 leaves A2's old sign (a masked value
  still compares as negative until reloaded through A1); the shift
  constant for `x >> n` by multiplying is 2^(23 - n); a DO loop may not
  end in a `jsr`.

Emulated, patched Hatari `639fadf6`, OBJ-only line at row 100: 190 us
without slivers (range pass and composing), ~20 us per sliver (pixels 5,
the two VRAM reads 5 in the slow range, the rest list building).

### DSP renderer: memory, overlays and Mode 7 (milestone d, 2026-09-24, night)

**Memory.** With BG, sprites and their tables, Y was full to the last word
and the boot image nearly so. The Mode 7 half of VRAM kept one VRAM word
per X word in 22 of 24 bits (tile << 14 | texel). Bits 13-8 and 23-22 of
two X words now hold one VRAM word of `$4000-$5FFF` ("lanes", `xl_read` /
`xl_write`), so Y keeps only `$6000-$7FFF` (5,462 words instead of
10,923). Mode 7 pays one mask per map read; sprite characters in the lanes
cost ~2 us more per sliver. The ~5,300 freed Y words hold the Mode 7
tables, overlay images and a second program part.

**Overlays.** Internal P from `rn_cells` to `vw` (118 words) holds either
the BG cell code or the Mode 7 loops. The booted BG code is saved to Y at
the first line; the Mode 7 image is assembled for internal P but loaded
into Y (`org p:rn_cells,p:M7IMG`); a line switches only when the other
kind comes. Guard labels must be compared as differences: with separate
run and load counters `*` is the load address.

**Mode 7 v2.** Exact against `ppu_ref_row` on 96 lines (random matrices,
flips, all M7SEL modes, sprites). The host still cuts the line with
`m7_plan` (INSIDE / TILE0 / FILL); the DSP buffers the segments and
samples with tables TVM/TVF/TUM/TUF (the texel row and column split into
map and fine parts, 1024 words each) and V7 (CGRAM index -> layer value).
The INSIDE loop steps v and u in one 48-bit add (`a1 = v`, `a0 = u`, table
bases folded into the coordinates) and keeps one pixel pending, so its
colour lookup and store fill the pipeline gaps: ~20 cycles per pixel.

| per Mode 7 line, DSP alone (M7BENCH) | us |
|---|---|
| BG1 only, first version (external P) | 820 sampling + compose |
| BG1 only, overlay + pending pixel | 493 (sampling ~326) |

Measurement pitfall: timing M7LINEs sent one after another measured the
68030 instead: `ppu_mode7_setup` and `m7_plan` (divisions) cost more per
line than the DSP's rendering. Measure the DSP with a repeat command; and
in the runtime the per-line Mode 7 setup has to move to the DSP.

Frame estimate at these rates (race: 56 sky rows, 168 Mode 7 rows, ~8
slivers per line): ~34 + ~83 + ~15 ms, about 130 ms of DSP time per frame.

### The DSP renders whole frames (2026-09-25)

The 68030 no longer composes anything while the DSP route is up. Per
frame it sends:

- the 256-word VRAM pages written since the last frame (VRAMSET; the DSP
  keeps its own storage format), and the colour tables when the adaptive
  palette's slot map changes;
- the frame stream (`src/ppu_dsp.c`, FRAME): the reference frame loop
  (`ppu_render_rows`: HDMA, IRQ-logged writes, OAM segments) runs as
  before, but per row only the fields the DSP derives its lines from are
  compared with what was sent (INIDISP, BGMODE, BGnSC, NBA, the scroll
  and Mode 7 registers as resolved 16-bit values, M7SEL, TM); changes go
  out as SET records, unchanged rows as runs of LINE, a new OAM segment
  or OBSEL as the compact sprite list.

The DSP keeps those fields as a shadow and derives every line itself:
forced blank, the paint order and ranks (tables per mode), the four
layers' descriptors (map row with wide/tall maps, character base, fine
row, depth, palette base), and for Mode 7 lines `ppu_mode7_setup` and
`m7_plan` (the floored `(x y) & ~63` products from one MPY each with
pre-shifted operands; divisions only need quotients clamped to 0..256:
`N >> 8` across A1:A0 and nine DIV steps, since DIV shifts in the
previous step's quotient bit).

Every line ends with 8 extra words: the CGRAM index of each 16-pixel
group's first pixel, two a word. They land after the 64 plane words of
each screen line; the Videl's line offset (`$FFFF820E` = 8) hides them,
and the CPU adds them to the palette's histogram. The reference renderer
counts the same pixels now (every 16th pixel counts 16, in every row),
so snapshots stay comparable.

Checked by PPUTEST: random frames (mid-frame writes, forced blank, OAM
segments, mode switches between 1 and 7, all M7SEL modes) rendered by
`ppu_render` and by FRAME, compared plane word for plane word and count
for count. Found on the way: `ppu_render_rows` kept its write-twice
latches in a static across frames (reset per frame now); the planner's
second `irange` call overwrote the first one's lower bound.

Memory: the fine-column table TUF went (`tu & 7` from an AND riding
along with the TVM load), the per-line test commands went; Y holds the
second program part at $26B0-$2DBF.

Race frame on the DSP (snapshot F0001194, PPUTEST PT_SNAP bench, emulated,
patched Hatari `639fadf6`): 210 ms without the SSI, 238 ms with it;
without sprites 142 ms, without BG1 141 ms, without both 77 ms (35
sprites per OAM segment reach the DSP). In the game a render takes ~380
ms: the 68030 walks HDMA in the same loop (78 ms: seven channels, the
Mode 7 matrix every line) and CPU and DSP take turns through the one-word
host port. Next: the DSP buffers the whole stream (the CPU returns to the
game at once), then sprites and Mode 7.

### Asynchronous rendering and the 68030's remaining share (2026-09-25)

The frame stream is built into RAM and fed to the DSP by a level-6
host-port interrupt (vector $C0 via the port's IVR), so the 68030 goes
back to the game while the DSP draws; `smkx_render` returns 2 while a
frame is in flight and finishes it on a later VBL (samples, palette,
CLUT, flip). Snapshot frames and the last frame are drawn synchronously,
so their inputs are the frame's; all snapshots of a 6000-frame run still
equal the host's renders.

Race, emulated, patched Hatari `639fadf6`: the DSP shows a frame every
~245 ms; the 68030 spends ~170 ms of that on rendering:

| part | ms per frame |
|---|---|
| HDMA walk (the Mode 7 matrix every line), in the frame loop | ~60 |
| palette update and samples (`finish`) | ~42 |
| the rest of the stream (frame loop, fields, OAM lists) | ~25 |
| VRAM pages and colour tables up | 3-11 |

The HDMA walk costs ~780 cycles per channel and line even with a
pointer fast path: the frame loop's code (HDMA, register writes, the row
function) is far larger than the 68030's 256-byte instruction cache, and
every miss goes to ST-RAM over the 16-bit bus. Tight per-channel loops or
walking HDMA on the DSP (which lacks the memory for SMK's ~1.8 KB of
tables) are the ways left.

### Missing karts: frames paced by the main loop (2026-09-25)

Reported in play: racer sprites on the track were sometimes missing. The
snapshots showed it too (demo race, frame `$FA0`: player 1's kart gone,
its shadow drawn), and they matched the host render, so the renderer drew
what it was given. The OAM segment in force at line 0 had the karts'
positions but 8x8 size bits instead of 16x16: each kart was cut down to
one transparent quarter.

SMK's split-screen IRQs upload OAM every frame without a lag check:
line 108 DMAs `OAMBuffer2` (bottom screen, `CODE_808B37` ->
`CODE_80947A`), line 220 DMAs `OAMBuffer1` (top screen, `CODE_808B9C` ->
`CODE_80946E`). Only the NMI handlers (`CODE_808237` and the rest of
`DATA_8081BF`) skip their uploads while `$44` says the main loop is busy.
On the console the main loop (`CODE_808056`: `JSL CODE_81E067`, `STZ
$44`, spin at `$80805C` until the NMI sets `$44`, dispatch on `$36`)
always finishes within the frame. Here it takes about five VBLs, the
runtime ran IRQs and NMI on every Falcon VBL, and the IRQs copied buffers
the main loop was half-way through building.

Fix (`SMKX_PACE`, default 1): `rt_mark_main_start` ($808060) marks the
main loop busy; a VBL that finds it busy leaves the frame due, and the
new mark `main_wait` at `$80805C` (`tools/smkx/marks.txt`) runs it as
soon as the loop waits for the NMI (`rt_vbl_frame`, shared with the VBL).
A frame waits at most `PACE_MAX` (50) VBLs. The spin at `$80805C` is the
only NMI wait in SMK; `CODE_81E067`'s mode loads run with the NMI off.
Every SNES frame now sees a finished main loop, as on the console; frame
numbers (`SMKX_FRAMES`, `SMKX_SNAP_EVERY`, `SMKX_INPUT`) count main-loop
passes, so the demo race now starts near frame `$A8C` instead of
`$FA0`, and input scripts timed for the old count need new frames.

Checked on 7000 frames (two demo races, split-screen and one player, and
the title screens between them): all 70 snapshots equal the host
render, and a scan of every OAM segment for 8x8 kart quarters 16 px
apart (the old signature, which flags the old `$FA0`) finds none.
Emulated, patched Hatari `639fadf6`.

### The 68030's rendering share, cut exactly (2026-09-25)

With frames paced by the main loop, the demo race showed a frame every
~410 ms and the DSP (~210 ms a frame) was idle half the time: the 68030
was the limit (rendering work plus ~3 main-loop passes per shown frame).
Every step below keeps the output identical; figures are the demo race
(state 2), `SMKX_DIAG=1`, emulated, patched Hatari `639fadf6`
(`scripts/smkx_game_time.py`, which now also reads the VBL count the
timing line carries: with pacing a frame is a main-loop pass).

| step | 68030 per render | frames shown/s | apart |
|---|---:|---:|---:|
| pacing only (`c5b9f06`) | 175 ms | 2.01 | 497 ms |
| no idle gap: flip and restart in one call, VBL shows finished frames (`5567043`) | 163 ms | 2.44 | 410 ms |
| palette: colour hash, per-index map cache, 16-bit distances; HDMA: matrix units in one step; dirty-field scan by bytes | 153 ms | 2.51 | 399 ms |
| palette: nearest slot from last time's, pruned on partial sums | 133 ms | 2.63 | 380 ms |
| palette computed once per frame (at the next start) instead of at finish and again after a CGRAM change | 122 ms | 2.72 | 368 ms |
| the stream fed to the DSP while it is built | 129 ms | 3.12 | 321 ms |

- `palette_update` (`src/palette.c`): distinct colours through a stamped
  32K table instead of a linear search; the CGRAM-to-slot map kept per
  index and recomputed only where the colour or the live slots changed;
  squared distances with `muls.w` (OKLab is scaled by 4096, so every
  difference fits 16 bits); `nearest_from` starts at the colour's last
  slot and drops a slot once a partial sum exceeds the best, ties still
  to the lowest slot. Checked against the committed version on 16,000
  calls (half with an unchanged palette), identical.
- `finish()` (`src/smkx_video.c`): an asynchronous finish no longer runs
  `palette_update`; the next start does, from the finished palette, its
  counts and CGRAM then, which is what either path produced before (a
  sync finish still computes it: the snapshot keeps `smkx_pal_next`).
- `hdma_line` (`src/snes_ppu.c`): a mode-2 unit into `$211B-$2120` sets
  the register, the matrix entry, the latch and the dirty bit at once, the
  state two `reg_write`s leave. `dsp_row` (`src/ppu_dsp.c`) skips clean
  bytes of the dirty mask.
- Streaming (`src/smkx_m7.c`, `falcon/smkx_m7dsp.s` `rt_feed_more`): the
  feed starts empty and the builder publishes every 32 words; the DSP draws
  the first lines while the 68030 walks the later ones. A full buffer is
  drained, then refilled (the old overflow path would have sent the
  buffer's start twice).
- The colours go to the Videl at the VBL where the new screen base takes
  effect (`smkx_clut`, `rt_clut_due`), not mid-frame.

Checks: the host renderer with the new code reproduces the planes and
palettes the old code drew on the Falcon (70 snapshots); PPUTEST 48
random frames and the VRAM round trip. **Correction:** the two 7000-frame
snapshot runs first reported for the lazy palette and for streaming ran
`SMKXSNAP3.TOS` and `SMKXSNAP4.TOS`, which Hatari's GEMDOS drive shows
under 8.3 names: both loaded the older `SMKXSNAP.TOS` (same load
addresses in the logs). The tree with both changes was checked later, in
the native DSP-1 run below: all 70 snapshots equal to the host.

The PC samples (timer C, last 100 s of the race) after the palette steps:
`palette_update` 13% (k-means whenever the colour set's signature
changes, which the sampled weights make frequent: changing that is a
visual-policy choice), the frame stream (`ppu_render_rows`, `dsp_row`,
`to_feed`) 15%, `smkx_m7_start` (VRAM pages, colour tables) 6%,
diagnostics (`rt_now`, `rt_note_caller`) 10%. A release build
(`SMKX_DIAG=0`, `make play-smkx`) runs the game code at 58 instead of
77 ms per main-loop pass. (A profile build that seemed to render
nothing was the same 8.3 clash: `SMKXPROF2.TOS` loaded an older
`SMKXPROF.TOS`.)

**Program names for Hatari:** keep them to 8.3 (`GAMENAT.TOS`, not
`SMKXGAME2.TOS`). A longer name is truncated on the GEMDOS drive and can
silently resolve to another program; compare the "WRAM at" line of the
log with the build's before trusting a run.

### Native DSP-1 commands (2026-09-25, night)

Game-only profile of the race (release build, rendering off, patched
Hatari `639fadf6`): 55 ms per main-loop pass, of which the DSP-1 took 45%
(the translated firmware, the compiled `project` region at ~770 us a call,
and ~155 host accesses a pass entering and leaving it), the translated
65816 code 24%, the generic memory helpers 13%, I/O and DMA 7%, dispatch
5%. A recorded race log (`SMKX_DSP1_LOG`, 549K host accesses) has, per
pass, ~20 `project`, ~3 `parameter`, ~7 `triangle`, ~1 `distance`,
~1.5 `multiply`; `raster` only while a race loads.

`src/dsp1_native.c` runs those five commands natively: each follows the
firmware on values only (flag A's OV1/S1 tracked where SGN reads them;
normalization by `bfffo`). `falcon/smkx_upd.s` (`UPD_NATIVE`,
`SMKX_DSP1_NATIVE=1`) starts a native command when its byte arrives at
the command poll, collects the parameter words with the chip's
handshake, calls `dsp1n_exec` once and serves the results, then leaves
the firmware at its command poll as `$000-$004` do (B `$C0`, SR `$8400`,
DR `$0080`). The other registers are dead there: replaying the race log
with A, TR, TRB, K, L, DP, RP, all flags and the return stack scrambled
at every command poll changes none of the 29,212 commands' reads.

`tools/smkx/dsp1_native.py` checks the C against the interpreter: nine
subroutines (3,000 random cases each), the five commands (2,000 random
parameter and RAM sets each: results and all 256 RAM words), and the race
log replayed through the same host protocol with every native command
in place (28,955 commands, every logged read, SR included): all equal.
`tools/smkx/upd_live.py` is a register liveness pass over the firmware
(context-insensitive, so conservative at `$000`).

Falcon run of the native build (`SMKXNAT.TOS`, `SMKX_DIAG=1`, demo race,
patched Hatari `639fadf6`): game code 77.6 -> 48.9 ms per main-loop pass,
11.4 passes a second, 3.22 frames shown a second; a 7000-frame run
(two demo races) with all 70 snapshots equal to the host render.

**The game state depends on speed.** The same binary run twice gives
identical WRAM and VRAM in every snapshot, but two firmware builds that
differ only in `SMKX_DIAG` (speed) differ by 8,000-31,000 WRAM bytes from
frame 400 on, more than native vs firmware DSP-1. Something outside the
paced main loop sees how many VBLs pass (boot before the main loop runs,
waits on `$4210`/`$4212`, the APU handshake, the `PACE_MAX` fallback):
not yet found. Until it is, game states from builds of different speed
cannot be compared; the native DSP-1 rests on the interpreter proofs.

### Game code towards 50 fps: runtime and translator steps (2026-09-25, night)

Release build (`SMKX_DIAG=0`), demo race, rendering every 500 frames,
emulated in the patched Hatari `639fadf6` (`scripts/smkx_game_time.py`;
programs named 8.3, load addresses checked):

| step | ms per main-loop pass | passes/s |
|---|---:|---:|
| before the native DSP-1 | 55.1 | 17.6 |
| native DSP-1 (`52e4101`: fixed for release builds, CT's runtime fixes) | 42.3 | 22.7 |
| indexed low-RAM operands inline (`$00xx,X/Y` with a known data bank, `$bb0000,X`) | 41.8 | 23.0 |
| DSP-1 words: `rt_wdspw`, word paths in `rt_rdspw`/`rt_rdsp_srw` | 39.9 | 24.0 |
| every runtime-resolved read inline (`rt_pages_r` lookup) | 39.5 | 24.2 |
| a VBL during a frame run counts for the next frame | 38.5 | 24.9 |

- `tools/smkx/emit.py`: `low_split` tests an indexed pointer's offset and
  reads/writes the low-RAM mirror at `a5` directly below `$2000`; with
  `inline_reads` every other runtime-resolved read does `rt_rb`/`rt_rw`'s
  page-table lookup at the call site (`bfextu` keeps X, the carry), I/O
  pages and page-crossing words still go to the runtime. 16-bit stores to
  the DSP-1 data register call `rt_wdspw` once.
- The PC samples (DIAG builds note the call site) put 85% of `rt_rw` at
  indexed pointers, yet removing the call halved `rt_rw`'s share and saved
  under a millisecond: on a Falcon without fast RAM the inline code fetches
  about as many words from ST-RAM as the call did. Count instructions
  executed, not calls saved.
- `rt_vbl`: a VBL that finds a frame still running (`rt_in_frame`) now
  makes the next frame due; it was lost for pacing, and a short pass then
  waited for a later VBL.

Checks: each step's build ran 7000 frames (two demo races) with all 70
snapshots equal to the host.

Where the 38.5 ms go (timer C): the translated 65816 code about a third,
the native DSP-1 about 12% (`dsp1n_exec` 6%, `inverse`, `norm32`,
`denorm16`), `rt_dispatch` 7%, `rt_rw` 5%, `rt_dma` and `rt_io_write` 7%,
waiting for the next VBL ~5% (1,700 passes took 3,419 VBLs). 50 fps needs
the pass at 20 ms: the translated code itself (flag and register
handling, indirect jumps through `rt_dispatch`) is the next and largest
piece.

Later the same night: the main loop's time at its NMI wait is now timed
directly (`rt_t_wait`, last field of the timing line): 0.03 ms a pass.
There is no idle time; the ~15% the timer-C profile put at the wait
(`rt_mark_main_wait` and the spin at `$80805C`) is a sampling artefact,
so the other shares read ~18% low. Then `rt_dispatch`'s hit path lost its
trap-report ring outside DIAG builds and builds the key with one `bfins`,
and `rt_dma` copies VRAM uploads whose source and VRAM address both step
by one as long words: 38.5 -> 37.6 ms a pass (25.4 passes/s). I/O
statistics (`SMKX_IOSTATS`): only 2-3 writes per register and frame, so
`rt_io_write`'s ~3% is cost per write, not traffic.

## The reference interpreter; far karts cut to one quarter (2026-09-25, night)

Reported in play: far racers on the top screen showed only their left
side. Snapshots matched the host render, so the renderer drew what it got;
the top view's OAM high table said "small" where the graphics were a
16x16 block, and the kart's three other entries were parked at y = $E0.
Reading the listing did not converge: F030SMW's reference interpreter did.

`tools/ref65816.c` (from F030SMW, `make check-ref`) runs the ROM on an
interpreted 65816 with the runtime's hardware model mirrored: SMK's HiROM
map (`rt_resolve`: SRAM at $20-$3F/$A0-$BF $6000, the DSP-1 in $00-$1F/
$80-$9F), the I/O and DMA as the runtime does them byte by byte, SMK's APU
echo model, F030CT's SLHV/OPVCT model, the IRQ chain of `rt_frame`, and
the DSP-1 as its own program on an interpreted uPD77C25 (the C port of
`tools/smkx/upd7725.py`). It delivers a frame whenever the main loop waits
for the NMI ($80805C with $44 = 0), as SMKX_PACE does, and prints a WRAM
sum per frame (after the IRQs, before the NMI, without the stack page
$1F00-$1FFF); a SMKX_SUMS=1 build prints the same (`rt_wram_sum`).
`scripts/smk_refcompare.py` finds the first frame that differs; a
reference dump (`build/ref65816 ... N+1 ref.bin`), a snapshot at the same
checkpoint (SMKX_FRAMES = N+1) and `REF_WATCH=offsets REF_WATCH_FROM=N-1`
find the bytes and the instruction that writes them. 3000 frames run in
0.6 s on the host.

Three bugs, each found this way:

1. **Pacing fallback (runtime).** After PACE_MAX (50) VBLs a busy main
   loop got its frame anyway, IRQs and NMI included. The intro's long
   passes hit it at points that depended on speed: first difference at
   frame 25 (`$44` = $FFFF at the checkpoint), and the reason the game
   state differed between builds of different speed. Now a busy main loop
   gets no frame (a VBL only shows a finished picture), and frames wait
   for the main loop's first wait (`rt_main_busy` set at start). SMK
   waits for the NMI nowhere else.
2. **Indexed absolute addressing across a bank (shared emitter).**
   `CODE_80BD84` stores a sprite group's size/X-high byte with
   `STA.w $FFFF,Y` (the listing's `!ContextDependentTable-$01,y`) with the
   data bank at $80. The 65816 carries base + index into the bank:
   $80:FFFF + $0401 = $81:0400, the OAM high table in low RAM. The emitter
   classified the operand by its base ($80:FFFF: ROM) and stored through
   `a3` into the Falcon's copy of the ROM: the size bits never changed
   (and the ROM image was written to). This is the far-kart bug: groups
   drawn as one small sprite by this routine kept stale size bits.
   `emit.py` now sends indexed absolute operands with a base of $FF00 or
   more through the runtime with the 24-bit sum (first difference at
   frame 42: `$0400-$0408`).
3. **A VBL between the wait and `main_start` (runtime).** Between leaving
   the NMI wait and $808060 the busy flag was still clear: a VBL there ran
   a frame whose NMI found `$44` set (a lag frame), and the game fell one
   pass behind (frame 386: `$38` and every racer's frame counter one
   less). `rt_vbl_frame` now marks the main loop busy after every frame.

4. **Indexed bank-$7F addressing past the bank (shared emitter).** In the
   second demo race, frame 5151, the sums parted over one byte, `$083D`
   (an angle in the racers' table at $0801,X). The writer ran on both
   sides with the same inputs except in one call; `REF_TRACE=lo-hi` (the
   registers at every instruction in a PC range) led back to `$81FCD4`,
   `LDA $7F5000,X`: SMK looks up its 128x128 map with X = $F216, a kart
   above the map's top edge. The 65816 carries $7F:5000 + $F216 into
   $80:4216, the divider's result register; the translation read
   `smkx_wram+$15000+X`, 16 KB past the end of WRAM. `host_or_rt` now
   marks bank-$7F operands indexed by a 16-bit register (`Loc.cross`):
   `cmpi.w #limit,index; bhi` to the runtime path with the 24-bit sum,
   kept out of line after the procedure (`Emitter.outline`), so the
   common path costs a compare and an untaken branch. An 8-bit index
   cannot carry a base up to $FEFF: no guard there. 1086 sites; the race
   pass costs the same with and without them (37.33 / 37.34 ms game code,
   release, frames 1750-3450, emulated on the patched Hatari 639fadf6).
   Bank $7E needs none (it runs on into $7F in `smkx_wram`). Not covered:
   a low-RAM mirror base ($0000-$1FFF in banks $00-$3F) whose sum passes
   $2000 still reads WRAM, where the SNES has I/O or ROM.

After these, 3000 frames (boot, title, into the race) have equal sums on
both sides; with the fourth, 7000 frames (boot, title and both demo
races, states 4, 2, 4, 2, 4) are equal (`make check-ref REF_FRAMES=7000`).
Each of the four differences was a bug, in the runtime or the emitter,
not noise: with frames given only at the main loop's wait, a build's game
state no longer depends on its speed.

## Jump tables bounded; the V counter per frame (from F030CT, 2026-09-25)

F030CT found `JSR/JMP (abs,X)` tables the translator had read short (a
label inside the table, handlers left as incbin); an index past the end
jumped through the next table's words. Taken over:

- `emit.py` `table_bound`: before every translated table jump,
  `cmpi.l #2*n,d1; bcc.l <table>_oob`; the stub stops in `rt_tableoob`
  with X and the 65816 site. 768 table jumps in SMK. At `JMP (abs,X)`
  this copy also saves the CCR around the compare when flags are live
  into the target (`table_jump` gets `live`); F030CT's version clobbers
  N/Z there.
- `tools/smkx/tables.txt` ("$BBAAAA count", read by `analyze.table_lengths`
  and used first in `table_targets`) and `scripts/table_audit.py` (adapted:
  `listing.parse`, the FastROM banks). SMK's audit had four suspects, all
  false positives: the ROM run from `$818F6B` goes on into the code after
  it, and from `$81F88B`/`$81F891`/`$81F89F` into `DATA_81F8AD`, whose
  bytes ($D418 ...) name code; the reader had read all of them in full
  (the `DATA_` continuation even reads past $81F88B's 3 entries). Their
  real lengths are in `tables.txt` now, so the audit comes out empty and
  the guards bound them exactly.
- `clr.w rt_vline` at the top of `rt_frame` (and `vline = 0` per frame in
  `tools/ref65816.c`): the modelled SLHV scanline starts at 0 every frame.
  SMK's code has no static `$2137`/`$213D` read, so nothing changes here.

## Windows on the main screen (2026-09-25)

The race census showed SMK's title menu (1P/2P GAME, mode, class) missing
on the Falcon: it is a box revealed by a window (HDMA channel 1 moves
WH0/WH1 per line, `TMW` $03 masks BG1 and BG2 inside it), and no renderer
modelled windows. Now they are, for the main screen:

- `ppu_win_spans` (`src/snes_ppu.c`): where a layer (BG1-4, OBJ) is hidden
  on a row, as at most three spans [x, x + len): window 1 WH0..WH1,
  window 2 WH2..WH3 (empty when left > right), enabled and inverted per
  layer by W12SEL/W34SEL/WOBJSEL, two combined by WBGLOG/WOBJLOG (OR, AND,
  XOR, XNOR), and only where `TMW` has the layer. Checked against a
  per-pixel evaluation on 200,000 random register sets. Colour windows,
  colour math and the sub screen are still not modelled.
- The reference row (`ppu_ref_row`, and `compose_m7_ext` for Mode 7 rows
  with the external stage) clears the hidden pixels of each layer before
  composing. The fast composer hands rows with spans to the reference
  (menus only).
- `reg_used` walks HDMA channels on window registers only when windows
  can hide something in the frame (`windows_possible`: `TMW` and a window
  select for the same layer among the frame's start values and IRQ
  writes, or HDMA on the selects or `TMW`). SMK's race moves window edges
  every line (channel 7) without ever hiding a layer, so the race keeps
  its cost.
- The DSP stream (`src/ppu_dsp.c`): 15 new fields (`F_WIN`, three spans
  per layer, `x << 12 | len`), recomputed when a window register changed
  (one dirty bit, 26).
- The DSP kernel (`falcon/dsp/ppu.asm`): the register shadow moved to
  Y:$2D70 (41 fields; the old place ran into the Mode 7 overlay's image),
  `win_mask` fills a layer's spans with its transparent value after
  `ren` (BG layers), after `obj_draw` (sprites) and after the Mode 7
  segments (with V7[0], so that a Mode 7 line copied without sprites shows
  the backdrop there).
- PPUTEST randomises the window registers in half of its frames and
  writes them mid-frame.

## CPU -> DSP by DMA playback: the SSI input scaffold (2026-09-25)

Goal: the 68030 builds what the DSP needs in memory and moves none of it.
Today the frame stream goes over the host port from an interrupt
(`rt_feed_isr`: the 68030 writes every word) and VRAM pages likewise. The
scaffold routes DMA playback into the DSP's SSI receive instead:
`Devconnect(DMAPLAY -> DSPREC, 25.175 MHz, prescale 1)`,
`Dsptristate(xmit off, recv on)`, 16-bit stereo, 4 tracks: 8 words a
crossbar frame, 49,170 frames/s, at most 393,000 16-bit words/s.

- `falcon/dsp/ssirx.asm`: the receive interrupt (vector $000C, a fast
  interrupt) stores each word in a Y ring through r7/m7; host commands
  arm, count, read back, sum and burn DSP time.
- `falcon/ssitest.s` (`make test-ssi`): plays a 4096-word pattern twice
  per crossbar mode, the DSP idle (every word read back and compared) and
  busy for ~20 ms (count and checksum).

Results (patched Hatari 639fadf6):

| mode | DSP idle | DSP busy |
|---|---|---|
| free-running (no handshake) | 8-13 words lost of 4096 | up to 388 lost |
| handshake | exact | exact |

Free-running, the crossbar shifts a word every slot whether or not the
DSP took the last one. Hatari runs the DSP only between 68030
instructions, and a late crossbar interrupt delivers two words back to
back: the first is lost (Hatari sets no overrun flag). A real Falcon runs
the DSP concurrently (about 40 DSP instructions a word), but the loss
under Hatari is reason enough. Handshake: the DSP asks for every word by
setting PC4 (SC1, a GPIO output) in port C (`bset #4,x:m_pcd` in the
interrupt, after storing the word); Hatari's crossbar moves one word per
request and, until the DSP has asked once, runs free, so the first request
goes out before the DMA starts. 4096 words took 2-5 VBL ticks of 5 ms:
the crossbar's full rate.

The same route and conclusions are in ScummVM's Falcon DSP c2p
(`scummvm-dsp-c2p`, `devtools/atari-falcon030/tools/ssi-dma-c2p`,
`dsp/hsc2p.asm`): DMA playback in and DMA record out, both in handshake
mode (SC1 asks for input, SC2 lets record take output), with two fast
interrupts and 32-word rings; its gates show exact data in quiet and busy
passes under Hatari. SMK's output side already uses SC2 (PC5) the same
way, so input and output can share the SSI.

### In the renderer (same day)

The renderer takes its commands from the SSI, without an interrupt, a
ring or a spare register:

- `get` (boot image) tests the receiver's enable bit (CRB RE, set by the
  new host command SSION, cleared by SSIOFF over the SSI) and goes to
  `get_x`, which waits for RDF, reads a slot and asks for the next one at
  once (`bset #4,x:m_pcd`): one slot is always on its way, none can be
  lost, and the order holds across play windows (the request made after
  a window's last slot takes the next window's first). A protocol word is
  two slots (bits 23-8, then bits 7-0, added: `get_x` may change `b`,
  which no caller keeps across `get`); VRAMSET's and COLOUR's data words
  are one slot each (`getw`). PCC $01C0 (SRD too), PCDDR $0030 (PC4 and
  PC5 as outputs). The boot image grew by two words (510 of 512).
- The driver (`src/smkx_m7.c`) sends SSION after the PING. Per frame it
  builds one buffer (the dirty VRAM pages, the colour tables when the slot
  map changed, the frame stream from `ppu_dsp_frame`), pads it with no-op
  commands (0) to whole crossbar frames of eight slots, arms the record
  window, then plays the buffer (`rt_ssi_play`: the play registers are
  selected with the record bits kept; `rt_m7_record` likewise keeps a
  running play window). A frame is in when play and record are done. A
  stream that outgrows the buffer (16,384 words) plays its uploads only
  and skips the picture (`smkx_m7_skips`). Without SSION the host-port
  feed is used as before.
- PPUTEST's SSI pass: SSION, new random VRAM and colour tables over the
  SSI, 8 BG, 8 sprite and 8 Mode 7 frames with their streams by DMA, all
  equal to the C renderer, then SSIOFF (the last slots: after it nothing
  is read, so the padding goes before it) and a PING over the host port.

Not done: VRAM pages are copied into the buffer (the DSP could take them
straight from the VRAM shadow with swapped bytes, in play windows of their
own), and the DSP waits for every slot it reads (one slot of prefetch).

Checked: PPUTEST (host and SSI passes), 13 menu snapshots and the
7000-frame run's 70 snapshots, all equal to the host render, with "Mode 7
on the DSP, its commands by DMA playback".

Timing in the race (release, rendering on, frames 1750-3450, emulated on
the patched Hatari 639fadf6; relative figures only):

| | host-port feed | SSI by DMA |
|---|---|---|
| game code per main-loop frame | 40.2 ms | 37.0 ms |
| main-loop frames per second | 15.0 | 18.1 |
| 68030 time per picture in `rt_render` | 117 ms | 116 ms |
| pictures per second | 3.27 | 2.67 |

The feed interrupt's time (counted as game time before) is gone: the
game gains 3 ms a frame. The 68030's work per picture is unchanged: it
was never the host-port writes, it is the palette (`palette_update`) and
building the stream (the frame loop's line-by-line replay in
`ppu_dsp_frame`). The DSP now waits at the crossbar's pace for every word
it reads (two slots, ~2.5 us each, one slot of prefetch), so a picture
takes longer: more prefetch (an interrupt-fed ring) would overlap that
with drawing. For the 68030 to do almost nothing, the palette and the
stream building have to move to the DSP as well.

## The 68030's render work per picture: 117 -> 51 ms (2026-09-26)

Measured with the new timing-line counters (`smkx_t_pal`, `smkx_t_smp`,
`smkx_t_up`, `smkx_t_stream`; `scripts/smkx_game_time.py` prints them per
picture), race frames 1750-3500, release with rendering, emulated on the
patched Hatari 639fadf6 (relative figures only):

| per picture | before | now |
|---|---|---|
| palette (`palette_update`) | 31.4 ms | 23.6 ms |
| samples | 4.3 ms | 0.4 ms |
| VRAM pages and colour tables into the buffer | 7.8 ms | 3.0 ms |
| the frame stream | 74.0 ms | 22.5 ms |
| all rendering work | 117 ms | 51.2 ms |
| main-loop frames per second | 18.1 | 21.9 |
| pictures per second | 2.67 | 3.25 |

**The 68030's instruction cache.** The 68030 has 256 bytes of instruction
cache and runs this code from ST-RAM. `ppu_render_rows` (1812 bytes, HDMA
inlined) plus `dsp_row` (858) is ten times that: every line fetched its
code from ST-RAM again, about 5000 cycles a line (74 ms a frame), while the
host runs the same stream in 10 us. Hot loops have to be small functions
of their own.

- **The stream's fast path** (`ppu_dsp_frame_slots`, `src/ppu_dsp.c`):
  the frame loop is split into `ppu_frame_start` / `ppu_frame_line`
  (`src/snes_ppu.c`, `PpuFrame` and `Hdma` in `src/ppu_internal.h`), and
  runs of plain lines (no IRQ write, no OAM segment, no HDMA entry change,
  only repeat-mode Mode 7 matrix channels transferring, from contiguous
  memory) go through `fast_run`, a 104-byte loop that writes the SET
  records `dsp_row` would write straight into the SSI slot buffer. 212 of
  225 lines in the race. `build/stream_check` compares it with
  `ppu_dsp_frame` word for word: 1138 snapshots of every mode, equal.
  What remains (PPUTEST's `bench-race`, 1P race picture): frame start
  1.6 ms, 212 fast lines 4.6 ms, 13 slow lines 15.1 ms (lines 1, 108 and
  220 with the OAM segments and IRQ writes about 9.5 ms of it).
- **The histogram on the DSP:** each group's first pixel counts in HIST
  (Y:$2CA0, 256 words); after the last line the DSP sends the 256 counts
  instead of 8 sample words a line, and clears them. Lines are 64 words
  (the Videl line offset is 0 again), the output 32,256 -> 29,184 bytes a
  picture, and the 68030 reads 256 counts instead of 1792 sample words.
  PPUTEST compares the counts with the reference's histogram.
- **Uploads:** VRAMSET and COLOUR go into the buffer by tight loops (the
  VRAM shadow's bytes swapped), no function call per word.
- **Palette:** the k-means assignment and the final mapping are small
  functions of their own, and the mapping computes each distinct colour's
  slot once per call. The same palettes to the byte (201 consecutive race
  frames replayed on the host). The clustering runs for every picture:
  some colour's weight class always changes.

Tools: `make bench-race` (PPUTEST's snapshot benchmark on
`build/racesnap/R1P.BIN` and `R2P.BIN`, from `make race-snaps`; it now
also checks the SSI frame against the reference and times the slot
builder by part), `scripts/dsp_profile.py` (Hatari's DSP profiler read at
a breakpoint; hottest DSP addresses with the listing's text).

**The DSP's picture** (1P race snapshot, the frame as it is, emulated):
about 7.5 M DSP cycles. Mode 7 sampling 33%, sprites about 19% (the line's
list, slivers, packed-VRAM reads through `xl_read`), compose 11%, c2p 9%,
Mode 7 line planning 5%, output 5%. For 25 pictures a second it would
have to fit 1.28 M cycles.

> Copied from F030SMK on 2026-09-24: the design and history of the
> translator as written for *Super Mario Kart*. Notes for this port go
> in ROADMAP.md and analysis/porting-notes.md.

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

## Findings from the F030SMW port (2026-09-24)

`../smw` runs this translator on *Super Mario World* (details in its
`analysis/smw.md`). What carries over to SMK:

- **Interrupt entries and the decimal flag.** SMW never sets D, so its NMI
  and IRQ entries are now analysed with D clear instead of unknown; that
  removed 32 `ADC`/`SBC` nodes that could not be translated. Check SMK the
  same way (any `SED`, or `SEP`/`PLP` that can set bit 3?) before seeding
  `analyse()`'s interrupt contexts with `d=False`.
- **Inline jump tables after a call.** SMW's `JSL ExecutePtr` pulls its
  return address and jumps through the `dw` table that follows the call.
  Treating such a call as a table jump in the caller (the emitter repeats
  the routine's side effects, then jumps through a label table) turned
  about 20,000 unreached instructions into reached ones. Worth a search in
  SMK for routines that `PLA`/`PLX` their return address and index a table
  behind the call site; the hand-written form `PHK : PER $0003 : JML` also
  occurs.
- **Code the game builds in RAM.** SMW writes a 128-store OAM clear routine
  to `$7F8000` at reset. It is not translated; the call becomes a runtime
  helper (`rt_oamreset`). Any `JSL` into `$7E/$7F` in SMK's report ("code
  outside ROM") would be handled the same way.
- **Placement checked against a symbol file.** For an Asar source,
  `tools/smkx/asar.py` (F030SMW) walks includes, conditionals, defines and
  macros and checks each label against Asar's `.sym` output: 9,271 labels
  agree. Asar lesson: match directives by case (`rep` is a directive,
  `REP` an instruction).
- **APU return-to-loader request differs per game.** SMK writes `$1F` to
  port 0, SMW `$FF` to port 1; the model's state-2 condition is the only
  game-specific part of `rt_apu_write`.
- The Makefile in F030SMW accepts the ROM as `.sfc`, `.smc` or a ZIP holding
  one (`ROM ?= $(firstword $(wildcard ...))`).

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
- **Flag liveness: escaped returns leaked V everywhere (shared fix, not yet
  applied here).** In SMW 34,344 of 95,540 nodes saved and restored the
  CCR only to keep V. `liveness.compute` marks every RTS/RTL of a procedure
  with `escaped_to` as "all flags live"; a single such return in a widely
  called routine makes V (almost never redefined) live through every
  routine it calls. F030SMW's `tools/smkx/liveness.py` instead adds edges
  from such a return to the continuations of the escaped caller's callers
  (keeping the direct-caller edges too) and pairs PHP with PLP
  (`php_pairs`: a PHP reads only the flags live after its PLP). CCR saves
  fell to 636. Check SMK's generated code for `move.w ccr,d5` counts and
  port the change.
- **DMA fast paths (shared runtime, not yet applied here).** OAM, CGRAM and
  VRAM DMA other than "mode 1, increment 1 after the high byte" went byte
  by byte through `rt_io_write`; SMW's NMI overran the VBL because of it.
  F030SMW's `rt_dma` has direct loops for VRAM (pairs or one half, any
  increment, fixed or incrementing source), OAM and CGRAM (CGRAM only when
  no IRQ log is being written).
- **Forced-blank frames are not composed** in F030SMW (`smkx_render`
  returns 0, the CLUT goes black, `ppu_snap` skips such snapshots); loading
  screens stop paying for the reference renderer.
- Measuring the translated game's real frame rate: SMW's `TrueFrame` ($13)
  counts finished main-loop passes; printed next to the VBL count it shows
  lag frames directly (F030SMW's status `state` field). SMK has an
  equivalent frame counter worth printing the same way.

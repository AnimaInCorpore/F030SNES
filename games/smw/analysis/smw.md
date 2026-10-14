# Translating SMW (tools/smkx on SMWDisX)

Notes specific to *Super Mario World*. The translator's design is in
[translator.md](translator.md) (written for SMK); this file records what
SMW needed on top of it.

## Placement: the Asar walker (2026-09-24)

`tools/smkx/asar.py` follows SMWDisX the way Asar 1.91 assembles it, far
enough to put every line at a ROM offset. It produces no bytes; it handles
`incsrc`, `ORG`, `base`/`base off`, `arch`, `pushpc`/`pullpc`, `skip`,
defines (`=`, `#=`, `?=`, `:=`), `if`/`elseif`/`else`/`endif`, `while`,
`function`, macros with `<arg>` substitution and `?local` labels, anonymous
`+`/`-` labels, `: ` statement separators, `db`/`dw`/`dl`/`dd` and
`incbin` (the checkout carries every binary). `!_VER` is 1 (U.S.).

- Every instruction is decoded from the ROM byte at its offset; the ROM
  mnemonic must equal the source's (`JML`/`JMP`, `JSL`/`JSR` alias).
  Immediates take their length from the `.B`/`.W` hint, or from the
  operand's value when there is none.
- Every label placed is compared with `SMW_U.sym`; a mismatch is an error.
  Where a size is unknowable (SPC700 code under `arch spc700`), the walk
  goes "unsure" and resynchronises at the next label the symbols place.
- Result: 62,465 instructions placed, 9,271 labels checked, no mismatch;
  about 7 s.

Two Asar details that cost a run each:

- Directives are lower case in SMWDisX; `REP #$30` is the instruction,
  `rep n : ...` the directive. Matching directives case-insensitively
  swallowed every `REP` and shifted everything after it.
- A quoted macro argument used inside quotes (`filesize("<file>")` with
  `"x.pal"`) must not end up doubly quoted.

## LoROM

`analyze.rom_index` maps `bank:$8000-$FFFF` to `(bank & $7F) * 32 KiB`;
the low halves are WRAM/I/O (banks `$00-$3F`, `$80-$BF`), a ROM mirror
(`$40-$6F`) or SRAM (`$70-$7D`), all resolved by the runtime
(`rt_build_banks`, `rt_resolve`, `rt_wb`). The vectors are read at file
offset `$7FE0+`. No DSP-1: I/O at `$6000-$7FFF` reads 0.

## SMW-specific program patterns

- **`JSL ExecutePtr` / `JSL ExecutePtrLong`** (65 + 20 sites): the routine
  pulls its return address and jumps through the `dw`/`dl` table that
  follows the call, indexed by A. The analysis treats the call as a table
  jump in the caller's procedure (entries in 8-bit widths, the call's frame
  never pushed); the emitter reproduces what the routine leaves behind
  (`$00-$03` or `$00-$05`, C = the entry, Y restored, carry clear) and
  jumps through a label table. `dw $0000` / `dl 0` entries are unused
  slots. One site builds the call by hand, `PHK : PER $0003 :
  JML ExecutePtr`; it is recognised and its three pushed bytes dropped.
- **OAMResetRoutine** (`$7F8000`): the reset code writes 128 `STA $xxxx`
  into WRAM; the NMI and others `JSL` to it (one enters at `+302`). It is
  not translated; `rt_oamreset` does the stores.
- **Decimal mode**: SMW never sets D (no `SED`, no `SEP` with bit 3), so
  the NMI and IRQ entries are analysed with D clear. With D unknown, 32
  `ADC`/`SBC` nodes in interrupt code could not be translated.
- **`TCD`** once at reset with C = 0; `rt_tcd` checks that it stays 0.
- `CODE_04945D` returns with 16-bit X on one path (reached by `JMP` from
  16-bit code); a caller then enters `CODE_04F708` with X = 16 bits and
  decodes its `LDX #` as three bytes. That is what the hardware would do
  too; the report lists it as a finding.

## APU

The handshake model is F030SMK's, with SMW's request to return to the
loader: `$FF` on port 1 (`StartMusicUpload`), where SMK wrote `$1F` to
port 0. The engine, the samples and each music bank go up this way.

## First runs (2026-09-24)

Emulator: F030Arcade patched Hatari v2.6.1-devel, submodule `639fadf6`,
binary SHA-256 `b3279c6d…1f7d` (the build F030SMK records); Falcon,
TOS 4.04, 14 MB, `--cpu-exact`, `--fast-forward` on. Frame counts are
emulator VBLs and say nothing about Falcon speed.

- Translation: 1,300 procedure contexts over 1,130 entries, 61,911 of the
  62,465 source instructions reached, 0 unsupported nodes, 4 traps (three
  `COP`, one `BRK` on paths the source marks as unused).
- The program boots, uploads the SPC engine, samples and music, and runs
  through game modes 0-7 (Nintendo Presents, title screen, attract demo)
  without a trap for 1,900+ frames.
- The C reference renderer (every 4th frame) is the bottleneck: during
  loading the frame handler overruns the VBL.

## Speed (2026-09-24, same emulator build)

Measured with rendering off (`SMWX_RENDER=0`) and VBL samples
from a given frame on (`SMWX_PROFILE_FROM`); the status line's `state`
field is `GameMode << 8 | TrueFrame`, and TrueFrame counts finished passes
of the main loop, so its rate against VBLs is the game's frame rate.

- **DMA byte by byte.** OAM, CGRAM and stripe-image DMA (fixed-source RLE
  into VRAM, vertical stripes with increment 32) went through the generic
  register handler per byte, and the NMI overran into the next VBLs: in the
  title demo about 57% of VBLs were lost. `rt_dma` now has direct paths
  for VRAM (word pairs or one half, any increment, fixed or incrementing
  source), OAM and CGRAM. Afterwards: no overruns in the demo.
- **CCR saves for V (solved, see "Reference interpreter" below).** 34,344
  of 95,540 nodes saved and restored the CCR only to keep V (58 `BVC`/`BVS`
  read it): escaped returns kept every flag live and each PHP read every
  flag. Precise successors for escaped returns and PHP/PLP pairing cut the
  saves to 636; a first attempt broke the attract demo, but the reference
  interpreter showed the cause was two emitter bugs, not the liveness.
- **Game logic speed.** Rendering off, the title demo (a real level): with
  the old liveness a little more than one 20 ms VBL per game frame, every
  second NMI a lag frame (TrueFrame +25 per 50 VBLs); with the precise
  liveness 44-50 per 50 VBLs, close to full speed at 50 Hz. SNES frames are
  16.7 ms.
- The C reference renderer needs several seconds of Falcon time for a
  full mode-1 frame; it is a checking tool, not the display path.

## Display: 256 colours in eight planes (2026-09-24)

The adaptive 16-colour palette is gone from the display path: SMW frames
show 225-255 distinct CGRAM entries. The Videl runs in eight bitplanes
(SPSHIFT `$0010`, 128 words per line); its palette is CGRAM (frame-start
copy) scaled by INIDISP brightness, prepared by `smkx_render` and copied to
`$FFFF9800` at the start of the next VBL (`rt_clut`), when the new screen
base takes effect. Timings are the 4-plane ones with HDB and HDE 16 units
later (Hatari's videl.c: the fetch offsets are `128/bpp` based, 32 vs 16
units at 4 and 8 bpp, for RGB and VGA alike).

- **Composer** (`src/ppu_fast.c`): per row, each BG layer is decoded once
  into a list of character rows with an opaque pixel (colour bytes via
  spread tables, palette added, opaque mask); layers are painted back to
  front in the mode's priority order (mode 1: BG3L, OBJ0, BG3H unless on
  top, OBJ1, BG2L, BG1L, OBJ2, BG2H, BG1H, OBJ3, BG3H when on top).
  Sprites come from per-frame row buckets (first 32 in OAM order, 34
  slivers fetched last to first, as the reference). Mode 7 rows use the
  reference renderer.
- **c2p** (`ppu_c2p8`): 32 pixels in eight registers, five merge stages
  (8, 1, 16, 2, 4 bit swaps between register pairs 4, 4, 2, 2, 1 apart);
  verified symbolically and by `make test-c2p`.
- **Checks**: the reference renderer with an identity map is the truth;
  `ppu_snap` compares the fast composer with it pixel for pixel and the
  Falcon's planes with `ppu_planar8` byte for byte. Seven snapshots across
  boot, title and attract demo: 0 differences.
- **Tile-row cache**: decoded character rows (eight colour bytes, opaque
  mask; unflipped, palette added on use, h-flip as a byte reversal) per
  VRAM word address, for 2 and 4 bpp, used by BG rows and sprite slivers.
  The runtime marks every written 256-word VRAM page (`rt_vram_dirty`, set
  by the port writes and the DMA paths next to the Mode 7 marks); the
  renderer drops those pages' rows before composing. Ten snapshots from
  boot through a level to the overworld: 0 differences, Falcon vs host.
- **Cost** (emulated, patched Hatari `639fadf6`, title demo, 100 renders):
  0.74 s per frame (c2p 0.15 s, composing 0.59 s). Earlier stages: without
  the cache 0.9 s; bit-by-bit packing plus two-pass layers 1.5 s. Profile
  now: `bg_decode` 52% (tilemap reads, address arithmetic, C register
  spills to the ST-RAM stack), c2p 19%, row set-up 10%.
- **make 3.81 again**: deleting SMWX.TOS from the stamp rule is not enough
  (make had already looked at the file in that run and skipped the link,
  leaving no binary); `make smwx` now updates the stamp and then runs a
  sub-make.

## Reference interpreter: checking the translation frame by frame (2026-09-24)

`tools/ref65816.c` runs the ROM on an interpreted 65816 with the runtime's
hardware model mirrored register for register (memory map, I/O shadows,
general DMA byte by byte, APU handshake, multiply/divide, the IRQ chain,
interrupts as the runtime delivers them: no frame bytes written, the
interrupted state restored whole, `rt_iflag` changed by SEI/CLI and
REP/SEP and restored after each interrupt handler). It runs ~20 million instructions in a few seconds.

To make the two comparable, `SMWX_SYNC=1` makes the runtime skip a VBL
while NMI is enabled and `LagFlag` ($10) is not 0, i.e. an NMI only arrives
when the main loop waits in `GameLoop`; the reference delivers its VBLs at
the same point. Game state after N frames then no longer depends on speed.
`SMWX_SUMS=1` logs a WRAM sum per frame (after the IRQs, before the NMI;
the stack page $0111-$01FF is skipped: the runtime writes no interrupt
frames and no ExecutePtr return addresses there). In synced runs the input
script counts game frames, and the reference takes the same script
(`REF_INPUT`). `make check-ref [SMWX_INPUT=...]` builds, runs and compares;
`scripts/smw_wramdiff.py` diffs a snapshot's WRAM with a reference dump,
`REF_WATCH=addr,...`/`REF_CALLS_FROM=frame` log writes and calls, and
`tools/smkx/traces.txt` prints the 65816 S at chosen addresses on the
Falcon (`SMWX_TRACE_FROM`). Both bugs below were found this way in about
an hour each.

- **Carry lost in flag merges (shared emitter bug).** When an instruction
  produces a live flag while other flags are kept across it (the
  `move.w ccr,d5 ... move.w d5,ccr` merge), the rebuilt CCR took N/Z from
  after the instruction and V from before, but X (the 65816 carry) from
  neither: a carry live across an `LDA` was cleared. First seen at frame
  95: `ADC` / `LDA` / `PHP` ... `PLP` / `ADC` in the title's overworld
  sprites never advanced. Fix: X always comes from after the instruction
  (`emit.py`, `keep = m68(need) | 0x10`).
- **`PHK : PER : JML ExecutePtr` emitted as a plain jump.** `control()`
  handled JMP/JML with absolute operands before the ExecutePtr case, so
  GenerateTile always ran the first handler and left the three pushed
  bytes on the stack; the caller then restored `Layer1YPos` from a return
  address (frame 443, a block bumped in the demo).
- **RTI and the I flag** (F030SMW2's finding): the translated RTI never
  restored the interrupted code's I flag, so after an interrupt handler's
  `SEI` the runtime's `rt_iflag` stayed set and cut the IRQ chain short.
  `rt_frame` now sets `rt_iflag` for the IRQ and NMI handlers and restores
  it afterwards; the reference does the same. SMW's state is unchanged
  (its main loop runs `CLI` every frame): 3,222 demo frames and 4,884
  gameplay frames still match.
- **Result**: 2,961 frames of the attract demo (twice through, back to the
  title) and 4,914 frames along title -> file select -> level with
  scripted input: 0 differences, with the precise liveness as default
  (`SMKX_SAFE_LIVENESS=1` switches it off).

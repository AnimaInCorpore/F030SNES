# Translating Chrono Trigger (tools/smkx on the Diz listing)

What CT needed on top of the shared translator (`analysis/translator.md`,
written for SMK). Emulator runs: F030Arcade's patched Hatari v2.6.1-devel,
submodule `639fadf6`, binary SHA-256 `b3279c6d…` (the build the other ports
record); Falcon, TOS 4.04, 14 MB, `--cpu-exact`, fast-forward on. Frame
counts are emulator VBLs, not Falcon measurements.

## Listing: the DiztinGUIsh adapter (`listing.parse_diz`)

Every placed line carries `;AAAAAA|bytes|target` (code) or `;AAAAAA`
(data); incbin runs carry their range. Placement reads the address from the
comment, decodes the ROM byte there and checks the mnemonic, and the bytes
where the line has them. Bank files `incsrc` further regions (audio, data
tables); those are inlined. Result: 105,374 instructions placed and checked,
4,590 labels, 12,878 symbols (labels plus `Name = $addr` equates).

- 17,170 lines have an address but no bytes: code the trace never ran,
  decoded by Diz with guessed widths, and often wrong (`AND.B #$00 : ASL.W
  $AAEB` at `$FDE21B` is really `AND #$0E00 : XBA : TAX`). They are placed
  but marked untraced; the analysis does not check widths against them.
- A few data lines (`dw PcAnimBlobs+…`) have no address: they continue the
  previous data item.
- The trace's third field is the effective address it saw; for direct-page
  operands that is D + dp, a check for the D tracking below (not used yet).

## Memory

HiROM, 4 MiB (`analyze.rom_index`, `ROM_MASK`), 8 KiB SRAM at
`$20-$3F:$6000-$7FFF` (CT uses bank `$30`). No coprocessor. The ROM's
big-endian word view is gone: for 4 MiB it needed 8 MiB on top of the ROM.
The 68030 reads misaligned words, so 16-bit ROM loads are `move.w` +
`ror.w #8` like WRAM loads. CTX.TOS: 1.3 MB text, 4.5 MB data, 0.8 MB bss.

## Direct page

CT moves D all the time (`$0000`, `$0100`, `$1D00`, `$0300`, `$0A00`, and
`$2100`/`$4300` to reach the PPU and DMA registers through `dp`). D is part
of the analysed state: a value, `IN` (the caller's) or unknown; `PHD`/`PLD`
carry it symbolically, `TCD` takes a constant from `LDA #imm` or
`LDA #hi : XBA : LDA #lo` right before it.

- Known D: `dp` operands become static bank-0 addresses (WRAM, or the
  registers when D is `$2100`/`$4300`).
- Otherwise `(dp,a2)`: a2 = WRAM + D, kept current by every TCD/PLD, and
  as fast as a fixed direct page. Contexts are only specialised on D when it
  points past low WRAM (`DP_IO`); that alone took the analysis from 580,000
  to 140,000 nodes.
- `rt_dreg` holds D in memory too, for the interrupt entry (below).

## Interrupts through WRAM

The ROM vectors `JML $000500` / `$000504`, where each engine stores a
`JML handler` (or an `RTI` to disable it). The analysis treats that JML as
a dispatch to the handlers in `tools/smkx/interrupt_targets.txt` (found
from the installers' `LDX #/STX $0501` pairs); `rt_ramjml` reads WRAM and
dispatches, an `RTI` there returns at once.

Before the translated NMI/IRQ runs on the interrupted registers,
`rt_entry_regs` rebuilds X/Y (zero upper words), D (a2) and the data bank
(d3, a3, a4) from `rt_dreg`/`rt_dbr`: the VBL can interrupt `rt_dma`, which
uses a2-a4 as scratch. (Shared bug, see the porting notes.)

## Widths and escapes

- `PLP` of a P byte pushed elsewhere leaves the widths unknown. CT only uses
  decimal mode between a `SED` and a `CLD`, so such a PLP clears D.
- Routines that leave through their caller's frame (the audio driver's
  `Audio_ServiceExit`, the intro program's task handlers) pull the caller's
  P. Their widths are resolved where the escape lands (`escape_into`,
  `pesc`), and where code in the routine itself needs them the emitted code
  branches on the pulled byte (`rt_lastp`, `width_split`). A general
  four-way split at every unknown PLP multiplied the program to 2.2 million
  nodes; this one costs nothing measurable.
- Pointer-dispatch sites continue in every exit context their handlers are
  known to return in (`open_exit`), with bank and D from the runtime.

## Pointer dispatch targets: per site

Seeding every dispatch site with all address-taken labels of its bank (the
SMK method) gave 580,000 nodes in CT: 12,878 symbols, many of them code
addresses used as data. Dispatch sites now use only the targets listed for
them in `tools/smkx/dispatch_targets.txt` (`$SITE: $T1 $T2 …`), and the
runtime reports every miss with its site. `scripts/ctx_seed_loop.py` runs,
appends the reported target to its site, rebuilds and runs again.

Jump tables the listing keeps as incbin bytes are read from the ROM up to
the next label, while each word names listed code (`rom_table`).

## Code in WRAM

- **The intro program** ("WRAM program"): C30002 decompresses 34,175 bytes
  (the block at `$C3:[$FE0003]`) to `$7E3000` and `JML`s there. It is
  translated at its WRAM addresses from bytes `tools/smkx/ct_lz.py`
  rebuilds (a Python port of `Gfx_DecompressEntry`, checked byte for byte
  against WRAM in a snapshot); every entry from outside checks eight bytes
  of WRAM (`rt_ovlcheck`). It runs task lists (`JSR ($0940,X)`,
  `JSR ($0950,X)`, `JSR ($00F0,X)`) whose handlers come from script records
  (`F8`, `FA`, `F9`, `FC`, `F0` followed by an address) and from
  `LDX #handler : JSR $7E395E/$7E3974`; the lists in `dispatch_targets.txt`
  come from those, filtered by "decodes cleanly to its first return".
  Seeding every record target of one kind blew the analysis up to 2.6
  million nodes (most are data).
- Other calls into WRAM run through `rt_ramcode`, a small interpreter for
  the opcodes seen there so far.

## The menu's coroutines

The menu engine (bank C2) runs its screens as cooperative tasks, each on
its own 65816 stack (`MenuTask_*`, `$0A00-$0BFF`, 64 bytes per task): a task
yields through `MenuTask_SleepFrames` (saves X, Y, P, D, then S), and the
scheduler resumes another with `TCS … PLD PLP PLY PLX RTS`. The 68k return
stack cannot follow that. `rt_setsp` (TCS/TXS) gives each stack region its
own 68k stack: a region keeps the a7 it had when S left it, and S set to a
region's top starts its 68k stack afresh (which also drops the frames of
code that resets the stack, `LDX #$06FF : TXS`). The VBL runs on its own
stack. In the analysis a call to `MenuTask_Sleep`/`MenuTask_SleepFrames`
is a yield (`YIELD_ROUTINES`): the task continues right after it with its
widths and D (the resume path restores P and D), and the emitted
continuation accepts any exit id. The resume path's PLY/PLX take their
widths from the pulled P (`width_split`). Tasks start with `JMP ($0000)` at
`$C28203`; its targets are the 14 entries passed to
`MenuTask_SetSleepFrames` (`LDA #entry : JSR/JMP $C28249`). Not yet
exercised at runtime.

## After the title: the menu shell and its dispatch sites

Start on the title ends the intro program (`JML $C00000`); the field
engine sees a location ≥ `$1F0` and jumps to `Menu_ShellEntryVector`
(`$C20000`), the menu shell that shows the file screen. Its code
dispatches a lot through pointers; per site:

- `$C2828E` (`PER $C2826B : JMP ($0000)`, the task state machine): a call
  that returns to the PER target + 1 (`analyze`: `ret_at`, `percall`);
  handlers from `MenuTask_StateTablePointers` (`$C28292`) and each menu's
  state table.
- `$C2EFD0`: `LDA.L table,X : STA $8A : PER : JMP ($008A)`, found by
  `pointer_idiom_sites` (text control codes).
- `$C20553` (`JMP ($0050)`, window slots `$0B30-$1B2F`): handlers passed to
  `Menu_FindFreeSlot`/`…Pri` in X, and the ones window scripts set.
- `$C21819`/`$C21836` (`JMP ($005B)` / `JML [$005B]`): native calls from UI
  scripts, op `$34` + word / op `$4E` + long (`MenuUiOpTable`, `$C20F91`);
  scanned from the ROM, kept where the target is traced code.
- The menu NMI `$C2840E` (and an RTI-only IRQ `$C28472`) is copied into the
  trampoline; it switches to a stack at `$1DFD` and back
  (`TSX : STX $1DFE : LDX #$1DFD : TXS … LDX $1DFE : TXS`), which the
  analysis now follows (`State.outer`). It saves X/Y in the interrupted
  code's widths; handlers entered with unknown widths push and pull them as
  16-bit.
- `$7E9690` is an `MVN : RTL` stub the menu and save code build in WRAM,
  inside the intro program's old range: calls into that range from outside
  compare WRAM with the translated bytes and run the interpreter when they
  differ (`rt_ovlmatch`). The intro program itself ends at its `CODE END`
  marker; its Mode 7 copy routine is generated right after it and runs in
  the interpreter.
- `rt_setsp` runs with interrupts masked: the menu NMI executes TXS too, and
  a VBL inside the main code's `rt_setsp` overwrote its saved return
  address (a crash at a different frame in every run).

## Sound: the SPC700 runs the game's driver

The echo model of the other ports is not enough for CT. Its driver uses
port 0 as a counter (so the SMK rule "`$1F` returns to the boot ROM" broke
the upload), and the intro waits on port 3 for the driver's beat
(`$7E5AAA`: `LDA $2143 : AND #$0F : BEQ`), which the driver writes with
`MOVW $F6,YA` at SPC `$02D4`.

- `src/spc700.c`: an SPC700 interpreter with timers, ports and the S-DSP
  register file (no audio). The boot ROM is not included; its upload
  protocol is modelled (`spc_cpu_write`), so the driver arrives exactly as
  `Audio_UploadDriver` sends it. `make test-spc` uploads CT's driver on the
  host, runs it and sends a command.
- Idle skipping: a port read twice with nothing else read in between ends
  the run slice; a timer counter read as 0 twice waits for its next step.
  An idle driver costs about 3,500 SPC700 instructions per second.
- Clock: the SPC700 advances with the 65816's work, not with the Falcon's
  clock: 4 cycles per port access (one 65816 polling iteration on the SNES)
  and 1/60 s per frame. A clock-driven catch-up fell further behind with
  every poll (a busy driver interprets several times slower than real
  time), and the 65816's 1024-poll timeouts stretched to minutes.
- Cost: while the driver is busy (sample uploads, song start) up to ~24,000
  SPC700 instructions per frame; the VBL handler overruns then. The plan
  (ROADMAP): the driver statically translated, the S-DSP on the Falcon DSP.

## Rendering on the DSP (2026-09-24, late night)

The DSP composes the tile modes (0, and 1-6 as the reference does them:
as mode 1) with sprites and priorities: `falcon/dsp/ppu.asm`, fed by
`src/smkx_ppu.c`. Mode 7 scenes (the intro) still use the Mode 7 kernel
with four planes; the runtime boots whichever program a scene needs. The
68030 only plans the frame; the DSP composes and sends it over the SSI into
DMA record while the game runs on.

- **Output**: eight planes, and a pixel is its CGRAM index: the Videl's
  256 colours are CGRAM as the frame started (brightness applied), so no
  palette mapping and no histogram. 57,344 bytes per frame.
- **Planner** (`src/ppu_dsp_plan.c`, CPU): `ppu_replay` (the reference's
  own HDMA/IRQ-log/OAM-segment replay, now exported) gives each row's
  registers; the stream (`src/ppu_dsp.h`) holds the registers at row 0,
  every change with its row, and per OAM segment the sprites that can show
  (in OAM order). It also lists the VRAM pages the frame reads: each
  layer's map row once per frame (whatever the scroll) and its characters,
  each sprite's character rows (a row of 16 characters is one page). On
  the 68030 per-row work is expensive (224 rows x ~250 instructions was
  20 ms): the replay flags rows where a register or the OAM segment
  changed (`PpuRegs.changed`), and only those are looked at again.
- **VRAM on the DSP**: 77 pages of 256 words (P:$1300-$3FFF, P:$6000-$7FFF),
  least recently used out; the runtime marks every VRAM page written
  (`rt_m7_dirty`, now all 128 pages), and a needed dirty page is uploaded
  again. Frames in the tested scenes read 22-29 pages; uploads are rare.
- **Composition** (the host model `tools/dsp_model.c` describes it):
  per row and layer 33 slivers of 8 pixels, a byte per output plane (the
  tile's planes, the palette base's bits as 00/FF) and the opaque mask by
  priority. A 16-pixel group of a plane is three sliver bytes shifted by
  the fine scroll, which the multiplier does in A0 in three `mpy`/`mac`
  (`b0 << (16+f) + b1 << (8+f) + b2 << f`). Sprites are laid into plane
  words (the lowest OAM index ends on top). Each group is then composed
  front to back: a pass takes the pixels no earlier pass covered; passes
  without pixels on the row are left out. Descriptors per 8-row map block
  (the character row's address, the step for vertical flip, a handler by
  bpp/flip/priority) are stepped from row to row.
- **Transport**: a 512-word ring; the SSI transmit interrupt sends from it
  and marks what it sent (bit 0: plane words have bits 7-0 clear), so it
  stops at the writer by itself. The first word of a frame goes out
  polled (Hatari raises the interrupt only once record took a word, as
  scummvm's `hsc2p` notes).
- **Booting without XBIOS** (`falcon/smkx_dspboot.s`): bit 4 of the PSG's
  port A resets the DSP; its bootstrap then takes 512 words into
  P:$0000-$01FF; `ppu.asm`'s stage-2 loader takes the rest (blocks: space,
  address, count, words). Supervisor mode, safe in the VBL handler; the
  runtime switches programs per scene this way.
- **Checks**: `make check-dsp-model` (host: plan + model against
  `ppu_render` + `ppu_planar8` on every snapshot); `make test-ppu` (the DSP
  program in Hatari on six real frames: field, menu, title with 89 and 100
  sprites, then `PPUTEST_RANDOM` random frames from `tools/dsp_rand.c`:
  modes 0/1, BG3 on top, 16x16 tiles, map sizes, fine scroll, register
  writes mid-frame, 120+ sprites of all sizes with flips at the edges);
  SNAP.BIN version 4 holds the planes' count and eight-plane frames, and
  `make check-snaps` compares them with the reference. 6 real + 40 random
  frames exact; every snapshot of a 6,000-frame run (intro, title,
  attract) exact.
- **What the random frames found**: the reference's scroll latches
  (`bgold`, `m7old`) carried over from the previous replay, so a
  single-byte scroll write mid-frame depended on the render before
  (`ppu_replay` clears them now); the sprite code clobbered the sprite
  list's pointer through the bit-reverse table for horizontally flipped
  sprites; mode 0's 12 compose passes overran the pass list into the
  table pointers.
- **Measured** (patched Hatari `639fadf6`, emulated, not a Falcon): DSP
  171 ms per field frame, 164 menu, 197-205 title (C reference: ~5.2 s);
  of the field frame the slivers take ~54 ms, compose ~41, sprites ~15;
  the SSI transport alone 76 ms (754 KB/s, four tracks). CPU planning
  8.6 ms per frame outside the title, 47 ms on the title (100 sprites, the
  logo's layers). The title's game code itself takes ~80 ms per frame
  (the translated NMI), which now dominates there.

Pitfalls met on the way (more in the porting notes): AND works on A1 only
and leaves A2's sign (the ring's fill looked negative, the interrupt
overran the writer); bit instructions only take short (X:$00-$3F) or I/O
absolute addresses; local labels need a letter after the underscore;
`m7t_hex` clobbered d2 (C expects it saved), which garbled test output.

## Game speed: 50 frames per second without rendering (2026-09-25)

Emulated in the patched Hatari (`v2.6.1-497-g639fadf6`, binary built
2026-09-22 21:37), not measured on a Falcon. Sound deferred
(`CTX_APU_FRAME=0`), rendering off (`CTX_RENDER=0`), release
build (`CTX_DIAG=0`). Two runs: Start every 200 frames into the menu, and
idle through the intro, the title and the attract demo (field scenes, a
battle). `scripts/ctx_fps.py` reads the rates from the log; the timing
marks now cover `Field_MainLoop` too (`tools/smkx/marks.txt`).

| scene | before | after | VBL handler per frame, after |
|---|---:|---:|---:|
| intro (Mode 7) | 12.5 fps | 50 fps | 1-10 ms |
| title | 10 fps | 49.8 fps | 15 ms |
| menu (Battle Mode, name entry) | 25 fps | 49.7 fps | 3.3 ms (main loop 6.0 ms) |
| field, NMIs | 36 fps | 50 fps | 4-5 ms |
| field, `Field_MainLoop` passes | ~25/s | ~46/s | 11.6 ms each |

Found with F030SMK's timer-C sampler (`rt_tc_sample`, taken over;
`scripts/smkx_profile.py --timer --context vbl|main`), in order of effect:

- **The frame-start copy** copied the whole 24 KiB register shadow every
  VBL: 28-35% of the VBL handler. Now only what the renderer reads (from
  F030SMK).
- **DMA byte by byte**: OAM and CGRAM uploads went through `rt_io_write`
  per byte (46% of the menu's VBL time). `rt_dma` now has F030SMK's direct
  paths (VRAM any increment, OAM, CGRAM, WRAM, fixed sources), plus a pair
  loop for CGRAM while no IRQ logs the writes. Menu NMI 16 -> 3 ms.
- **B -> A DMA from the multiplier**: the title fills its OAM buffer with
  `$E0` by setting M7A = 1, M7B = `$E0` and DMAing 512 bytes from `$2134`
  (`$7E3843`), per byte through `rt_io_read` and `rt_wb`. `rt_dma .mpydma`
  reads the product bytes once and fills (long words when they are one
  byte).
- **Page tables** for the generic memory helpers (`rt_rb/rw/wb/ww`, from
  F030SMK): one lookup per 256-byte page instead of the bank rules per
  access. `rt_rb` reads WMDATA (`$2180`) inline: the title's sprite builder
  `$C30D0E` reads its lists through it with an unknown data bank.
- **Release build** (`CTX_DIAG=0`): no call-site note per generic access;
  menu main loop 10.4 -> 6.1 ms. The status line's "from" and the profile's
  attribution to 65816 procedures need `CTX_DIAG=1` (the default).

The field's NMIs keep up now; its main loop is the limit: about 27% of it
is `Field_WaitVblank`'s spin (idle), ~15% the sound driver (the SPC700
runs whenever the game polls the ports, `$C70xxx`), the rest game code and
generic memory helpers. Event frames (loading, decompression) take longer
than one frame, hence ~46 passes per second on average in the attract
demo.

Two hangs came to light once the attract demo ran further:

- **RDNMI race**: the location change at `$C03B71` waits with `LDA $4210 :
  BPL` while NMIs are on, but the field NMI reads `$4210` first thing and
  clears the flag, so the poll never saw it (the demo stood for minutes).
  On the SNES the poll wins: the flag is set at V-blank before the NMI is
  taken. The runtime sets the flag again after the NMI returns.
- **Latched V counter**: `BattleSys_WaitScanline` (`$CD3B82`) latches
  through `$2137` and waits until `$213D` reaches a line; `$213D` returned
  its shadow, so the frame handler hung in the attract demo's battle. Each
  latch is now one scanline later (wrapping at 262), and `$213D` reads
  through the low/high flip-flop that `$213F` resets.

Checked with rendering on (release build, every frame on the DSP
renderer, a snapshot every 250th frame): 24 of 24 snapshots over 6,000
idle frames (intro, title, Zeal) and 32 of 32 over 8,000 frames into the
menu match the host render byte for byte; the title's sprites and the
menu look right. The attract demo's dim, sprite-only stretch after the
title (INIDISP `$05`, TM `$10`, ~1,950 frames, black) was there before
these changes too.

The profile script also named samples after absolute equates (`SRAM_SIZE`,
`SMKX_FRAMES`) that fall inside the runtime's addresses; it skips them
now. Rendering (the DSP renderer, ~165-205 ms per frame in Hatari) is the
limit for the displayed rate; `analysis/dsp-renderer-budget.md` sets its
next steps.

## Game speed with rendering on (2026-09-25, afternoon)

With the DSP renderer on, the 68030's share of rendering runs in the VBL
handler before the NMI, so it cost game frames. Emulated (patched Hatari
`639fadf6`), release build, every frame rendered; `scripts/ctx_fps.py`
now also prints DSP frames, VBLs between them and the planning, upload
and send time per DSP frame.

| scene | before | after | planning per DSP frame |
|---|---:|---:|---:|
| intro (Mode 7) | 6.2 fps | 8.8 fps | (Mode 7 path) |
| title | 38.8 fps | 45 fps | 48 -> 10 ms |
| menu | ~41 fps | 48-50 fps | 31 -> 11 ms |
| dim sprites-only scene | 41.7 fps | 50 fps | 20 -> 9 ms |
| field (Zeal) | 40.5 fps | 46.6-48.5 fps | 65 -> 15 ms |

- **Planner** (`src/ppu_dsp_plan.c`): a map row's character pages were
  marked per entry (`need()` 2-5 times per entry, Zeal touches nearly
  every map row of three layers). Now each 32-entry map block's page bits
  are cached with the block; the runtime's `rt_m7_dirty` (written since
  upload, cleared after planning) says which blocks to drop, so clean
  blocks are trusted without a compare (`DspPlan.vdirty`; the host tools
  pass none and compare). Bits collect per layer and become page marks
  once (`flush_chr`). Rows between two changes are one span
  (`ppu_replay_changes`, `close_span`).
- **Replay** (`src/snes_ppu.c`): F030SMK's HDMA (tables through host
  pointers, `ppu_aptr_hirom` with CT's 4 MiB mask; channels that write
  nothing the renderer reads are not walked). Then: channels are stepped
  from event to event (a non-repeat entry holds its count without a walk
  per line: CT's field tables transfer twice per frame and held the loop
  on all 225 lines); a unit equal to the channel's last one is skipped
  when repeating it changes nothing (plain registers, whole write-twice
  pairs; off when any channel writes a write-twice register halfway, and
  after IRQ writes); `changed` means the state a row sees changed, not
  that a register was written.
- **Mode 7 frames**: a frame starting in Mode 7 went to ppu.asm first
  (boot, plan, fail) and then back to the Mode 7 kernel (boot again): two
  DSP program loads per frame. It now goes to the kernel straight away
  (forced blank counts as not Mode 7: the intro loads in forced blank).
  `compose_m7_ext` fills rows without sprites directly instead of a
  per-pixel loop. The intro still costs ~370 ms per `render4` (C
  composition around the kernel, the palette twice): Mode 7 in ppu.asm
  (ROADMAP) is the real fix.

Checked: 56 of 56 snapshots (6,000 idle frames, 8,000 into the menu)
match the host render byte for byte; the model agrees on the 40 tile
frames among them; 900 random frames pass `dsp_rand` (planner pages
included). What is left: the title's NMI (15 ms) plus planning (10 ms)
loses a VBL per DSP frame; the field's main loop (11 ms) leaves little
room; planning could move out of the VBL handler.

## Rendering in the game's idle time (2026-09-25, afternoon)

The render work (planning and sending a frame to the DSP renderer, or the
Mode 7 path's composition) no longer runs in the VBL handler. The VBL
only asks for it and shows finished frames; the work runs in a context of
its own (`rt_render_thread`, stack `rt_rstack`, `falcon/smkx_rt.s`):

- **Where**: at the game's idle points (`idle` marks in
  `tools/smkx/marks.txt`: `Field_WaitVblank`'s spin `$C0EC71`, the menu
  shell's frame-flag wait `$C281CA`, the intro program's loop `$7E3023`)
  `rt_mark_idle` saves the game and starts or resumes the work. A VBL over
  the render context runs its frame (the NMI gets the game's a6, the
  65816 S) and returns to the game; the TOS VBL chain is called with a
  format-0 frame (`rt_chain_tos`). A scene without an idle mark gets the
  work from the VBL after `RWAIT` (3) frames, still preempted by the next
  VBL.
- **Inputs**: while work is pending, frames skip the frame-start copy, the
  IRQ log and OAM segments, so the work sees the frame it was asked for.
  VRAM is live: the planner has its own dirty flags (`rt_plan_dirty`),
  since the upload clears `rt_m7_dirty` and VRAM may now change between
  planning and upload.
- **Presentation**: the paths record how a frame is shown (planes,
  palette); `smkx_render_present` applies it at the flip (`rt_flip`), so
  the Videl never switches mode under the frame still on screen.
- **Snapshots and the frame limit** wait until no work is pending (a
  snapshot then carries a later frame number, e.g. `F000167D`).

Emulated (patched Hatari `639fadf6`), release build, rendering on:

| scene | game fps before | now | pictures per second now |
|---|---:|---:|---:|
| intro (Mode 7) | 8.8 | 50 | 1.6 |
| title | 45 | 47.2 | 3.4 |
| field (Zeal) | 46.6-48.5 | 49.5-50 | 3.3 |
| menu | 48-50 | 49.8 | ~4.7 |

The game gets its frames; the picture gets what the game leaves. In the
intro most of the rest goes to the SPC700 (~38% of the main context: the
intro waits on the music's beat), so the Mode 7 path shows 1.6 pictures a
second. The title's NMI alone takes 15 ms. Snapshots: 23 of 23 idle, 31
of 31 into the menu match the host render.

**Planning while the DSP draws** (same day): `smkx_ppu_start` is split
into `smkx_ppu_plan` (68030 only) and `smkx_ppu_send` (uploads, record,
stream). The VBL's poll (`smkx_render_poll`, every rendered frame) asks
for the next frame while the DSP still draws, timed from running averages
of the DSP's time and the plan's time (in VBLs), so the plan ends about
when the DSP is done and the frame is no older than before. The work
plans, waits for the DSP (`smkx_render_waiting`: the VBL's fallback does
not hand the CPU to a context that only waits; the poll clears it when
the frame is in, or scenes without an idle mark deadlocked), plans again
if the game wrote a VRAM page the plan reads, and sends into `rt_back` as
it is then. Pictures per second: title 3.4 -> 4.0, menu 4.7 -> 5.0, field
3.3 -> 3.6 (Zeal) and 5.0 -> 5.5, the dim sprites-only scene 7.2 -> 9.1;
game rates unchanged (title 47.5, field 49-50, menu 49.8). In Zeal a
picture still takes ~14 VBLs against the DSP's ~9: planning and page
uploads only get the game's idle time. Snapshots 24 of 24 and 32 of 32.

Next for the picture rate (done the same evening, below): Mode 7 in ppu.asm (the Mode 7 path's composition around the kernel is ~370 ms
of 68030 time per picture). A second `start` symbol (a static C function)
had shifted the profile's attribution: `scripts/smkx_pc.py` keeps the
first one now.

## Mode 7 on the DSP renderer (2026-09-25, evening)

A second program from the same source: `falcon/dsp/ppu.asm` assembled with
`M7K=1` (`tools/smkx/dsp_asm.py --define M7K=1`, `build/ctx/ppu7.bin`)
takes frames whose visible rows are all Mode 7. The runtime loads the
program a frame needs (`smkx_ppu_boot(kernel)`); CT's Mode 7 scenes are
whole screens, so a switch happens at scene changes only.

- **Memory**: the Mode 7 plane (VRAM words 0-$3FFF) flat at P:$1300-$52FF,
  uploaded by the CPU packed as `(tile * 64) << 8 | texel`, the format of
  `m7c2p.asm`; sprite pages raw in 12 slots at P:$7400-$7FFF; stream, OBJCA,
  layer 0's strip, a line buffer, the c2p tables and the coordinate tables
  (TVM/TUM/TVF/TUF, built at assembly time with `DUP`) in X:$1300-$32FF.
  The tile-only code (slivers, descriptors, character tables) is left out;
  sprites, compose, the ring and the loader are the same code.
- **Stream**: after the OAM segments, one entry per row: the line cut into
  segments by `m7_plan` (INSIDE, TILE0, FILL), as the Mode 7 kernel takes
  them (`src/ppu_dsp.h`). `dsp_plan` sets `p->kernel`; a frame mixing Mode
  7 and tile rows fails and goes to the CPU; a frame with no visible row
  stays with the loaded program (`p->prefer`).
- **A row**: the kernel's sampling loop (packed coordinates, one 48-bit add
  per pixel, four table reads, two P reads) into a line buffer, then c2p
  into layer 0's strip in an 8bpp sliver format (planes 0-7, MLO): per
  sliver `mac` over tables whose bits sit at 22, 13 and 5, scaled by
  2^(8-i), so three planes land in a1[7:0], a0[22:15], a0[14:7] (bit 23 of
  a0 stays clear: a0 is multiplied as a signed value to shift it). Compose
  runs a Mode 7 pass list (OBJ 3, 2, 1, BG1, OBJ 0) with eight planes.
- **Sprite pages inside the plane**: a name table can wrap past $8000 into
  pages 0-63; those pages get a raw copy in the sprite slots after the
  packed one, and the page table points to the last upload
  (`DspPlan.sprneed`). Random frames found it (planes 0-3 wrong, 4-7 right:
  the bytes were permuted, the masks not).
- **Checks**: host model and planner against the reference on 300 random
  Mode 7 frames per seed (wrap, clip to transparent, tile 0, flips,
  matrix writes mid-frame, sprites); PPUTEST 73 of 73 frames exact (the
  intro's three Mode 7 frames, 16 random Mode 7 frames with ~120
  sprites); game snapshots 24 of 24 and 32 of 32.
- **Speed** (emulated): the intro's Mode 7 frames take the DSP 134-238 ms
  (tile frames 164-205); 4.6 pictures a second in the intro instead of 1.6
  (the four-plane path), game at 50 fps, the picture in CGRAM's own colours.

Pitfalls met: the assembler flags a register loaded one instruction before
it is used as an address ("Contents of register written in previous
instruction"); `t_%.o` did not depend on `src/ppu_dsp.h`, so a changed
constant did not reach PPUTEST; clearing `rt_m7_dirty` after all uploads
instead of before each lost writes the NMI made during the upload in the
render context (a stale page, 67 words in the last snapshot).

## The latched scanline and the page cache (2026-09-25, late evening)

- **The V counter model starved the field engine's scripts.** The morning's
  model (each SLHV latch one scanline later) counted on across frames.
  `Field_ReadHVLatch` (`$C05A46`) runs object scripts one slice at a time
  while the latched line is below a budget (`$0169`): whenever the counter
  stood above it, no script ran. The cathedral's three characters never
  reached OAM (0 sprites on screen; the snapshots matched the reference,
  so the renderer was not at fault), the credits text stayed black, the
  Dragon Tank did not fight. Now the counter starts at line 0 each frame
  (`rt_frame`): one line per script slice, every queued script each frame,
  and waits for a line still end.
- **The cathedral needed 80 VRAM pages** (three full layers plus sprites)
  against the tile program's 77 slots: the frame went to the C renderer,
  one picture in ~14 s of idle time. The per-layer character tables
  (CADDR, 4K words) are gone: `desc` finds a character's row through the
  page table (a dozen instructions per descriptor, once per 8-row block),
  and the 4K words are 16 more slots at P:$4700-$56FF: 93
  (`DSP_TILE_SLOTS`, `dsp_tile_slot_p`). Random "wide" frames (any
  character) in `dsp_rand` need up to 91 pages; PPUTEST 57 of 57 exact.
- The status line's renderer counters now end with the last refused
  frame: pages << 16 | stream words, program << 16 | sprite pages.

## Jump tables cut short: the attract battle's stop (2026-09-25, night)

- **What stopped.** The attract demo's battle ended in a "bad exit": a
  `JSR (abs,X)` jumped past its translated table into whatever the next
  table's code offsets named. The Hatari CPU history led to
  `jsr ([t3111_6,d1.l*2])` at `$CD16AF` (BattleScene_DispatchCmd, scene
  script commands `$80-$F9`) with X beyond a table of 12 entries: the
  table has 122, but its handlers lie in bytes the listing kept as
  incbin, so `analyze.rom_table` stopped at the first handler it could not
  see as code.
- **A second way to cut a table short**: the listing puts labels inside
  tables (`PointerTo05AttackRandomP_B8C5` in the enemy AI's target-selector
  table), and both readers stop at a label. `$C1B8BB` was read as 5 of 57
  entries, `$C25842` (text modes) as 2 of 4, `$CFF01E` as 8 of 16.
- **Fix**: `tools/smkx/tables.txt` gives such tables their length
  (`analyze.TABLE_LENGTHS`, used by both readers; the Makefile now
  retranslates when it changes). `scripts/table_audit.py` lists every
  table read shorter than either the run of ROM words naming listed code
  or the entries up to its lowest handler (for handlers left as incbin),
  and dumps a table with its handler names. Most suspects run on into the
  next table; each checked one is in tables.txt with its reason, so the
  audit comes out empty. The second test found `$CD2A51`
  (Dispatch_BattleSceneB, 2 of 29, where the guard below had stopped the
  demo), `$C223DB` (the menu states, 4 of 10) and `$C2E298` (name entry
  cursor, 3 of 4).
- **A guard at every translated table**: the translator emits
  `cmpi.l #2*n,d1; bcc.l <table>_oob` before each `jsr/jmp ([table,d1.l*2])`;
  the stub pushes the 65816 site and stops in `rt_tableoob` with X and the
  site ("jump table index past the translated table"). A short table now
  stops where it is used instead of running into foreign code. The bad
  exit report also dumps the 68k stack (as text offsets) and the 65816
  stack.
- **Then an untranslated handler** (`$C279E0`, frame `$2E7C`): the UI
  script's native call (op `$34` + 16-bit address, `JMP ($005B)` at
  `$C21819`) from the scripts in bank `$C3` (`$C310C0` on), which the
  earlier scan of op `$34` records had not covered. All 27 such targets
  there (listed code right after an RTS/RTL/JMP/BRA; most are the battle
  windows at `$C275xx-$C27Bxx`) are in `tools/smkx/dispatch_targets.txt`;
  the 24-bit form (op `$4E`) there names only `$C6E04D`, already listed.
- **Result**: the attract demo runs its 14,000 frames without a stop
  (no rendering, Hatari v2.6.1-497-g639fadf6, emulated time). The battle
  is slow: `$1F0E-$283C` runs at 39 fps (NMI 10.6 ms, main loop 5.3
  passes/s at 81 ms), `$2EAE-$300C` at 18 fps (NMI 43.7 ms per frame).
  The battle engine's NMI and main loop are the next speed work.

## Sprites torn in idle-time pictures: a render copy of VRAM (2026-09-25, night)

- **Seen in realtime play**: sprites sometimes drawn with only some of
  their parts. The snapshots never showed it: they compose at once, in the
  VBL, with no work pending.
- **Cause**: a picture's registers, CGRAM and OAM are copied at the frame
  start and kept while its work runs, but VRAM was read live. Planning
  takes ~40 ms (two VBLs or more) and the upload comes after it, and the
  game's NMI runs in each of those VBLs: CT streams its characters'
  animation tiles into VRAM every frame. A picture then drew OAM of frame
  N with sprite tiles of frame N+k (the stale check only replanned
  against the newer VRAM, and the upload read live VRAM too).
- **Fix**: the renderer reads a copy, `rt_vram_r` (64 KiB). VRAM writes
  mark their page in `rt_vr_dirty`; `rt_vram_sync` copies the marked
  pages where a picture's inputs are fixed (asking for the work in
  `rt_render_poll`, and before a sync render when none is pending) and
  marks them for the uploads and the planner (`rt_m7_dirty`,
  `rt_plan_dirty`). The work then sees one frame's VRAM. The copy costs
  the pages written since the last picture, 512 bytes each, in the VBL.
  Files: `falcon/smkx_rt.s`, `src/smkx_ppu.c`, `src/smkx_m7.c`,
  `src/smkx_video.c`. Snapshots: 56 of 56 exact over 14,000 frames.

## The battle at 50 fps (2026-09-25, night)

Emulated in the patched Hatari (v2.6.1-497-g639fadf6), release builds,
sound deferred (`CTX_APU_FRAME=0`); not Falcon measurements. The attract
demo's Flea battle ran at 18-40 fps while effects played: single NMIs
took 20-40 ms and swallowed the next VBL. Now every stretch of it runs at
50 fps, with rendering off and on (rendering every frame: 49.6-50 fps,
2.6-4 pictures per second in the battle).

Found with `scripts/smkx_profile.py --timer --context vbl` over short
sample rings (`SAMPLES` lowered for the run) and, for the runtime's time,
a temporary caller note in `rt_iorb/iowb/iorw/ioww` and `rt_rb/rw/wb/ww`
(`move.l (sp),rt_io_caller`); `--pairs N` now lists (runtime routine,
calling procedure). A temporary per-NMI time log (NMIs over 15 ms, with
the frame) showed the losses came from runs of long NMIs, not the
average. The diagnostic build (`CTX_DIAG=1`) triples runtime-heavy NMIs:
its rates are not the game's.

- **The WRAM port trick at `$217F`** (BattleFx_QuadWindowEmit*, `$D1FA29`
  on): a 16-bit STA/LDA at `$217F` pairs APU port 3 (the mirror) with
  WMDATA, up to four per scanline for a window shape. Each port access
  ran the SPC700 for 4 cycles. Now the port byte passes without SPC700
  time (it is a side effect, not a poll: `smkx_apu_in/out`, port 3), and
  WMDATA goes straight to WRAM: in `rt_iorw/rt_ioww`, in `rt_rw/rt_ww`
  for system banks, and through `rt_rw217f/rt_ww217f`, which the
  translator emits for a word access at absolute `$217F` with an unknown
  data bank.
- **BattleSys_WaitScanline** (`$CD3B82`) polls the latched V counter up to
  the HUD split (`$D3`): with one line per latch some 1,000 runtime calls
  per wait. A mark at its latch (`$CD3B9A vwait`, `rt_mark_vwait`) moves
  the counter to the line before the target; the loop ends at its first
  pass, in the state it would have ended in.
- **MVN/MVP** copied byte by byte through `rt_rb/rt_wb`; now runs up to the
  next page boundary of either side are copied directly where both pages
  are mapped (same order, so overlapping fills stay fills).
- **The multiplier and divider inline**: the translator loads
  `$4214-$4217` from the register shadow (`rt_io`), stores `$4202/4/5`
  there, and calls `rt_wrmpyb/rt_wrmpyw/rt_wrdivb` for the writes that
  start an operation; through an unknown data bank, `rt_d{r,w}{b,w}42xx`
  test the bank and fall back to the generic path. PalAnim_ScaleRGB
  (`$FDE64F`, fades and flashes: three multiplies per colour) caused the
  longest NMIs.
- **VRAM DMA**: the usual upload (word increment 1, incrementing source,
  no wrap) is a long-word copy instead of a loop per word.
- **Idle mark** for the battle's wait (`$C10084`, BattleSys_PumpFrames): the
  render work runs there, not by taking the CPU from the game after
  three frames.

Snapshots: 56 of 56 exact over 14,000 frames. Still below 50: the 1999 AD
overworld scene after the forest battle (menu engine sprites, 24-32 fps)
and a field stretch at `$12F2-$1518` (46-47 fps).

## The DSP renderer's input by DMA playback (SSI) (2026-09-25, night)

F030SMK's route taken over (its porting note "SSI input in the
renderer"): DMA playback -> crossbar -> the DSP's SSI receive, handshake
mode (the DSP asks for every slot with PC4), beside the record route that
carries the picture out.

- **DSP** (`falcon/dsp/ppu.asm`, both programs): PC7 (SRD) to the SSI, PC4
  a GPIO output; `get` reads from the SSI while CRB's RE bit is set
  (`get_x`: a 24-bit word as two slots, bits 23-8 then 7-0; `getw`: a
  16-bit word as one slot). New commands: SSION (3, host port, answers 1),
  SSIOFF (9, over the SSI: the play window's last slots), PAGE24 ($21: the
  Mode 7 plane's packed words, two slots each). PAGE and FRAME keep their
  inline host-port loops and read through `getw`/`get` over the SSI.
- **Runtime** (`falcon/smkx_m7dsp.s`): Devconnect(DMA play -> DSP receive,
  handshake, 25.175 MHz, prescale 1), Dsptristate(receive on);
  `rt_ssi_play`/`rt_ssi_play_done`; `rt_m7_record` keeps a play window's
  bits in $FF8901. `rt_dsp_boot` waits for a play window to end first (the
  DSP may still be reading it).
- **Driver** (`src/smkx_ppu.c`): a frame that uploads VRAM pages is built
  in one buffer (the pages, the stream, zero padding to whole DMA frames,
  SSIOFF), SSION goes over the host port, record is armed and the buffer
  played; the 68030 writes no page word. A frame without pages goes over
  the host port as before. `SMKX_PPU_SSI`: 1 the tile program (default),
  2 the Mode 7 program too, 0 never. The status line's ninth renderer
  field counts the frames sent by SSI.

**Why per frame.** The crossbar delivered 8,192 slots in 74.6 ms here
(~110,000 a second; Hatari 639fadf6, emulated). A page then costs the DSP
about as long as over the host port (~2.4 ms, PPUTEST: ~2.5 ms), and the
68030 saves its writes; a stream word costs two slots (~18 us) against
1-2 us over the host port. Sending every frame by SSI lost pictures where
the DSP is the bottleneck (the Mode 7 intro); by SSI only when pages go
up, tile scenes gained (pictures a second, host -> SSI: the field 3.2 ->
4.0, 2.5 -> 3.4 and 2.1 -> 2.9 in later scenes; the battle 3.7 -> 3.6),
the whole run 5.06 -> 5.09. The intro's stretch at `$28A-$7D0` swings
between 6.3 and 8.0 pictures a second with any change of timing (also
with SSI off, and with only `nop`s added): which game frame is sampled
decides when the Mode 7 program is loaded. PPUTEST's draw times are the
same with the old and new program. Checks: snapshots 56 of 56 exact over
14,000 frames (303 of 1,597 DSP frames by SSI), PPUTEST 57 of 57 (host
port).

## What the DSP waits for (2026-09-27)

F030SMK's 2026-09-26 findings (the 68030's 256-byte instruction cache;
DSP shifts as one multiply) were checked here first. A timer profile of
a release run (14,000 frames, rendering every frame; patched Hatari
639fadf6, emulated) puts the 68030's render work at about 8% of the
samples (`close_span`, `replay`, `dsp_plan`, `upload_to`), 4.4% in
`compose_frame`'s wait for the DSP and 9.4% idle with no picture asked
for: the pictures are limited by the DSP, not by the planner, so the
split frame loop would free CPU time but add no pictures. The SSI input's
`rep #8` shifts wait on the slots anyway (110,000 a second, ~140
instruction cycles each).

Where the DSP's time goes: the 200 Hz timer tick sorts it into states
(`rt_dsp_ticks`, printed as "SMKX dsp ticks" at the frame limit; the
render context's phase is `smkx_render_phase`, `smkx_inflight` the
picture in flight). Over 289 emulated seconds:

| state | share |
|---|---|
| drawing (record running) | 67.0% |
| done, not yet seen by the VBL's poll | 5.7% |
| idle: the next picture planned, waiting for the game to idle | 12.0% |
| idle: planning / sending / the Mode 7 path, the game has the CPU | 10.5% |
| idle: the same with the render context running, and the rest | 4.8% |

The largest gap: `wait_dsp` spins until the VBL sees the picture done,
but the VBL then returns to the game, and the render context gets the
CPU back only at the game's next idle mark; only then does the planned
picture go out. Handing the CPU to a waiting render context right after
the VBL (a send is ~6 ms of 68030 time) should recover most of the 12%.
Not done yet.

## Main-screen windows (2026-09-27)

No renderer drew windows (F030SMK's census entry). A census over 200
demo snapshots (every 64th frame; `tools/win_census.c`, which replays each
frame with every register walked, `ppu_walk_all`) found main-screen
masking (TMW with a window select) in 16: wipes that hide BG1-3 and the
sprites left of an edge HDMA moves every line (window 2, 11-19 edge sets
a frame, up to a full cover), a BG3 reveal cut by two inverted windows
ANDed (61 edge sets, next to a split screen), and windows of one pixel
or none. Clip to black (CGWSEL bits 7-6) is never used; colour math is
on in 57 snapshots (a separate gap).

- **Reference** (`src/snes_ppu.c`, from F030SMK): `ppu_win_spans` /
  `ppu_win_layers` (at most three hidden spans per layer and row; W1/W2,
  inversion, OR/AND/XOR/XNOR, TMW), masking in `compose_row` and
  `compose_m7_ext`; `windows_possible` makes `reg_used` walk HDMA
  channels on window registers only when windows can hide a layer.
  `tools/win_test.c`: the spans against a per-pixel evaluation, 200,000
  random register sets, no mismatch.
- **A third DSP program** (`falcon/dsp/ppu.asm` with `WIN=1`,
  `build/ctx/ppuw.bin`): the planner (`src/ppu_dsp_plan.c`) counts the rows
  windows cut and picks `DSP_KERNEL_WIN` for such tile frames (a Mode 7
  frame with windows goes to the CPU). Its stream has four more slots
  (`DSP_SLOT_WSEL`..`DSP_SLOT_WLOG`, the window registers packed); the
  other programs' streams are unchanged, and their binaries are byte for
  byte what they were. On a window slot change `win_build` makes per
  layer and 16-pixel group a keep mask (each window's range as group
  masks from edge tables, inverted, combined, complemented); compose ANDs
  a BG pass's mask with it (a sixth pass descriptor word), and the
  sprites' priority masks `OM` are ANDed after `obj_row`. It gives the
  last two VRAM page slots (P:$5500-$56FF) to its tables: 91 slots.
- **Host model** (`tools/dsp_model.c`, `win_keep`), `tools/dsp_rand.c`
  (every fourth frame random windows, with window writes mid-frame),
  PPUTEST (five real window screens, 32 random frames).

Checks (patched Hatari 639fadf6, emulated): the model equals the
reference on 400 random frames (69 with windows) and on all 200 census
snapshots; PPUTEST 46 of 46 frames exact, the window frames on the new
program. Its cost on the five real window screens, against the plain tile
program: 0-7% (164.6 -> 167.9, 303.4 -> 302.7, 251.2 -> 235.7, 221.8 ->
236.8, 168.7 -> 178.5 ms); frames without windows keep the plain program
and pay nothing, apart from a program switch (a few ms) where a scene
starts or ends its windows.

## Colour math and the sub screen (2026-09-27)

`tools/ppu_census.c` counts per snapshot what the renderers left out
(modes 2-6, colour math with the fixed colour or the sub screen, mosaic,
CGRAM or brightness changed mid-frame, SETINI, direct colour). Over 200
demo snapshots: colour math with the fixed colour in 34, with the sub
screen in 25, Mode 2 in 20; none of the rest.

**The SNES's picture.** `ppu_render_rgb` in `src/snes_ppu.c` draws a frame
as the SNES shows it: main and sub screen with their windows (TMW, TSW),
the colour window, clip to black, CGWSEL/CGADSUB (add, subtract, half, the
fixed colour; OBJ only with palettes 4-7; no halving where the fixed
colour stands in for a transparent sub pixel or the pixel was clipped),
brightness. The fixed colour: COLDATA writes each component alone, so the
runtime keeps the three at `$21F0-$21F2` (`rt_io_write`'s `.coldata`, a
register address nothing uses; the frame start copies them), and
`PpuRegs.fixed` follows IRQ and HDMA writes. `tools/ppu_rgb.c` writes a
snapshot as Falcon | SNES side by side (with `PPU_RGB_REF=1` the left
half is the reference's picture instead of the stored one).

**Fixed colour: exact in the palette.** When every visible row applies
the fixed colour the same way (CGWSEL: math always, nothing clipped, the
fixed colour or an empty sub screen) and the CGRAM entries it changes
are drawn by no shown layer it leaves alone (mode 0's BGs in 32-entry
blocks; modes 1-6 BG1/BG2 1-127, BG3 1-31; Mode 7 BG1 1-255; sprites
128-191 never, 192-255 by CGADSUB bit 4; entry 0 the backdrop), the
Falcon palette gets those entries changed (`dsp_plan`: `math_row`,
`DspPlan.mathpal`; `dsp_math_palette`; `smkx_ppu_palette` on the
Falcon). The planner walks HDMA on CGWSEL, CGADSUB, COLDATA, TS and the
windows only when colour math can change a pixel (`math_possible`). The
1999 AD Day of Lavos map (subtract on BG1-4) is now dark red-brown as on
the SNES: 0 pixels differ, before ~55,500. A per-line gradient (the battle
menu windows: COLDATA per line) cannot be one palette and stays flat.

**Sub screen: an approximation.** Per row `ppu_sub_cfg` says whether and
how (tile-mode rows that add the sub screen with colour math always on,
nothing clipped, and a shown layer or the backdrop taking part). Such a
row shows the sub screen's pixel on a checkerboard ((x + row) even) where
the main pixel's layer takes part and the sub pixel is opaque; where it
adds without halving, only sub pixels of CGRAM blocks bright enough to
show (average luminance at least 8 of 31), taken from lightened copies
`min(31, 2c + K/2)` (K the average main colour) that the planner puts
into blocks nothing in the frame draws (map palettes of the shown layers'
rows, sprite palettes; `relocate`, `DspPlan.subreloc`,
`dsp_sub_palette`); a black backdrop under an opaque sub pixel shows it
exactly (backdrop + sub = sub). Dim blocks add little on the SNES, so the
main pixel stays: the Blackbird's dark cloud layer would otherwise haze
the ship, the Zeal palace's golden light shows. Rows that change nothing
(no block relocated, no black backdrop; or no shown layer taking part)
are switched off. The reference (`compose_sub_row`, with
`PpuInput.subreloc`), the host model and the DSP do the same; the CPU
path has no relocation (those rows keep the main pixel).

**On the DSP** the window build (`WIN`) takes these rows too: a fifth
slot (`DSP_SLOT_SUB`), the relocations after the OAM segments; it draws
TM|TS, composes the sub screen first (`fx_compose`, `SUBMODE`: groups
into SUBBUF, SCOV), then the main screen with each pass's colour math
flag (bit 23 of the keep word, OBJ in its third word) gathering MMV,
and merges at the group's output (`_mrg`, `fx_reloc`: the relocated
blocks' pixels by their planes 4-7). Its routines moved to P:$5300-$54FF
(the program would have reached the sprite arrays at P:$0C00): the build
keeps 89 VRAM page slots. The tile and Mode 7 programs are byte for byte
unchanged. Cost: a second compose, +100-170 ms a picture on rows with the
sub screen (PPUTEST, emulated). Mode 7 rows (the intro's copyright text,
the title's clock face: sprites on the sub screen) are not done yet.

Checks (patched Hatari 639fadf6): the model equals the reference on 400
random frames (50 with the sub screen, up to 8 blocks relocated) and 185
snapshots; PPUTEST 49 of 49 (the palace, the Blackbird, a halving frame,
random sub-screen frames); the demo with snapshots every 64 frames, all
equal to the reference (200 of 200; 54 pictures with sub-screen rows, 29
with the colour math palette).

## snes9x as the reference (2026-09-27)

The references above are our own C. `tools/s9x_ref.c` runs snes9x 1.63's
libretro core headless (fetched and built by the Makefile under
`build/snes9x`, untracked; the installed Snes9x.app cannot be driven
frame by frame): the attract demo without input, work RAM cleared at
power-on as the port has it. It pairs each snapshot with the game frame
whose work RAM matches (a cheap test on every 32nd byte, then in full;
the port's frame count runs ahead of the game's, twice as fast in the
field engine), then takes the frame within 120 either side closest to our
reference's picture. `scripts/s9x_compare.py` runs it all and reports.

Over 185 demo snapshots: all paired (median 151 of 131,072 work RAM bytes
differ, the rest sound-driver and timing state); 84 pictures of nearly
one colour (between scenes) left out, since they match any black frame;
of 101, our reference equals snes9x within 1% of the pixels in 60. Where
the states line up the reference is exact, colour math included: the
Blackbird and the palace glow, 0 pixels; the Zeal wipe (window 2 over
everything), 0 pixels 56 frames after the work RAM match. The large
differences left are timing: the closest frame lies more than 8 frames
from the work RAM match in 53 of 101; battle backgrounds scroll ahead,
the battle menu appears later, effects at other moments. The port's
NMI-driven animations (fades, wipes, scrolling backgrounds) run out of
step with its slower main loop, while the SNES runs them in lockstep.

## A whole attract cycle: the NMI in step with the main loop (2026-09-27)

On snes9x the attract mode is a cycle of ~11,000 game frames: the
pendulum at ~600, the title, Zeal, the Blackbird, battles, 1999 AD,
Magus's castle, Ozzie's battle, the planet, Zombor, black, the pendulum
again at ~11,400; the second cycle adds the credits and other scenes.
The port had never got through it: every earlier check stopped at 12,800-
14,000 port frames, which was game frame ~5,900.

- **Stuck at Magus's castle gate** (game frame 5,900; port frames 12,800
  to 17,887 without the game moving on, then a crash). The work RAM
  against snes9x's: the field objects' state had drifted apart. The port
  ran the game's NMI every VBL although its translated main loop needs
  more than one; NMI-driven counters and scrolls ran ahead of the main
  loop's object logic, and the event waiting for the party never came
  true. **`SMKX_SYNC`** (falcon/smkx_rt.s, on by default): a VBL runs the
  game's IRQs and NMI only once the main loop waited since the last one
  (an idle mark, `rt_mark_idle`, or a V-blank poll of `$4210`), as the
  SNES does for CT; otherwise a lag VBL (the renderer goes on). After 8
  lag VBLs it comes anyway (`rt_forced`, the status line after the NMI
  count): 5 of 25,683 game frames over the cycle. The game's pace barely
  changed (77% of the VBLs carry a game frame, 71% before).
- **`$7E36E8`**: the `$7E3000` overlay addresses the PPU registers
  through the direct page (D = `$2100`) with an index; the emitter now
  computes the address at run time (`dp-io`, generic helpers). 12 fewer
  unsupported nodes.
- **A jump to `$000000`**: self-modifying code in the same overlay: a
  shared helper's `JSR $0000` at `$7E35FC` gets its operand stored before
  it runs (`LDA #$45B0 : STA $35FD` or `LDA #$9B46 : STA $35FD`).
  `analyze.PATCHED_CALLS` lists such sites and their targets; the emitter
  compares the stored bytes and calls the matching translation (another
  value stops with rt_trap).
- `report.txt` lists every unsupported node with its address now.

Result (patched Hatari 639fadf6, emulated): 32,000 port frames without a
stop, to game frame 18,989 in the second cycle, where the picture equals
snes9x's exactly at the work-RAM match (offset 0). With a snapshot every
256 frames: 123 of 123 equal to the reference; against snes9x 44 of 77
(those not of nearly one colour) within 1%. Left: the snapshot builds
still copy a snapshot's work RAM some frames after its registers (the
pairing's offsets); CGRAM rewritten per line by HDMA (Frog Squash's
colour gradient: the Falcon has one palette per picture); an attack
beam in colour math that the Falcon leaves out.

## True colour on the DSP? Measured: no (2026-09-27)

The question: would a chunky true-colour (RGB565) DSP renderer be faster
than the planar kernel, since it needs no bitplanes, and exact in colour?
Emulated in the patched Hatari 639fadf6.

- **The planar kernel's time** per picture on six real frames (field /
  menu / title), by its `PROF` switches: whole 173 / 165 / 198-206 ms;
  output only 76.5 ms; BG layers ~98 / ~89 / ~83 (slivers ~53,
  descriptors ~45); compose ~41 / ~59 / ~41; sprites ~5 / ~11 / ~64-72.
  The output overlaps the drawing: compute-bound.
- **Output**: the SSI is at the 25.175 MHz ceiling (57,344 bytes in
  76 ms, 755 of 786 KB/s; the ScummVM DSP c2p project's figures). A
  256x224 true-colour picture, 114,688 bytes, needs ~146 ms of output
  alone; 32 MHz would give ~115 ms, but Hatari's handshake shifts data by
  2 bits there and the specification keeps that clock for the codec.
- **The chunky core, measured** (`falcon/dsp/tcbench.asm`,
  `src/tcbench.c`, `make test-tcbench`): three BG layers (4, 4, 2 bits),
  33 tile rows each per row, 224 rows; per tile row the plane bytes, four
  L: table lookups giving all eight pixels as 6-bit fields, each field by
  the multiplier into its key table (depth and palette, 0 transparent) and
  a max into the row's line; then RGB565 per pixel through the ring and the
  SSI. **220 ms computing, 258 ms with the output** (unrolled; 242 / 287
  with a loop), without sprites, windows or colour math. The planar kernel
  draws whole pictures, sprites included, in 165-206 ms. With parallel
  moves (a progressive shift: `mpy x0,y0,b` by 2^5 leaves the pixel in B1
  and the rest shifted up in B0, one constant and no mask; the line in Y
  loaded in parallel with the multiply; the rest copied back in the N
  register's gap): **188 / 233 ms**. Further scheduling (pixel pairs
  against the address-register gaps, no line clear, a tighter output
  loop) might reach ~145 ms for this core; with sprites, windows, colour
  math and the output (45 ms here) a whole renderer would land at about
  the planar kernel's speed, not above it.

So chunky true colour is slower on this DSP: SNES tiles are planar, and
the 56001 does a bitplane operation on 16 pixels at once where chunky work
costs ~20 cycles a pixel and layer (~8 with parallel moves, plus the
per-tile work). The planar kernel stays.

Open, not started: (1) the planar kernel's BG stage (~98 ms a picture;
~45 of it descriptor work) is the largest cost and speeds up every scene;
(2) a true-colour Mode 7 program, where chunky skips the c2p and may win;
(3) a true-colour program only for colour-math scenes, exact but slower
there. Exact colour
remains possible as a separate program for the frames that need it
(colour math, per-line CGRAM), at a slower picture rate there, or through
the palette and checkerboard approximations as now.

Snow Bros (F030Arcade, tested on a real Falcon) has the 256x224
true-colour Videl set, should it be needed: RGB 50 Hz `$8282`
`$00C70076`, `$8286` `$003F001E`, `$828A` `$007600AB`, vertical as CT's,
`$82C0` `$0185` (25.175 MHz), `$82C2` 0, `$8266` `$0100`, `$8210`
`$0100`; VGA `$00FC00A9`, `$002502F3`, `$00A800C0`, `$82C0` `$0182`,
`$82C2` 5. A hand-derived timing looked right in Hatari but mirrored the
right edge on a real monitor.

## The decompressor natively; how far preparing data goes (2026-09-27)

Measured in the patched Hatari (v2.6.1-497-g639fadf6), emulated, not on a
Falcon.

**Where the 68030's time went.** Hatari's own CPU profiler over the release
build (`CTX_DIAG=0`, rendering every frame) from boot to VBL 14,100 (game
frame ~13,100; 281.6 emulated seconds). `CTX_PROFILE=1` builds crash at
game frame 473, right after the first `$C30002` call (vector 4, a PC in
WRAM), with or without `CTX_DIAG`; the release build runs, so the profile
came from Hatari. The recipe: `--parse init.ini` with `profile on` and
`b VBL = 14100 :once :quiet :file save.ini`, where `save.ini` holds
`profile save hprof.txt` and `quit`. A `profile save` sent over
`--cmd-fifo` while the emulation runs writes only the header, because
Hatari processes profile data when it enters the debugger. Keep the paths
relative: the debugger cuts long lines. Rows map to 65816 procedures
through `CTX.map` and `scripts/smkx_pc.py`.

- Gfx_DecompressEntry (`$C30557-$C308B2`, called from about 50 places):
  11.99 s, 4.3%, inclusive. Its own translated code took 4.40 s. The rest
  was runtime helpers called once per byte: `rt_rb` 3.62 s, `rt_mvn` 1.58 s,
  `rt_iowb` 1.23 s (one WMDATA write per output byte), `rt_setdbr`,
  `rt_rw` and `rt_ioww`.
- Larger shares, for comparison: `spc_run` 11.0%, the generic memory
  helpers `rt_rb`/`wb`/`rw`/`ww` about 19%, and `rt_io_write`/`read` 9.6%.

**The native routine** (`falcon/ct_lz.s`) follows the listing step for step
and cites it:
- the four variants (11- or 12-bit offsets; MVN into `$7E` or `$7F`);
- the bit counter `$0B`, which a zero flag byte (eight literals) leaves
  alone;
- the carry of X + 9 that goes into Y (`$C305D5`: REP #$21 clears it only
  once);
- the WMDATA port address (`rt_wmadd`, WMADDL/WMADDH written as the routine
  writes them);
- the direct page `$0306-$0316` as it is left, and the ten bytes the pushes
  leave below S (the MVN's PHB byte too).

It declines, and the translation runs, for a source bank whose pages are
not one linear host block (`lz_check`, cached per bank), or output that
starts in `$7E` at or below `$0316`, where it could overwrite its own
direct page.

The translator hooks it in (`tools/smkx/natives.txt`: entry, RTL, name;
`emit.native_hook`): at `$C30557` every context pushes a stub doing its own
RTL (the exit context of the routine's RTL node with the caller's widths)
and the M/X/D bits PHP pushes, then calls `rt_native_lz`. All 8 contexts
are hooked. `CTX_LZ=0` switches the native routine off.

**Checked** (`CTX_LZ_CHECK=1`): each call runs the native routine, keeps
its WRAM and port address, restores the state before and lets the
translation run. A mark at the RTL (`$C308B2 lzchk`) then compares all
128 KiB and the port address. Over the whole attract cycle (32,000 frames),
comparing about 340 calls:

| Result | Calls |
|---|---|
| Identical | 311 |
| Differ only in the pushed P's C bit | 28 |
| Interrupted, not comparable | 9 |

- **The C bit.** The translation keeps C dead there: its PHD at `$C30558`
  takes the X flag from its own `SUB.L`, and PLP's C is not read after the
  RTL. So it pushes whatever the host flag held, where the native code
  pushes the caller's C.
- **Interrupted calls.** An NMI or IRQ ran during the translated pass.
- **A pitfall of the check.** Interrupts must be masked during the native
  pass. The intro program's 34 KB decompression once took 9 VBLs in the
  check build, and the NMIs that ran in it looked like differences in game
  counters.

**Speed**, the same profile set-up with `CTX_LZ` = 0 / 1:

| | Calls | Total | Per call | Share |
|---|---|---|---|---|
| Translated | 168 | 11.99 s | 71 ms | 4.3% |
| Native | 187 | 1.37 s | 7.3 ms | 0.5% |

In the same VBL window the native build ran the decompressor 187 times
instead of 168. The time comes back in scene loads, the "event frames"
below 50 fps. Pictures per second do not change: the DSP limits those.

**How far preparing data goes on the renderer.** PPUTEST's 17 real frames,
DSP ms per picture, by the kernel's `PROF` switches:

| Frames | As is | `PROF=8` (no slivers) | `PROF=7` (output only) |
|---|---|---|---|
| Field | 173 | 120 | 76.5 |
| Menu | 165 | 103 | 76.5 |
| Title | 198-206 | 157-165 | 76.5 |
| Window frames | 240-382 | 190-308 | ~76.6 |

- Tile decode for free is the ceiling of any tile preparation. A prepared
  form cannot reach it, since expanded tiles do not fit the DSP's 77-93
  page slots.
- The 8-plane output, 76.5 ms, stays as the floor.
- `PROF=16` (no `desc_build`) now crashes the DSP (an illegal instruction
  at P:$0071: the handler pointers are never set), so the descriptor cost
  (about 45 ms on an earlier kernel) was not measured again.
- A chunky variant of `tcbench.asm` with tile rows prepared as 6-bit fields
  took 160 / 205 ms (computing / with output), against 188 / 233 ms as is.
  With a ready key per pixel, which would not fit in DSP memory, it took
  113 / 161 ms.

## BG modes 2-6; offset-per-tile on the DSP (2026-09-27, evening)

Everything drew BG modes 2-6 as mode 1: 4, 4 and 2 bits per pixel, mode
1's priorities, no offset-per-tile. The census (`tools/ppu_census.c`)
over 188 demo snapshots found mode 2 in 20 of them (the title, frames
$A83-$F40) and none of modes 3-6.

**Reference** (`src/snes_ppu.c`), as snes9x 1.63 draws it (`gfx.cpp`,
RenderScreen and DrawBackgroundOffset):

- **Layer formats** (`bg_layers`, `mode_bpp`): mode 2 BG1 4 + BG2 4,
  mode 3 8 + 4, mode 4 8 + 2, mode 5 4 + 2, mode 6 4; BG3 and BG4 only in
  modes 0 and 1.
- **Priorities, front to back**, modes 2-6: OBJ 3, BG1 high, OBJ 2, BG2
  high, OBJ 1, BG1 low, OBJ 0, BG2 low (snes9x's depths 16/15/12/11/8/7/4/3;
  `compose_row` and `front`). Mode 1's order puts BG1 high and BG2 high
  together.
- **Offset-per-tile** (`opt_word`, modes 2, 4, 6): BG3's map, scrolled by
  BG3's offsets, holds a word per tile column; bit 13 applies it to BG1,
  bit 14 to BG2.
  - Modes 2 and 6: a horizontal word from BG3's scroll row and a vertical
    one from 8 lines below it.
  - Mode 4: one word, vertical when bit 15 is set.
  - A horizontal word replaces the scroll's coarse part (the fine part
    stays the register's); a vertical word replaces the whole vertical
    scroll.
  - The leftmost column is never offset.
- Modes 5 and 6 are hi-res on the SNES; here they are drawn at 256 pixels
  with their layer formats. Nothing in CT uses them. Direct colour is not
  modelled (the census found none).

Checked against snes9x (`scripts/s9x_compare.py`) on the 20 mode 2
snapshots: 0 pixels differ in every one, where the old reference differed
in 800-2,700 pixels. Most of that was the "TRIGGER" line, which BG1 and
BG2 both draw: mode 1's order gave it to BG2, so the colour math CGADSUB
$21 applies to BG1 was left out.

**DSP renderer:**

- **Planner** (`src/ppu_dsp_plan.c`):
  - BG modes 3-6 go to the CPU.
  - Mode 2 draws BG1 and BG2.
  - `ppu_opt_slivers` (snes_ppu.c) gives each layer's 33 slivers their
    scroll words: H on << 23 | V on << 22 | V << 10 | H. The frame sends one
    table (66 words after a count, at the end of both tile programs'
    streams, `ppu_dsp.h`); a frame whose table changes between rows goes
    to the CPU.
  - The map rows the columns' own vertical scroll reaches are marked as
    needed.
- **Host model** (`tools/dsp_model.c`): the table in `strip_build`, the
  new pass order, two layers in mode 2.
- **Kernel** (`falcon/dsp/ppu.asm`):
  - the stream's table (`OPTP`);
  - mode 2 rows draw BG1 and BG2 only;
  - pass list `TPL2` (Y:$F0 in the tile programs);
  - `desc_opt`: a mode 2 row's descriptors, sliver by sliver with its
    own scroll. `desc_build`'s map-row part is `desc_row` now.

  A row after one with the same key (configuration, the registers' first
  column and vertical scroll, the previous row: `DKY` = row | $400000)
  steps the slivers that stay in their 8-row block and builds only the
  others.

**Checks** (patched Hatari 639fadf6, emulated):

- The model equals the reference on all 188 snapshots.
- PPUTEST: 64 of 64 frames exact (the 20 mode 2 snapshots, 12 others of
  modes 0, 1 and 7, 32 random).
- A rebuilt demo run with a snapshot every 64 frames: 199 of 199 Falcon
  pictures equal to the reference.
- Before the change, 17 of the 20 mode 2 snapshots differed from the
  corrected reference, by 495-1,129 plane words.

Against snes9x over the 199 snapshots, 74 of nearly one colour left out,
of 125:

| | Equal to snes9x (within 1% of the pixels) |
|---|---|
| Reference | 86 |
| Falcon | 76 |

The rest is timing (61 pairings more than 8 frames apart) and the
Falcon's colour-math approximations.

**Cost.** Title pictures take 267-282 ms on the DSP:

| Version | ms per title picture |
|---|---|
| Old mode 1 approximation (not exact) | 198-206 |
| Every descriptor rebuilt every row | 453-468 |
| Map-row addresses reused across slivers | 395 |
| Stepping (current) | 267-282 |

The remaining cost is the per-sliver loop on every mode 2 row, 33
slivers for each of two layers.

The Falcon still lacks the title's colour math: BG1 (math on) and BG2
(math off) share CGRAM entries, so the planner's palette cannot hold it
(`mathpal` off). That needs a per-pixel colour path.

## True colour with pre-converted tiles: every mode, exact (2026-09-27, night)

The earlier measurement ("True colour on the DSP? Measured: no") left
open a true-colour program with tiles prepared ahead: on the Falcon the
ROM is in RAM, so a page can be converted once, when it goes to the DSP,
into a form the renderer reads without bitplane work. Built and checked;
emulated in the patched Hatari 639fadf6.

- **Reference.** `ppu_render_565` (`src/snes_ppu.c`): RGB565 from the
  shared reference, brightness `c * (b + 1) >> 4`, green `g << 1 | g >> 4`.
- **Converted formats** (`src/ppu_tc.h`, `tc_convert`; the DSP converts in
  TPAGE): F2 (3-bit fields, a clear row is a zero word), F4 (5-bit fields,
  flags CLEAR / SOLID), F8 (9-bit fields, two a word), RAW (maps), M7 (the
  Mode 7 plane, each word `P(t) << 8 | texel`: a map entry gives its
  character's DSP address). Fields keep a zero top bit: the 56001's
  multiply is signed. Measured on the snapshots: 52% of tile rows are
  clear, 45% solid, so most rows cost one flag test or eight straight
  writes.
- **The program** (`falcon/dsp/tc.asm`): per pixel a key
  `depth << 16 | source << 8 | index` (snes9x's depths, the greater key in
  front); each layer into LK, then into MAIN and SUB through the window
  masks; colour per pixel. Two builds: modes 0-6, and `--define TC7=1`
  for frames whose rows are all Mode 7 (the plane in slots 0-63; map row
  and column by one multiply each, `(tv & 7) * 8` and `tu & 7` by table).
  TCTEST boots the one each frame needs.
- **Colour paths per row**: no math: T565, one lookup; fixed-colour math
  independent of the colour window: CTAB, one lookup; sub-screen math
  independent of the colour window: `sub_check`'s tables, S = CTAB + ES0
  and three channel lookups (about 21 instructions a pixel); otherwise the
  general `pixel` routine.
- **The host model** (`tools/tc_model.c`) is the program's executable
  description, and it is exact: `make check-tc` runs 199 snapshots and 400
  random frames of every mode (Mode 7 included) against the reference.
- **On the DSP**: `make test-tc` (TCTEST) runs 27 real and 24 random
  frames. All 51 are exact, 7 of them Mode 7.

| Frame | ms per picture (emulated) |
|---|---|
| Simple field frames | 154 (output alone ~146) |
| Title (mode 2, offset-per-tile, colour math) | 309 (first version 1,331) |
| Other real frames | 165-545 |
| Mode 7 with sub-screen math | 267-390 (general path: 922-1,044) |
| Mode 7, no sub screen | 228 |
| Random frames (windows, math, every mode) | 400-1,470 |

What made the difference, from profiles (`dspprofile` at a breakpoint):
sliver descriptors stepped from row to row, sprites decoded once per OAM
segment, one-screen layers without windows drawn straight into their
line, CTAB, and the sub-screen tables; on Mode 7 frames the general colour
routine had taken 68% before them, the sampling only 58 ms.

**In the game: `CTX_TC=1`.**
- `src/smkx_tc.c` gives smkx_ppu.h's interface for the true-colour programs. Its page cache is keyed by page and conversion; TC7 keeps the plane in slots 0-63. Uploads go by SSI playback when a frame sends pages.
- The Videl is in 16-bit true colour throughout, with Snow Bros' timing. The screens are 114,688 bytes.
- The CPU draws a frame the DSP cannot take with `ppu_render_565`.
- Snapshots carry the RGB565 frame (planes 16, `ppu_snap_frame`), and `tc_snap` compares it with the reference.
- Run from a copy (`build/tcrun/`), so its F*.BIN snapshots don't overwrite the planar ones the tests use.

Over the first 3,000 frames (intro, Mode 7, the title):
- No trap, and no frame fell back to the CPU.
- All 13 snapshots are exact, including the title's colour math, which the palette only approximated.
- Pictures: 242 in 3,283 VBLs (3.7 a second), against the planar build's 361 in 3,170 (5.7). Most of that stretch is Mode 7, where the planar Mode 7 program is faster.

Then, over 12,000 frames of the attract cycle:
- **Frames too big for the slots.** From frame 7,750 no picture came:
  a field frame needed 78 page slots (the program has 74), and the CPU's
  `ppu_render_565`, in the time the game leaves it, took about a minute
  per frame. Across all snapshots, 2 of 211 frames need more than 74 slots
  and a dozen need 71-74. Such a frame now goes in bands (at most 4): the
  DSP's BAND command draws and sends only rows [first, last) while walking
  every row (slot changes, sprite segments, Mode 7 entries), the planner
  marks pages only for the band's rows (`DspPlan.band0/1`), and the bands
  fill one record window in order. Checked with `tc_snap -bands 2` (at most
  71 slots a band, exact, nothing read outside a band's plan) and in
  TCTEST, where every third frame is drawn a second time in two bands.
- **The 68030's planning.** A CPU profile (Hatari `profile`, saved from a
  VBL breakpoint) put 506,000 cycles per call in `need_span_tc`: every
  visible map cell went through need_cell's calls and a multiply. It now
  takes each map row once, with the shown columns as a bit mask and the
  pages computed by shifts. HDMA that writes a register every line made
  every row a span of its own, so each layer's cells are now marked per
  run of rows with the same fields. `PpuRegs.cgwrites` spares the CGRAM
  compare on rows without writes, and `math_row` (the planar palette's)
  is skipped for true colour. Planning went from 184 to 109 ms per
  picture; the planar planner takes 73.
- **Mode 7** writes its keys straight into MAIN or SUB when BG1 is on one
  screen and has no window there: 29 ms less per frame.

| Frames | true colour, pictures/s | planar |
|---|---|---|
| 0-3,000 (intro, Mode 7) | 3.1-4.3 | 4.1-6.7 |
| 3,000-5,000 (title) | 2.3-3.6 | 3.1-7.1 |
| 6,000-12,000 (field scenes) | 1.5-2.5 | 2.8-4.4 |
| the whole run | 2.74 | 4.29 |

The field scenes are now DSP-bound (300-550 ms a picture; the output
alone ~146).

**Toward 2x** (the plan: `tc-speedup-advice.md`; the check:
`tc-speedup-review.md`):
- **Internal program RAM.** The patched Hatari (`dsp_cpu.c`) charges 2
  cycles when an instruction touches more than one external space among
  its fetch, X and Y. The hot loops therefore run from internal P over the
  finished stage-2 loader: `hotcode`/`extcode` macros, loaded at P:$1000
  (the ring later) and copied in by `init`, which itself runs once from
  P:$1200.
- **Solid F4 rows** skip the transparency test. On a line that holds only
  the backdrop so far, they store two instructions a pixel.
- **The merge's window path** walks runs of one window code (WSEG),
  not pixels.

The review measured 1.12x on the real test frames: field 1.20x, Mode 7
1.12x, the title 1.07x. The best field frame went 545.7 -> 360.8 ms
(1.51x). Everything stayed exact.

What is left:
- In field frames, per-sliver bookkeeping costs about 100 ms a picture
  (`step_row`, `sliver`, `bg_line`, the format dispatch). That calls for
  specialised layer-row loops.
- Rows whose colour math depends on the colour window still call
  `pixel` for every pixel.
- Two-screen layers still go through LK and a merge.

## True colour in the game: the Videl, mixed frames, the frog scene (2026-09-28)

**Videl.** The first live test showed RGB565 as bitplanes on TOS's white
palette. After setting `$8266` to true colour, the setup cleared the
horizontal scroll with `clr.l $ffff8264.w`, which also clears `$8266`.
Now it clears the word only and follows Snow Bros' order: `$82C0`, then
`$8266` (clear, then $0100), then `$82C2`, `$8210`, `$820E`. Palette
entry 0, the border in true colour, is set black.

**Frames the DSP could not take.** The planner now reports why
(`DspPlan.fail`), and snapshot builds write out the first such frame and
the first mixed ones. Over a 40,000-frame attract run:
- **Mode 7 and tile rows in one frame (code 6).** The frog scene: sky in
  mode 0 over a Mode 7 ground, BGMODE by HDMA. The CPU drew the whole
  frame with `ppu_render_565`, Mode 7 included, about 30 s emulated. Such
  a frame now goes in runs of one kind of row (`dsp_tc_runs`, from
  `DspPlan.rowkind`), each drawn by its program with BAND. The driver
  reloads the other program between runs, so the page uploads come again.
- **Mode 7 with windows (code 7)** was the planar Mode 7 program's limit
  and no longer applies to true colour.
- **Stream longer than the program's 2,048 words (code 9).** A
  sprites-only frame with many OAM segments, 3,696 words. **Offset-per-tile
  differing within the frame (code 3).** Both go in bands now: a band's
  stream carries its first row's state as the row-0 slots and CGRAM, only
  its own rows' changes and only the OAM segments its rows use; the
  offset-per-tile table is compared over the band's rows only.
- The result: no CPU fallback over 40,000 frames; 82 snapshots exact,
  mixed frames included (checked as runs by `tc_snap` and `tc_rand`).

**The frog still does not appear, in either build.** In that scene the
game's frame handler overruns every frame ("frame handler still running",
56 times in one run). The renderer is only switched in when the game
idles or after three frames without an idle, and never while the handler
runs, so it gets almost no CPU: the planar build completes no render over
the whole scene. The two frames the true-colour build did draw show the
scene before its Mode 7 perspective table exists. The table at $7E:F100
is zero, as in snes9x at the same work RAM state, which fills it over the
following frames. Planning such a frame takes 0.05 ms on the host; the
2-7 s measured on the Falcon is starvation. Next step: profile the
68030 in that scene; the table's arithmetic is the likely cost.

## True-colour speedup, 2026-09-29

`dsp-2x-investigation.md`: 1.32x on the mean render time of 222 real tile frames (all exact),
up to 2.25x on the fixed-colour gradient frames, none on the frames at the output floor; the
tile program now keeps 72 page slots (cold code in P:$7E00-$7FFF and X:$0C64). Method and
scripts: `scripts/tc_bench/`.

## Status (2026-09-24, night)

Boot, the intro (Mode 7 on the DSP kernel), the title, the attract scenes
(field engine: Zeal, the Blackbird) and, with scripted input, the menu
shell: Start on the title opens the Battle Mode menu (Active/Wait), Down
moves the cursor, A confirms and opens Crono's name entry, where A types
letters. Runs to 12,000 frames without a stop. Snapshots every 250th
frame match the host render byte for byte.

Known: button presses are one-NMI pulses (`$F0`, computed in the NMI) and
the translated menu loop is slower than one NMI per pass, so a single
press is often missed; the input script repeats presses. The fix is speed,
or holding the NMI back while the main loop has not finished its pass
(F030SMW's `SMKX_SYNC`). Rendering is the C reference renderer, every
250th frame in these runs.

## The render context at once when the DSP idles (2026-10-06)

The 2026-09-27 tick table left 12% of the DSP's time in "the next picture
planned, waiting for the game to idle". The cause is in `rt_vbl`
(`falcon/smkx_rt.s`): the render context gets the CPU at the game's idle
marks or, in a scene without one, after `RWAIT` (3) VBLs, and a context
that waits for the DSP is left out of that count. When the VBL's poll takes
the finished picture, the context's pending send, or the plan the poll had
asked for, waits for an idle mark; the DSP idles meanwhile. Change: after
its frame, the VBL hands the CPU to the render context at once when work is
pending, the context is not waiting for the DSP and `smkx_inflight` is 0
(`.now`); otherwise the `RWAIT` count as before. The game keeps its frame:
a VBL over the render context still returns to the game.

Measured on the attract demo, 12,000 frames, release build, `CTX_LC=0`
(the whole-frame DSP renderer), Hatari v2.6.1-devel (Oct 1 2026),
`../../../F030Arcade/third_party/hatari/build/src/hatari.exe`, SHA-256
`600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf`;
emulated, not a Falcon. Logs `build/ctx/base-stderr.log`,
`build/ctx/new-stderr.log`; `scripts/ctx_fps.py` with the ranges below.

| scene (frames) | game fps before / after | pictures per second before / after |
|---|---:|---:|
| 1F4-60E, intro (Mode 7) | 50 / 50 | 9.1 / 11.1 |
| 60E-8CA | 50 / 50 | 5.0 / 5.4 |
| 8FC-DDE, title | 49.9 / 50 | 3.5 / 3.5 |
| E10-128E | 50 / 50 | 9.3 / 11.0 |
| 12C0-13BA | 50 / 49.0 | 3.6 / 6.3 |
| 1900-1F72 | 50 / 50 | 3.9 / 4.2 |
| 2008-21CA | 50 / 50 | 3.0 / 4.7 |
| 24B8-2936 | 50 / 50 | 3.2 / 3.9 |
| 299A-2EEA | 48.3 / 48.4 | 2.1 / 2.5 |

Whole run: 1,227 -> 1,390 pictures (+13%), game NMIs 10,025 -> 9,971
(-0.5%), VBLs the same. The DSP tick table: drawing 85.0% -> 86.9%,
"asked" (plan not started, DSP idle) 302 -> 37 ticks, the render phases
"held" (the game has the CPU while the DSP idles) 3,354 -> 2,603 ticks,
"done, not seen" 1,664 -> 2,125 (the poll sees more pictures finish
because more are drawn). What is left while the DSP idles is mostly the
plan itself (phase 1, 2.0% run and 4.0% held): the planner's time is the
next CPU cost, then the DSP kernel (`dsp-renderer-budget.md`).

Built natively on Windows: `scripts/make_win.sh ctx CTX_LC=0 CTX_FRAMES=12000`
(the make variables it passes are the whole trick, see the script), run with
`scripts/run_smkx.py --program build/ctx/CTX.TOS --tag new --fast`, which
on Windows stages the program as `AUTO\CTX.PRG` in `build/ctx/<tag>-drive`
and passes that directory (this Hatari takes no program path), adds
`--debug-except none`, uses no FIFO and stops only its own Hatari PID.

## Where the DSP's cycles go, by function (2026-10-06)

Hatari's debugger has a DSP profiler (`dp`, F030Arcade's DEBUGGING.md): it
counts every instruction's cycles per P address and reports hardware time
(cycles / 32 MHz) whatever speed the emulator runs at. `scripts/dsp_profile.py`
drives it inside a game run: a `--parse` script sets `logfile`, loads the
kernel's labels from its listing (`dspsymbols`), and two `:trace :once :file`
breakpoints on Hatari's VBL count turn it on and dump it (`dp save`); the
driver stops its own Hatari PID once the dump is on disk (no FIFO on
Windows), then sums the per-address cycles over each label's range, with
local labels (`_x`) folded into the global label before them. Hatari's VBL
count runs ~194 ahead of the game's `vbl` field (the TOS boot). Same build and
Hatari as the section above, `build/ctx/field-dp.prof`, `title-dp.prof`.

**Field** (game frames 2040-21C0, BG mode 1, BG1-3 and sprites, 384 VBLs,
7.69 s of DSP time, 43 pictures; `CTX_LC=0`, partial rendering on):

| what | functions | share |
|---|---|---:|
| composition | `comp_group` 15.8, sliver handlers `h4nl`/`h4nh`/`h2nl`/... 6.1, `merge` 1.8, `any_mask` 1.8, `slivers` 1.7, `split` 0.8 | 28% |
| output service (SSI) | `tx_isr` 10.5, its vectors 3.5, `fill` 3.2, `gate` 2.9, `tx_start` 1.1, `room` 0.8 | 22% |
| descriptors | `desc` 7.0, `layer_row` 5.5, `row` 4.0, `render` 1.6 | 18% |
| sprites | `obj_row` 5.4, `obj_sliver` 4.5, `obj_sprite` 1.2 | 11% |
| waiting for the host's next command | `g_h` | 11% |
| the row loop | `compose` | 5% |

**Title** (frames 960-DC0, BG mode 2 with offset-per-tile, 1,120 VBLs,
22.4 s, 80 pictures): `desc_opt` 21%, `comp_group` 9.6, `h4nl` 8.7, `h4fh` 5.2,
`h2nh` 2.7, `obj_row` 7.3, `obj_sliver` 3.9, `tx_isr` 6.3 + vectors 2.1,
`desc` 5.4, `layer_row` 4.2, `g_h` 2.2.

What it says. The transmit interrupt costs ~24 cycles a word (a long
interrupt: `jsr`, `jset`, `movep`, `bset`, `rti`), 128 words a row: about a
fifth of a field picture, a fixed cost per word sent. A two-word fast
interrupt cannot do it: the reader must see whether the writer has reached
the word (the DSP draws slower than the SSI drains, so the reader catches up
all the time), and a fast interrupt cannot test; fewer words (partial
rendering) is the only lever there. The drawing itself is composition plus
descriptors plus sprites, 57% in the field: the budget document's items 2
and 3 (keep layer configuration and decoded tile rows across rows, tighten
`comp_group`) address the largest shares; mode 2's offset-per-tile
descriptors (`desc_opt`) dominate the title. The 11% waiting for the host in
the field is the planner's time on the 68030 (the render context), the
picture rate's other limit.

**Inside the hot functions** (the same field profile, per instruction;
`python3 scripts/dsp_profile.py --tag field --report` gives the ranges, the
`.prof` the instructions). `comp_group` ran 134,864 groups; its plane loop
(`_pl`: OFF word, `lua`, the three-word gather by `mpy`/`mac` with KONST,
`and` with the pass's effective mask, `or` into ACC) ran 438,106 times, 3.2
planes a group at 15 instructions (34 cycles) each: 5.3% of all DSP time,
the single largest loop. The pass pre-test before it (the mask gather and
`and` with C_UNC) runs 101,879 times at ~24 cycles: 1.7%; the 8-word ACC clear
per group 0.9%. `obj_row` scans every sprite of the segment for each row
(`082B-0845`, 186,859 iterations at ~22 cycles: 1.5%) and clears 180 words
of OM per row (`rep #$b4`: 0.7%); the budget document's row buckets would
remove both. `layer_row` and `desc` are long straight-line code per layer
row (12,000 and 50,800 runs), mostly register shuffling, 12% together: the
configuration part (map size, character base, scroll factors) is the same
for every row of a frame unless a change descriptor says otherwise, and is
derived again each row. These are the measured entry points for the
kernel work: the plane gather first (an aligned-scroll path with two source
words, or slivers shifted once per tile row and reused), then the per-row
recomputation in `layer_row`/`desc`, then the sprite scan.

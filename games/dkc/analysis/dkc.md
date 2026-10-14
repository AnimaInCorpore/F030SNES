# Translating DKC (tools/smkx on the DKC1 disassembly)

Notes specific to *Donkey Kong Country*. The translator's design is in
[translator.md](translator.md) (written for SMK); this file records what
DKC needed on top of it. Emulator for every figure: F030Arcade's patched
Hatari v2.6.1-devel, submodule `639fadf6`, binary SHA-256
`b3279c6d…1f7d`; Falcon, TOS 4.04, 14 MB, `--cpu-exact`,
`--fast-forward` on. Frame counts are emulator VBLs, not Falcon speed.

## Placement (2026-09-24)

`tools/smkx/listing.py` parses `DKC1/Routine_Macros_DKC1.asm` (same
Yoshifanatic1 framework as SMK, v1.2.0) with four additions:

- **Banks `$C0-$FF`**: `macro DKC1BankXXMacros`, file offsets up to 4 MiB.
- **Data macros**: the script opcodes (`%DKC1_SSS_Op81(...)`,
  `%DKC1_AnS1_OpXX`, `%DKC1_CrS_...`) are expanded from
  `SNES_Macros_DKC1.asm`, so their `db/dw/dl` words are placed and scanned.
- **Conditionals**: `if/else/endif` resolved from `CONDITIONS`, the
  switches of the original build: no FastROM bank remap (code runs at
  `$80-$BF`), copy detection assembled, level sprite data inline.
- **Unsized operands** (`JMP CODE_BBAA26`, no `.w`): length from the ROM
  opcode. `%InsertGarbageData` resynchronises like an unsized `incbin`.

The graphics, tilemaps and SPC files are not in the checkout (the
disassembly extracts them from the ROM); their `incbin`s resynchronise at
the next address-bearing label. Result: **53,624 instructions placed and
ROM-checked, 14,452 labels, no label disagrees with its placement.**

## Analysis

Output now: 2,545 procedure contexts over 2,324 entries, 51,148
instruction addresses reached (50,770 of the listing's), 129,525 nodes, 0
unsupported nodes, 0 traps; analysis 7-12 s, liveness and emission ~27 s.

- **Size.** The first complete analysis had 875,841 nodes. `JMP
  (DATA_BF8177,x)` (the sprite routine table, ~200 entries) was inlined
  into every procedure that reached it, three entries times four bank
  contexts of 11,715 addresses each; and callees returning in many data
  banks gave one address up to nine states that differed only in DBR.
  Table jumps with nothing pushed are now tail calls (`tailtable`), and
  `STATE_CAP`/`CTX_CAP` are 1 (the DBR goes to the runtime sooner). The
  table interleaves routine and parameter words (`dw Routine,$0030`,
  indexed by id*4); non-ROM words become `rt_badtable` slots.
- **Liveness** linked each return to each caller's continuations by
  scanning the caller's whole continuation map; with a join node per
  procedure the pass went from more than 10 minutes to 6 (875k nodes) and
  ~27 s (129k).
- **Named labels as seeds.** Besides `CODE_xxxxxx`, the listing names
  hundreds of routines (`DKC1_NorSpr05_Kritter_Main`); every label placed
  on an instruction is a dispatch seed when a table or immediate names it.
- **Decimal mode**: DKC never sets D (no `SED`, no `SEP` with bit 3); the
  interrupt entries are analysed binary.
- **Findings left** (report.txt): the lag handlers `CODE_80A980/A98A` are
  also seeds of other dispatch sites and "return below their frame" there
  (they are never reached that way); `$B8BAB9` is an overlapping
  instruction (`A9 80 85 64 FA` is `LDA #$80 : STA $64 : PLX` in 8-bit
  mode, which the listing writes as `LDA.w #$8580 : STZ.b $FA`).

### The direct page register

The sprite spawn-script interpreter (`CODE_B58064`) points D at a sprite's
table slot: `TCD : LDA $0002,y : STA $00,x`. All 30 other `TCD`s follow
`LDA #$0000`. The analysis carries `dp` in states and procedure contexts:
a constant after `LDA #imm : TCD`, unknown otherwise. Code with D = 0
stays on the `(dp,a5)` fast path; a constant page is folded into the
address; an unknown D is read from `rt_dp` (direct modes through the
runtime's generic path, pointer modes through a1 = a5 + D). Pointer
dispatch sites must run with D = 0.

### Calls written out

The animation-script interpreters (`CODE_81D95A`, `CODE_BE815F`) call a
handler as `PHK : PEA ret-1 : JMP [$007A]`; the handler's `RTL` returns to
`ret`. The analysis recognises the three pushed bytes as a call with an
explicit return address (`pushed_calls`), the emitter as a `jsr`.

## The game lives in its NMI

DKC's main program only ever waits: `WAI : BRA *`. The NMI (`CODE_80A96C`)
saves A/X/Y, forces blank and jumps through `$1C`:

- `CODE_80A980` (INIDISP from its RAM copy `$051A`, restore, `RTI`) while
  a frame is still being computed; `CODE_80A98A` returns at once;
- otherwise a frame handler (`CODE_8080AB`, `CODE_80BC7B` for the logos,
  `CODE_80CCB6`, ...), which starts with `LDX #$01FF : TXS`, runs the game
  frame, stores itself in `$1C` again and waits in the next `WAI` loop.
  `CODE_80C1C9` switches handlers: `STA $1C`, wait for the NMI flag to
  clear, enable NMI (and H/V IRQs, whose handler is `SEI : RTI` and only
  ends the `WAI`), `WAI`.

The runtime therefore takes the NMI where the program waits
(`SMKX_NMI_AT_WAI`): the VBL only flags it; `WAI` is `jsr rt_wai`, which
waits for the flag and performs `CODE_80A96C` natively. For the lag
handlers it writes INIDISP and returns into the `WAI` loop; otherwise it
looks the handler up in the dispatch table (context m16 x16, it is a
`JMP ($001C)` target in the analysed NMI), enters it with C = `$xx8F`, X
and Y as they were, S minus the ten bytes the NMI pushed, and a7 at the
program's base. `TCS`/`TXS` (all 17 of them load `#$01FF`) reset a7 to
that base too (`rt_sp_base`, taken at reset). NMIs that arrive while a
frame runs are dropped instead of running the lag handler; the next
`WAI` takes the pending one at once, which is as fast as the Falcon can
go.

## Runtime

- **Memory.** No ROM word view (it would take 8 MiB): 16-bit ROM loads are
  `move.w` + `ror.w #8`. DKCX.TOS is 8.4 MB (4 MiB ROM, ~4 MiB code); page
  tables (512 KiB) and WRAM are BSS.
- **SRAM** at `$B0:6000` (2 KiB), mapped by the HiROM rules for
  `$20-$3F`/`$A0-$BF`. No DSP-1: `$6000-$7FFF` in `$00-$1F` reads 0.
- **DMA**: F030SMW's fast paths (VRAM, OAM, CGRAM) plus the WRAM port:
  DKC's reset clears all 128 KiB through `$2180` from a fixed zero byte.
- **APU**: DKC uploads a 40-byte loader through the IPL to `$04B8`
  (`CODE_8AB0E8`) and sends everything else through it (`CODE_8AB4FF`):
  wait for port 0 to echo `$00`, byte on port 1, address on ports 2/3,
  port 0 = `$00` with bit 7 flipped and bit 0 set (bit 0 clear: run at
  ports 2/3). The started driver reports ready with port 1 = `$FF`
  (`CODE_8AB204`). The echo model does all of it; the only DKC line is
  "port 0 with bit 0 clear sets port 1 to `$FF`". Sample and music
  uploads are long: four port writes per byte, each a runtime call.
- **Pads**: `$4218/$4219` latched per frame from the keyboard (F030SMW's
  handler) and `DKCX_INPUT`; `$4016` reads 1 (a standard pad in port 1,
  checked by `CODE_80C0F8`), `$4017` reads 0.
- **Speed-ups from F030SMK**: page tables for generic accesses, a dispatch
  cache, `DKCX_DIAG=0` for a build without per-call diagnostics, the
  frame-start copy limited to what the renderer reads.

## First runs (2026-09-24)

- The reset code clears WRAM and VRAM, uploads the loader and the sound
  program, then the logo handler (`CODE_80BC7B`) shows "Nintendo
  presents" (BG mode 3) while it streams samples to the APU.
- The intro cutscene follows (Cranky's gramophone, DK dropping in with the
  boombox, the jungle), then it loops. Scripted Start presses reach the
  file select ("SELECT A GAME") and the player-mode menu.
- No trap, no dispatch miss, no exception in any run so far.
- Every snapshot checked so far (16-colour path, 20+ frames from the logo
  through the menus) matched the host render byte for byte.
- With rendering every 250th frame and the F030SMK speed-ups, 3,442 NMIs
  ran in 4,500 VBLs through the intro and menus.

One bug on the way, found with a Hatari memory breakpoint on WRAM `$1C`
(`run_smkx.py --parse`): a new `.wram` block in `rt_dma` sat after the
`.cgram` loop, which falls through, so every CGRAM DMA also ran the WRAM
copy and wiped `$0000-$01FF`.

## Rendering budget (2026-09-25)

From F030CT's `analysis/dsp-renderer-budget.md` (renderer review, emulated
in patched Hatari `v2.6.1-497-g639fadf6`), applied to DKC:

- **DKC needs eight planes.** Colours per frame, counted from the Falcon
  planes of the eight-plane snapshots of 2026-09-24 (`build/ppu_snap`
  prints the frame count; its counter wrapped past 255 hits and printed
  248-305 before the fix of the same day):

  | Snapshot | Scene | CGRAM indices | Most on one row | Palettes of 16 |
  |---|---|---:|---:|---:|
  | F00001F4, F00003E8 | logo | 31, 69 | 13, 39 | 2, 6 |
  | F00005DC-F0000FA0 | intro, jungle, title | 114-134 | 69-73 | 8-9 |

  Four planes (16 colours) would lose most of that, and there is no row
  that would fit, so the payload stays 57,344 bytes a frame.
- **The SSI sets the floor**: 1 MB/s at the 32 MHz crossbar clock
  (Atari Developer Support Package, Hardware p. 21) gives 57.3 ms a
  frame; the clock the ports use now gives 72.9 ms, and CT measured 76 ms
  for a blank frame (emulated). Rendered frames therefore reach at most
  ~13/s at the current clock, whatever the kernel does; the first target
  is a kernel fast enough to keep the SSI busy (CT: 164-205 ms a frame
  now, 2.2-2.7x to go).
- **Under 40 ms** needs a different output path, not a faster kernel:
  host-port readback by the 68030 has to be benchmarked first (programmed
  I/O; its cost comes out of the game's CPU time). Dirty rectangles do
  not help DKC's levels, which scroll and animate tiles continuously.
- **Game cadence is separate**: the NMI (game frame) and the render run
  at their own rates; measure game/NMI code, planning, uploads and
  presentation on their own before judging frame rate.
- **BG modes**: the logo screen's snapshot says mode 3 (the I/O copy),
  but HDMA sets mode 1 for rows 0-149 and mode 5 below, which the
  reference, the fast composer and CT's planner all draw as mode 1. Read
  a frame's modes from the replayed registers, not from `io[$105]`.

## The level-load bug (2026-09-25)

Jungle Hijinxs showed only its background: no ground, no Kongs, nothing
moving. Found by bisecting against `tools/smkx/ref65816.py` with dumps at
game frames and at the n-th execution of a probed address
(`DKCX_DUMP_PC/_HIT`; the reference counts executions from reset, so a
routine's first execution in the frame is the easy checkpoint). Four
causes, in the order they turned up:

- **The 65816 V flag**: the Falcon differed at the first `$B99412` in
  game frame 3132; with F030SMK's change (V in the byte `rt_vf`, not the
  CCR, commit 729869b) it did not any more. Not narrowed further.
- **`LDA.l DATA_BC903C&$FF0000,x`** (`CODE_BCB052`, the level's entry
  data, X >= `$8000`): a long address in bank `$BC` below `$2000` plus a
  16-bit X reads ROM at `$BC:8xxx`, but the translator took the base for
  the low-RAM mirror and read WRAM + X. Now such operands (banks
  `$00-$3F`/`$80-$BF`, base below `$2000`, not `$7E/$7F`) get `dp,X`'s
  range check with the bank added on the far path (`tools/smkx/emit.py`,
  Loc "dpx" with `bank`). Five instructions in DKC. Found at the first
  `$809F41`: `$1B23`/`$1B25` held `CODE_BCB052`'s results.
- **The OAM address at VBlank** (both models): DKC prepares the OAM DMA
  in the frame handler (`CODE_80A93F`) and starts it early in the next
  frame (`$808C6F`) without writing `$2102`: the PPU reloads the OAM
  address from `$2102/$2103` when VBlank starts, unless the screen is
  in forced blank. Without the reload each frame's list landed `$220`
  bytes further on, so sprites were missing or scattered. Found from OAM
  in a snapshot (tile `$FF`, attribute `$FF` throughout while WRAM `$200`
  held a real list) and a trace of `$420B` writes in the reference.
  `rt_wai` and `Machine.wai` reload it now.
- **VRAM reads** (both models): `$2139/$213A` return a prefetch latch,
  loaded when `$2116/$2117` is written and reloaded before the address
  steps on a read, so the first word after setting the address comes
  twice; games read once first, as `CODE_B99412` does before copying
  VRAM `$6C00` to `$7400`. Without the latch that copy was shifted by
  a word. `rt_vprefetch` in the runtime, `vprefetch` in the reference.

Also fixed: `rt_write_dump` loaded the frame number into d7 before saving
it; d7 holds the frame handler's address there, so a run with more than
one dump jumped to `$80xxxx` = the frame number.

After the fixes Falcon and reference agree at every 40th game frame
from 3133 to 4693, and every snapshot's planes match the host. With Right
held from game frame 3300 the Kongs walk, jump and meet the first enemy.

## The DSP renderer (2026-09-25)

F030CT's DSP tile renderer (`falcon/dsp/ppu.asm`, driver `src/smkx_ppu.c`,
planner `src/ppu_dsp_plan.c`, boot loader `falcon/smkx_dspboot.s`, checks
`tools/dsp_snap.c`, `tools/dsp_rand.c`, `tools/dsp_model.c`, PPUTEST),
taken over from F030CT's commit b307a8b/0511390 state. It fits DKC's
display as it is: eight planes, pixel = CGRAM index. `smkx_render` asks
it every frame; it starts a frame whenever the last one is on screen and
the game runs on meanwhile; the fast composer takes the frames the DSP
cannot (every 4th call). The palette of a DSP frame is its frame-start
CGRAM, loaded at the VBL that shows it (DKC's `rt_clut`, not CT's direct
load). `rt_render_sync` waits for the frame when a snapshot is due, so
snapshots stay comparable.

What DKC needed on top:

- **93 VRAM page slots instead of 77.** DKC's pre-rendered backgrounds use
  almost every tile: level and jungle frames read 78-85 pages of 256
  words, CT's frames 22-29. CT's per-layer character tables (CADDR,
  X:$0700-$16FF, 4K words) are gone: a descriptor computes a character's
  address (`base + c << size`, then the page table) when a layer reaches
  a new 8-row block, and the freed memory holds 16 more pages
  (P:$4700-$56FF; `smkx_ppu.c`, `pputest.c`).
- **`changed` only for registers the planner reads** (`snes_ppu.c`): the
  levels rewrite CGRAM by HDMA on every line, which marked every row.
- **Visible map columns only** in the planner's page marking (34 of 64
  per map row; a row is marked again for another first column).
- The snapshot layout (2 KiB SRAM, eight planes, no palettes) in
  `dsp_snap.c`; `ppu_planar8` from `ppu_fast.c`.

Measured (patched Hatari `639fadf6`, emulated; not a physical Falcon):

- PPUTEST: 6 DKC frames (jungle 78-81 pages, map, menu) and 16 random
  frames exact. Render and send: jungle 187-214 ms, map 139-143 ms, menu
  159 ms.
- In Jungle Hijinxs with Right held (game frames 3306-4135): every frame
  on the DSP (109 frames, no fallback), 3.9 frames shown per second, 31
  game frames per second; before, 0.8 shown and ~3 game frames per second
  in a normal build. All 17 composed snapshots of the run exact.
- On the 68030 per DSP frame: planning 48.7 ms (inside the VBL handler:
  this is what keeps the game at 31 instead of 50), uploads 7 ms, sending
  the stream 0.4 ms. The host profile puts half the planner in page
  marking and a fifth in the HDMA replay, but halving the marking changed
  the Falcon's figure little: profile it there next.

## Rendering in idle time (2026-09-25, later)

From F030CT's commits c2ef936 and b0b8378 (porting-notes, "register replay
and planning ~5x cheaper", "render work in the game's idle time").

- **Replay and planner** (`src/snes_ppu.c`, `src/ppu_dsp_plan.c`,
  `src/smkx_ppu.c`): HDMA stepped event to event through host pointers
  (`aptr`), repeated units skipped, `changed` only on real changes, map
  blocks' character pages cached and trusted while `rt_plan_dirty` shows
  them clean. One difference kept from DKC: CT counts a CGRAM change as a
  change; DKC's levels rewrite CGRAM on every line and the planner does
  not read it, so `changed` stays limited to `planned_reg`. DKC's
  visible-column marking is dropped: with the block cache whole map rows
  are cheap, and frames need at most 85 of the 93 page slots. Planning per
  DSP frame 48.7 -> 18.7 ms; game 31 -> 35.6 fps, shown 3.9 -> 4.4.
- **Idle time** (`falcon/smkx_rt.s`, `src/smkx_video.c`): simpler than
  CT's, because DKC's VBL never runs game code (the NMI is taken at WAI).
  The VBL shows a DSP frame that has arrived (`rt_flip`, palette with it)
  and asks for the next composition (`rt_rstate`); while work is pending
  the frame-start copy is skipped. `rt_wai` runs the work in its own
  context (`rt_render_thread`, `rt_rstack`) while it waits for the NMI; a
  VBL over that context saves it and returns to the game in `rt_wai`,
  which takes its NMI and resumes the work at the next WAI. The CPU
  fallback runs there too, so it no longer stalls the game. Snapshots and
  the frame limit wait until no work is pending (`rt_snapdue`,
  `rt_snapnow`).
- **Measured** (patched Hatari `639fadf6`, emulated, Jungle Hijinxs with
  Right held): DIAG build 41.0 game fps and 3.3 shown; release build 43.9
  and 3.9. Game code 12-15 ms a frame. The display now waits for the
  planning to get through the game's idle time; pipelining planning with
  the DSP's rendering is the next step. Snapshots exact (20 of 20, 11 of
  11).

## Checked against the other ports' findings (2026-09-25, night)

- **Mode 7 on the DSP (F030CT)**: not needed. No DKC frame captured so far
  is in mode 7; the logo's BG-mode HDMA table (`$0687`) holds modes 1 and
  5. The matrix registers serve as the multiplier (36 writes to A/B, 17
  product reads); `CODE_81CC34` fills the whole matrix during the logo
  without mode 7.
- **Latched V counter (F030CT's correction)**: not applicable. DKC never
  latches or reads the H/V counters; it reads `$213F` twice.
- **Indexed operands that leave their bank or their region (F030SMK)**:
  `tools/smkx/refaudit.py` ran the reference to game frame 4699 (boot,
  menus, map, Jungle Hijinxs with Right held). No access carried into the
  next bank (984 bank-$7F sites with an index exist; eight reads of
  `DATA_89FF00,x` could, with X >= $100). 75 sites used a low-RAM-mirror
  base with an index past $2000 (`LDA.w $0000,x` with a pointer in X,
  `LDA.l DATA&$FF0000,x`); all 75 are translated right (73 through the
  runtime as indexed pointers, 2 range-checked).
- **Jump tables cut short (F030CT)**: none found. In the run, 27 table
  sites were used, none past its translated table (three more are dynamic
  dispatches through `rt_dispatch`). In the listing, 13 of 115 tables have
  a label inside their dw run; 11 are parallel tables of equal size, and
  the other two (`DATA_81D9E6`, 10 entries; `DATA_BFF7E0`, 9) are followed
  by data read under its own label. CT's range guard (two instructions
  per table jump) is not taken over yet.

# Porting notes

Findings from the other SNES ports (F030SMK, F030SMW, F030SMW2, F030DKC,
F030CT) that help this one; the rule is in `AGENTS.md`. Each entry names
the port it comes from and says whether this project's copy of the shared
code already has the fix. Newer entries go at the end.

SMK is where the shared code started; its own findings are in
`analysis/translator.md` and were sent to every other port on 2026-09-24.
Nothing has come back from the other ports yet.

## From F030SMW, 2026-09-24

Findings from the Super Mario World port (details in F030SMW's
`analysis/smw.md`). None of the code changes below is in this copy yet;
the files to take over are named per entry. Emulated timings: patched
Hatari `639fadf6`, not Falcon measurements.

### Listing and translator

#### Placing an Asar disassembly with the symbol file — method

For an Asar source tree (SMWDisX), F030SMW's `tools/smkx/asar.py` walks
the source as Asar assembles it (incsrc, ORG/base, arch, defines,
if/while, functions, macros with `<arg>` and `?label`, anonymous labels)
and checks every label against Asar's `.sym` output; each instruction is
decoded from the ROM at its offset and must match the source mnemonic
(`listing.parse_smwdisx`). SMW: 62,465 instructions, 9,271 labels, no
mismatch, 7 s. Two Asar pitfalls: directives are lower case (`rep` is a
directive, `REP #$30` an instruction; matching case-insensitively shifted
everything after the first REP), and a quoted macro argument used inside
quotes must not be quoted twice. Worth using for any port whose
disassembly assembles with Asar.

#### Inline jump tables behind a call — pattern

SMW's `JSL ExecutePtr` pulls its return address and jumps through the `dw`
table after the call, indexed by A (`ExecutePtrLong`: `dl`). The analysis
now treats such a call as a table jump inside the caller (entries in
8-bit widths, no frame pushed); the emitter repeats what the routine
leaves behind ($00-$03, C = entry, Y restored, carry clear) and jumps
through a label table. It turned about 20,000 unreached instructions
into reached ones. A hand-written form (`PHK : PER $0003 : JML
ExecutePtr`) occurs once. Look for routines that pull their return
address and index a table behind the call site.
Files: `tools/smkx/analyze.py` (`execptr`), `tools/smkx/emit.py`
(`execptr`), `tools/smkx/liveness.py` (kill at the dispatch).

#### Code the game writes into RAM

SMW builds a 128-store OAM clear routine at `$7F8000` during reset and
calls it with `JSL` (one caller enters at +302). Not translated; the call
becomes a runtime helper (`rt_oamreset` in `falcon/smkx_rt.s`, `oamreset`
in analyze/emit). "code outside ROM at $7Exxxx/$7Fxxxx" in the report is
the symptom.

#### Interrupt entries in binary mode

With the NMI/IRQ entered with D unknown, 32 ADC/SBC nodes in interrupt
code could not be translated. SMW never sets D (no SED, no SEP with bit
3), so its interrupt entries are analysed with D clear
(`analyze.analyse`). Check the game for SED/SEP #$08 first.

#### Flag liveness: dispatch callers (safe) and a failed optimisation (unsafe)

- Safe, recommended: `liveness.compute` linked RTS/RTL only to static call
  sites (`prog.callers`); procedures also reached by pointer dispatch
  (`prog.dyn_callers`, continuation "assumed") got no edge to those
  continuations. F030SMW adds them (`all_callers` in
  `tools/smkx/liveness.py`).
- Unsafe, do not port: in SMW 34,344 of 95,540 nodes save/restore the CCR
  only to keep V (58 BVC/BVS read it), because escaped returns keep all
  flags live and every PHP reads all flags. Precise successors for escaped
  returns plus PHP reading only what is live after its PLP cut the saves
  to 636 and roughly doubled the level frame rate, but Mario died in the
  attract demo. Bisection: any subset of the 76 PHP/PLP pairs is fine,
  only all together fail, so a flag reader or edge is missing from the
  equations and the "all live" cycle hid it. Reverted.

### Runtime

#### DMA byte by byte made the NMI overrun

OAM, CGRAM and all VRAM DMA except "mode 1, increment 1 after the high
byte" went through `rt_io_write` per byte; SMW's stripe images (fixed
source RLE, vertical stripes with increment 32) made the NMI overrun and
~57% of VBLs were lost in the attract demo. `rt_dma` now has direct loops
for VRAM (pairs or one half, any increment, fixed or incrementing
source), OAM and CGRAM (CGRAM only while no IRQ log is written). After:
no overruns. File: `falcon/smkx_rt.s` (`rt_dma`).

#### Forced-blank frames are not composed

`smkx_render` returns 0 when the frame starts in forced blank and no IRQ
writes a register; the runtime keeps the shown buffer, the CLUT goes
black, and `ppu_snap` skips such snapshots. Loading screens stop paying
for the reference renderer. Files: `src/smkx_video.c`,
`falcon/smkx_rt.s` (`rt_render`), `tools/ppu_snap.c`.

#### APU: the return-to-loader request is per game

SMK writes `$1F` to port 0; SMW writes `$FF` to port 1 before each
upload (`StartMusicUpload`). It is the only game-specific line of
`rt_apu_write`'s state 2.

#### Input for tests

Keyboard as pad 1 through the ACIA interrupt (TOS's handler replaced
for the program's lifetime; IKBD packets skipped; `$4218/$4219` latched
once per frame) and a frame-timed input script built into the program
(`SMWX_INPUT="2000:1000,2008:0000"`, `tools/smkx/input_script.py`).
`$4016` bit 0 = 1 tells SMW a pad sits in port 1. Script by game state
where possible: scripted frames shift when the game's speed changes.
Files: `falcon/smkx_rt.s` (`rt_acia`, `kbd_map`, `rt_frame`), Makefile.

#### LoROM

`analyze.rom_index`, `rt_build_banks`/`rt_resolve`/`rt_wb` (SRAM at
`$70-$7D:0000`, ROM mirror in `$40-$6F` low halves), vectors at file
offset `$7FE0+`, `ppu_abus_lorom` for HDMA tables. For LoROM games.

### Measurement and build

#### Real frame rate from the game's own counter

SMW's `TrueFrame` ($13) counts finished main-loop passes; the status
line prints it next to the VBL count, so lag frames show directly.
Rendering off, SMW's level scenes need a little more than one 20 ms VBL
per game frame on the emulated 16 MHz 68030: half speed, every second
NMI a lag frame. The title screen's own timing hides this (it counts
NMIs, not game frames).

#### make 3.81 compares whole seconds

Two `make` runs with different switches inside one second keep the first
link (`flags.stamp` and the TOS share a second). F030SMW's stamp rule
deletes the TOS whenever the stamp changes. File: Makefile.

### Video

F030SMW moves its display path to 256 colours in 8 planes (the Videl
palette holds CGRAM itself; SNES 4bpp tiles are already planar, so tile
rows copy into planes 0-3 and planes 4-7 carry the palette number). A
follow-up entry will report how it performs.

## From F030SMW, 2026-09-24 (later): a 256-colour display path

F030SMW replaced the 16-colour adaptive palette with 256 colours in eight
Videl planes (commit `967d28f` there; details in its `analysis/smw.md`).
Not in this copy; the files are `src/ppu_fast.c`, `src/ppu_fast.h`,
`src/ppu_internal.h`, `src/snes_ppu.c` (internals exposed, no behaviour
change), `src/smkx_video.c`, `falcon/smkx_video.s`, `falcon/smkx_rt.s`
(`rt_clut`, snapshot v4), `tools/ppu_snap.c`, `tools/c2p_test.c`,
Makefile.

- **Why**: SMW frames show 225-255 distinct CGRAM entries; any SNES game
  with many sub-palettes loses badly at 16 colours. In eight planes every
  pixel is its CGRAM index and the Videl palette is CGRAM itself (scaled
  by INIDISP brightness), so nothing is quantised and fades cost 256
  palette writes.
- **Videl timings**: take the working 4-plane values and move HDB and HDE
  16 units later; line width 128 words; SPSHIFT `$0010`. The fetch offsets
  depend on bpp (Hatari's `videl.c`: `128/bpp`-based, 32 units at 4 bpp,
  16 at 8 bpp, RGB and VGA alike). Load the CLUT at the start of the VBL
  that shows the new screen base, not while composing.
- **Composer**: rows of CGRAM indices, a character row at a time: each BG
  layer decoded once per row (spread tables, palette added, opaque mask),
  painted back to front in the mode's priority order; sprites from
  per-frame row buckets with the reference's 32-sprite/34-sliver rules.
  The reference renderer with an identity map is the checker:
  `ppu_snap` compares pixel for pixel (16 snapshots from boot to a level,
  0 differences).
- **c2p**: 32 pixels in eight registers, five merge stages (bit swaps of
  8, 1, 16, 2, 4 between registers 4, 4, 2, 2, 1 apart; chunk g = i4 i3
  i2 loads into register (~i3, ~i4, ~i2); register n ends as plane
  (n&1)*4 + (n>>1&1)*2 + (n>>2&1)), derived as a permutation of address
  bits and verified symbolically and by `make test-c2p`.
- **Cost** (emulated, patched Hatari `639fadf6`): 0.9 s of Falcon time per
  full mode-1 frame in C, c2p 0.15 s of it; the rest is memory traffic on
  ST-RAM (table lookups, VRAM reads). Next steps planned there: a
  tile-row cache, 68030 assembly, then composing and c2p on the DSP.
- For this game: the adaptive palette and the DSP Mode 7 kernel (4-plane
  output) remain valid only on the 16-colour path; an 8-plane variant of
  the kernel would drop the slot table.

## From F030SMW, 2026-09-24 (later still): tile-row cache; make correction

### Tile-row cache for the 256-colour composer — not yet in this copy

F030SMW's composer (`src/ppu_fast.c`, see the 256-colour entry above) now
caches decoded character rows: eight colour bytes (palette not added) and
the opaque mask per VRAM word address, for 2 and 4 bpp, unflipped (an
h-flip is a byte reversal, `__builtin_bswap32`), used for BG rows and
sprite slivers. The runtime marks every written 256-word VRAM page in
`rt_vram_dirty` (the `M7DIRTY` macro and `rt_m7_mark` in
`falcon/smkx_rt.s`, now for all 128 pages, not only the Mode 7 ones);
`smkx_render` drops those pages' rows before composing
(`ppu_fast_invalidate_page`). Recognising a missed mark is cheap:
`ppu_snap`'s "Falcon planes vs host" comparison would differ, since the
host starts with an empty cache. Ten SMW snapshots from boot through a
level to the overworld (heavy VRAM reloads): 0 differences.
Gain (emulated, patched Hatari `639fadf6`): 0.9 s -> 0.74 s per full mode-1
frame; composing 0.75 -> 0.59 s. What remains in the C row decoder is
tilemap reads, address arithmetic and register spills to the stack in
ST-RAM, so the next step there is 68030 assembly, then the DSP.

### Correction: the make 3.81 fix recommended earlier is not enough

The earlier F030SMW entry suggested deleting the TOS in the switch-stamp
rule. Within one make run, 3.81 had already looked at the TOS, did not
notice the deletion and skipped the link, leaving no binary (seen when two
builds with different switches ran back to back). F030SMW's Makefile now
has `smwx: $(SMWX)/flags.stamp` with a recipe that runs a sub-make for the
TOS (`@$(MAKE) --no-print-directory $(SMWX)/SMWX.TOS`); the sub-make looks
at the files afresh. Take over the stamp rule and that target if the
stamp pattern is used here.

## From F030DKC, 2026-09-24

Findings from the Donkey Kong Country port (details in F030DKC's
`analysis/dkc.md`). DKC boots, uploads its sound program through its own
loader protocol and plays the Nintendo logo and the intro cutscene; all
snapshots match the host render byte for byte. Emulated timings: patched
Hatari `639fadf6`, not Falcon measurements. None of the code below is in
this copy yet; the F030DKC files are named per entry.

### Translator

#### Liveness linked every return to every caller's continuations by scanning — speed fix

`liveness.compute` added, for each RTS/RTL, edges to the continuations of
each caller by scanning the caller's whole `conts` dict: returns x callers
x continuations. On DKC's first, oversized analysis (875k nodes) the pass
ran for more than ten minutes. Now every return flows into one join node
per procedure, which flows to the callers' continuations (indexed by call
node): same result, far fewer edges. 875k nodes: 6 min; 129k nodes: 27 s
including emission. File: `tools/smkx/liveness.py` (`conts_of`, the
`("ret", pkey)` node). SMK's analysis is ~105k nodes; the join node shortens its liveness pass too (no behaviour change).

#### Analysis size: table jumps and data-bank states

DKC's first analysis had 875,841 nodes for 51,132 instruction addresses.
Two causes, fixed in `tools/smkx/analyze.py`:

- `JMP (DATA_BF8177,x)` reaches ~200 sprite routines; the analysis flowed
  into all of them inside every procedure that reached the table (11,715
  addresses per procedure, three such entries, four bank contexts each).
  Table jumps with nothing pushed are now tail calls (`tailtable`): each
  handler is its own procedure and returns through the jumping
  procedure's frame, as dynamic JMPs already did; the emitter jumps
  through a table of entry labels. Words of the table that are not ROM
  code (DKC interleaves parameters: `dw Routine,$0030`, indexed by id*4)
  become `rt_badtable` slots.
- Callees return in many data banks, so one address carried up to nine
  states differing only in DBR, and every procedure existed in four or
  five bank contexts. `STATE_CAP` 8 -> 1 and `CTX_CAP` 4 -> 1 leave the
  bank to the runtime sooner (a3/a4 make that cheap).

Result: 129,437 nodes, analysis 7-12 s, coverage unchanged. SMK's caps work at ~105k nodes; nothing to do unless code size matters.

#### The direct page register D

DKC's sprite spawn-script interpreter points D at a sprite's slot
(`TCD : LDA $0002,y : STA $00,x`); every other TCD is `LDA #$0000 : TCD`.
The analysis state and procedure context now carry `dp`: a constant after
`LDA #imm : TCD`, None (runtime) after any other TCD or a PLD of an
unknown value; PHD/PLD pairs keep it. The emitter folds a constant D into
the address (a nonzero page goes through `host_or_rt`, so D = `$2100`
reaches the I/O path); a runtime D reads `rt_dp` (direct modes through the
generic runtime path, pointer modes through a1 = a5 + D). TCD, TDC, PHD
and PLD maintain `rt_dp`; pointer dispatch sites must have D = 0. Files:
`tools/smkx/analyze.py`, `emit.py` (`dp_base`, `loc`), `liveness.py` (TCD
defines N and Z), `falcon/smkx_rt.s` (`rt_dp`), `falcon/smkx_rt.i`.
SMK keeps D = 0 (the current emitter hard-wires `(dp,a5)`); nothing to do unless a TCD shows up.

#### A game that lives in its NMI: NMI taken at WAI, TCS resets the 68k stack

DKC's main program only waits (`WAI : BRA *`). The NMI (`CODE_80A96C`)
saves A/X/Y, forces blank and jumps through `$1C`: to a short handler
that restores INIDISP and returns while a frame is still running, or to
the frame handler, which starts with `LDX #$01FF : TXS`, runs the whole
game frame and ends in the next WAI loop. Running that inside the VBL
interrupt, as SMK's model does, would run whole game frames at interrupt
level and nest. F030DKC's model: the VBL only flags the NMI;
`WAI` becomes `jsr rt_wai`, which waits for the flag and performs DKC's
NMI natively (lag handler: INIDISP, return; otherwise the frame handler's
entry comes from the dispatch table and it is entered with fresh stacks);
TCS/TXS also reset a7 to the base taken at reset (`rt_sp_base`), since
every TCS/TXS in DKC restarts from an empty stack. Symptom in the report:
"reached WAI" traps and a `JMP ($xxxx)` in the NMI. Files:
`tools/smkx/analyze.py` (WAI falls through), `emit.py` (WAI, TCS, TXS),
`falcon/smkx_rt.s` (`rt_wai`, `SMKX_NMI_AT_WAI`, `rt_sp_base`),
`falcon/smkx_rt.i`. SMK waits for its NMI in a flag loop; no WAI loop, nothing to do.

#### A JSL written out: `PHK : PEA ret-1 : JMP [dp]`

DKC's animation-script interpreters call a handler through a long pointer
this way; the handler's RTL returns to `ret`. As a tail jump, the RTL
would return to the wrong 68k frame. The analysis now recognises three
known pushed bytes before `JMP [dp]`/`JML [dp]` as a call with its own
return address (`pushed_calls`); the emitter dispatches with `jsr` and
continues at `ret`. Files: `tools/smkx/analyze.py` (`pushed_calls`,
`resume_after_call`, `escape_into`), `emit.py` (`call`, `frame_size`).
Search SMK's listing for `PEA` followed by `JMP [`/`JML [`.

#### Named code labels as dispatch seeds

`address_taken` only knew `CODE_xxxxxx`; DKC's listing names hundreds of
routines (`DKC1_NorSpr05_Kritter_Main`), which appear in `dw` tables and
immediates. Every label placed on an instruction now counts, its address
from the name or, without one, the FastROM mirror of its offset.
File: `tools/smkx/analyze.py` (`label_address`, `address_taken`).
SMK's listing names code `CODE_xxxxxx`; nothing to do.

#### Yoshifanatic1 listings: macros, conditionals, unsized operands

`tools/smkx/listing.py` for DKC: data macros from `SNES_Macros_*.asm` are
expanded (script opcodes such as `%DKC1_SSS_Op81(...)`); `if`/`else`
blocks are resolved from a table of the original build's switches
(`CONDITIONS`); `%InsertGarbageData` resynchronises; an operand without a
`.b/.w/.l` hint (`JMP CODE_BBAA26`) takes its length from the ROM opcode;
banks `$C0-$FF` (4 MiB). All 53,624 instructions place and check.
SMK uses the same framework; its listing parses already, the additions are harmless there.

### Runtime

#### The ROM word view is two bytes per ROM byte

`smkx_romw` holds a big-endian word for every ROM offset: 8 MiB for a
4 MiB ROM, 12 MiB with the image, of the Falcon's 14. F030DKC dropped it:
16-bit ROM loads are `move.w` + `ror.w #8` like WRAM loads (`emit.py`:
`Loc.rom`), and `rt_build_words`/`smkx_romw` are gone from
`falcon/smkx_rt.s`. SMK is 512 KiB: the view costs 1 MiB, nothing to do.

#### DMA to the WRAM port `$2180`; a fall-through to watch for

DKC clears all 128 KiB of WRAM at reset by DMA from a fixed zero byte to
`$2180`; F030SMW's `rt_dma` (taken over here) sent that byte by byte
through `rt_io_write`. A `.wram` path copies to `rt_wmadd` directly.
Pitfall: in that `rt_dma`, `.cgram` falls through into `.dmadone`; a block
inserted after it must be branched around. The first version did not, so
every CGRAM DMA also ran the WRAM copy and wiped WRAM `$0000-$01FF` (the
game's NMI pointer at `$1C` among it). File: `falcon/smkx_rt.s`
(`rt_dma`). SMK: check the boot for DMA to `$2180`.

#### Joypad presence: reads of `$4016`

After auto-read, DKC reads `$4016` once; bit 0 clear means "no standard
pad" and it clears the buttons (`CODE_80C0F8`). A shadow byte of 1 is
lost as soon as the game writes the strobe (`STZ $4016`, DKC's reset code
does), so `rt_io_read` now answers `$4016` with 1 and `$4017` with 0.
File: `falcon/smkx_rt.s`. SMK: check whether it reads `$4016` after auto-read.

#### APU: DKC's own loader protocol

DKC uploads a 40-byte loader through the IPL to `$04B8`, then sends data
through it: each byte waits for port 0 to echo the value kept in `$00`,
puts the byte on port 1 and an address on ports 2/3, and flips bit 7 of
port 0 (bit 0 set: data; clear: run at ports 2/3). The started program
reports ready with port 1 = `$FF` (`CODE_8AB204`). The echo model covers
it with one game-specific line (port 0 with bit 0 clear sets port 1 to
`$FF`), again only in state 2 of `rt_apu_write`.

### Diagnosis

#### A clobbered WRAM byte, found with a Hatari memory breakpoint

The runtime prints the host address of WRAM at start; `run_smkx.py
--parse FILE` passes Hatari debugger commands. With
`b ($<wram+$1C>).w ! ($<wram+$1C>).w :file onhit.txt` (onhit.txt:
`r` and `c`), every change of the word is reported with the 68k
registers and PC, and the run continues. That pinned the `$1C` clobber
above to one instruction in one run (F030SMK's `SMKX_WATCH` reports per
VBL; this reports the store itself). Files: `falcon/smkx_rt.s`
(`msg_wram_at`), `scripts/run_smkx.py` (`--parse`).

## From F030CT, 2026-09-24

Findings from the Chrono Trigger port (details in F030CT's
`analysis/ct.md`). Emulated runs: patched Hatari `639fadf6`, not Falcon
measurements.

### Traps crashed instead of reporting: `rt_fatal_sp` fell through — not yet in this copy

Found in F030CT when the first trap (an untranslated NMI vector) ended in
"CPU exception, vector 4" at a text offset before the program, with
garbage in a2. `rt_fatal_sp` stores the 65816 address and then *falls
through*: the exception catcher (`rt_catch_exceptions`, inserted after it
in F030SMK's copy) runs, overwrites a0-a2 and the vectors, and its `rts`
returns to the `pea`'d 65816 address as a 68k address. Every stop through
`rt_trap`, `rt_unsupported` and `rt_noreturn` therefore crashes, often
with a misleading exception. Fix: `bra rt_fatal_arg` after
`move.l (sp),rt_fault_arg`. Status here: not yet in this copy: take over `falcon/smkx_rt.s` (`rt_fatal_sp`). Affects:
SMK's traps (`rt_trap`, `rt_unsupported`, `rt_noreturn`, `rt_badtable`) all go through it.

### Flag merge dropped a live carry — not yet in this copy

`emit.py` (`emit_node`): when an instruction clobbers a flag that stays
live (usually V), the CCR is saved and merged back as "flags the
instruction produced" + "saved flags". Flags that stay live and that the
instruction leaves alone fell into neither set and came back as 0 — the
typical case is `DEX`/`DEY`/`INX`/`INY` or `INC`/`DEC` emitted with the
carry kept (`step_reg`'s address-register path) while V is live: the
65816 carry (68k X) is lost. Fix: `keep = m68(need | (through & ~clob))`.
Take over `tools/smkx/emit.py` (`emit_node`). Symptom: a carry that
vanishes across a counter update in a loop (multi-byte arithmetic around
`DEX`). In SMW, where V is live nearly everywhere, this merge runs
thousands of times.

### Interrupt entry on scratch registers: D and the data bank — not yet in this copy

The translated NMI/IRQ runs on the interrupted 68k registers. `rt_dma`
saves and restores a2-a4 but uses them as scratch while it runs with
interrupts enabled, so a VBL that lands inside it enters the NMI with
garbage in d3/a3/a4 (the data bank) — and in CT's copy a2 (D). F030CT
keeps the bank in memory (`rt_setdbr` stores `rt_dbr` first) and rebuilds
X/Y upper words, a2 and d3/a3/a4 in `rt_entry_regs` before `jsr smkx_nmi`
and `jsr smkx_irq`. Take over `falcon/smkx_rt.s` (`rt_entry_regs`,
`rt_setdbr`, the two call sites in `rt_frame`). SMK's NMI and IRQ entries start with the bank unknown, so an IRQ that lands while the main loop is in `rt_dma` (the race's VRAM uploads) reads through garbage a3/a4 until its first PLB. That is a candidate for any crash that moves between runs.

### An SPC700 interpreter that runs the game's own driver — available

CT's intro waits for the sound driver's beat on port 3 (`LDA $2143 : AND
#$0F : BEQ`), so the echo model could not continue; and CT's driver uses
port 0 as a counter, which SMK's "`$1F` returns to the boot ROM" rule
misread. F030CT now runs the driver: `src/spc700.c` (SPC700, timers,
ports, S-DSP register file, no audio), the boot ROM's upload protocol
modelled (no ROM image needed), `src/smkx_apu.c` as the runtime glue,
`make test-spc` on the host. Two lessons:

- Clock it by the 65816's work, not the Falcon's clock: 4 SPC700 cycles
  per port access (one 65816 polling iteration on the SNES), 1/60 s per
  frame. A clock-driven catch-up fell behind with every poll (a busy
  driver interprets ~6x slower than real time) and the 65816's timeouts
  (`LDX #$0400 … DEX : BNE`) stretched to minutes.
- Skip idle loops: a port read twice with no other I/O between ends the
  slice; a timer counter read as 0 twice waits for its next step. Idle
  cost: ~3,500 SPC700 instructions per second; busy (uploads): up to
  ~24,000 per frame.

SMK's echo model works because its driver only echoes; if its sound (or a port wait that isn't a plain echo) ever matters, the interpreter can take over (its `$1F` return to the boot ROM is then the real boot ROM protocol, which `spc_cpu_write` models). Files: `src/spc700.[ch]`, `src/smkx_apu.c`,
`tools/spc_test.c`, the `.apu` cases of `rt_io_read`/`rt_io_write`.

### Methods that may help (translator, all in F030CT)

- **Per-site dispatch targets.** Seeding every pointer-dispatch site with
  all address-taken labels of its bank gave CT 580,000 nodes. Sites now
  take only `$SITE: $T…` lines from `dispatch_targets.txt`;
  `scripts/ctx_seed_loop.py` runs, appends each reported miss to its
  site, rebuilds and runs again.
- **Dispatch continuations for every handler exit** (`analyze.open_exit`):
  handlers returning in other widths, bank or D no longer stop at
  `rt_badexit` when the caller restores them itself.
- **Widths from an escaped PLP.** A routine that leaves through its
  caller's frame pulls the caller's P; its widths are resolved where the
  escape lands (`escape_into`, `pesc`), and code inside it that needs them
  branches on the pulled byte (`width_split`, `rt_lastp`). Warning: a
  general four-way split at every unknown PLP took CT from 116,000 to 2.2
  million nodes.
- **Jump tables in incbin data**: read from the ROM up to the next label,
  while each word names listed code (`analyze.rom_table`).
- **Code the game decompresses into WRAM** can be translated at its WRAM
  addresses from rebuilt bytes, with an 8-byte WRAM check at each entry
  (`WRAM_OVERLAYS`, `rt_ovlcheck`).
- **Coroutines.** CT's menu switches 65816 stacks between tasks;
  `rt_setsp` gives each stack region its own 68k return stack. Relevant
  wherever a game does `TCS`/`TXS` to something other than a reset.
- The word view: for a 4 MiB ROM it is 8 MiB on top of the ROM; F030CT
  dropped it (misaligned `move.w` + `ror.w #8`), as SMW2 and DKC did.

## From F030DKC, 2026-09-24 (later): parallel sessions share one machine

Sessions of all five ports run Hatari on the same machine at the same
time. `pkill -f run_smkx` or `pkill -f hatari/build/src/hatari` kills
every port's emulator run, not only this one's (seen from F030DKC: a run
died in the middle with no trap and no frame limit), and `pgrep` on those
patterns waits for other sessions' runs. Keep the PID of your own run
(`nohup python3 scripts/run_smkx.py ... & echo $! > build/<x>/run.pid`)
and stop or wait for that PID only; `run_smkx.py` stops its own Hatari
through the command FIFO when it ends.

## Taken over in this copy, 2026-09-24 (late)

Status of the entries above in F030SMK's code (the demo race runs through
its end with these; see `analysis/translator.md`):

- F030CT `rt_fatal_sp` fall-through: taken over (`falcon/smkx_rt.s`).
- F030CT flag merge (`keep = m68(need | (through & ~clob))`): taken over
  (`tools/smkx/emit.py`).
- F030CT interrupt entry registers: taken over as `rt_entry_regs` without
  the D part (SMK keeps D = 0): a5, X/Y upper words and the data bank from
  `rt_dbr`, which `rt_setdbr` stores first (`falcon/smkx_rt.s`).
- F030SMW `all_callers` and F030DKC's return join node: taken over
  (`tools/smkx/liveness.py` is now F030DKC's copy, `TCD` in `NZ_DEFS`
  included).
- games/smw/F030DKC `rt_dma`: VRAM with any increment, pairs or one half,
  fixed or incrementing source, Mode 7 pages marked from the first to the
  last word; `$2180` WRAM; branched around, not fallen into. SMK keeps its
  own OAM block copy and CGRAM logging; those two take incrementing
  sources only (`falcon/smkx_rt.s`).
- F030SMW forced-blank frames: taken over (`src/smkx_video.c` returns 0,
  `rt_render` keeps the shown buffer, `tools/ppu_snap.c` skips them).
- F030SMW make 3.81 stamp and sub-make: taken over; the stamp also removes
  the C objects built with the switches (Makefile).
- F030DKC `$4016` reads: not needed, SMK only writes the strobe.
- F030SMW input (ACIA keyboard pad, input scripts): next.

## From F030SMW, 2026-09-24 (evening): a reference 65816; the carry bug; liveness is safe after all

F030SMW commit `17baedb`; details in its `analysis/smw.md` ("Reference
interpreter"). Emulated: patched Hatari `639fadf6`.

### Shared emitter bug: the carry was lost in flag merges — not yet in this copy, take over `tools/smkx/emit.py`

When an instruction produces a live flag while other flags stay live
across it, `emit_node` saves the CCR, runs the instruction and merges:
N/Z (produced) from after, the kept flags from before (`andi.w #save,d5`).
The 68k X bit, which holds the 65816 carry, was in neither mask, so
`move.w d5,ccr` cleared a carry that was live across the instruction. In
SMW: `ADC` / `STA` / `LDA` (N, Z live; V kept) / `PHP` ... `PLP` / `ADC`
lost the first ADC's carry; sprites stopped moving. Every port with the
shared emitter has it, and it fires wherever V (or another flag) is kept
across a load or logic instruction whose result flags are read. Fix:
`keep = m68(need) | 0x10` (X always from after the instruction: the
emitted code either defines the carry there or leaves X alone; the fused
compare case keeps its own mask).

### A reference 65816 to check the translation frame by frame — method

`tools/ref65816.c` (F030SMW) runs the ROM on an interpreted 65816 with
the runtime's hardware model mirrored register for register (map, I/O
shadows, DMA byte by byte, APU handshake, multiply/divide, IRQ chain;
interrupts as the runtime delivers them: nothing written for the frame,
the interrupted state restored whole, the I flag only changed by SEI/CLI
and REP/SEP). ~20 million instructions in seconds. To compare, the
runtime switch `SMKX_SYNC=1` skips a VBL while NMI is enabled and the
game's frame-done flag is not set (SMW: `LagFlag`), so NMIs only arrive
when the main loop waits and the state after N frames no longer depends
on speed; `SMKX_SUMS=1` logs a WRAM sum per frame (skip the stack page:
the runtime writes no interrupt frames and no return addresses for
table-dispatch routines). First differing frame -> snapshot there ->
WRAM diff -> write watch in the reference (`REF_WATCH`) -> the routine;
`tools/smkx/traces.txt` prints the 65816 S at chosen addresses on the
Falcon. Two bugs found, about an hour each; afterwards 2,961 demo frames
and 4,914 frames of scripted gameplay match exactly. The game-specific
parts are the wait address and the frame-done flag; SMK (its frame flag),
DKC (NMI at WAI) and the others need their own sync point.

### Correction: the precise flag liveness is sound — the earlier "do not port" is withdrawn

The earlier F030SMW entry called the escaped-return successors plus
PHP/PLP pairing unsafe because SMW's attract demo broke with them. The
reference interpreter showed the breakage came from the carry bug above
(and an SMW-only emitter bug), not from the liveness: with both fixed the
optimised build matches the reference frame for frame. It is now the
default in F030SMW (`SMKX_SAFE_LIVENESS=1` switches it off): CCR saves
35,043 -> 636, SMW's level scenes from half speed to close to full speed
at 50 Hz with rendering off. Take over `tools/smkx/liveness.py`
(`php_pairs`, the escaped-caller edges, `all_callers` with
`prog.dyn_callers`) together with the emit.py fix, never without it.

## From F030SMW2, 2026-09-24

Findings from the Yoshi's Island port (details in F030SMW2's
`analysis/yi.md`). YI's translated 65816 program boots, runs its SuperFX
code through a GSU interpreter, uploads its sound, takes NMIs and IRQs and
reaches game mode 7 with the main loop running (emulated, patched Hatari
`639fadf6`). None of the code below is in this copy; the F030SMW2 files
are named per entry.

### Runtime: RTI did not restore the I flag — IRQs masked after the first interrupt

**What happened.** YI's NMI and IRQ handlers start with `SEI` and end with
`RTI`. The runtime models `SEI`/`CLI` in `rt_iflag`, but nothing modelled
`RTI` restoring the interrupted code's P: after the first NMI `rt_iflag`
stayed set and `rt_frame` never ran another IRQ. YI does its frame work in
an IRQ in some NMI modes (`$011C = 8`), so the main loop waited forever on
`LDA $011B : BMI` while NMIs kept coming. **Recognised** by the PC samples
sitting in the main loop's wait and a snapshot showing the NMI mode.
**Fix** (`falcon/smkx_rt.s`, `rt_frame`): around `jsr smkx_irq` and
`jsr smkx_nmi`, push `rt_iflag`, set it (taking an interrupt sets I), and
restore it after the handler returns. **For this game:** check the NMI and
IRQ entries for `SEI` (SMW's `I_NMI` and `I_IRQ` both start with it, and
end with `PLP : RTI`, which restores the pushed P with I set by the SEI);
if the status line's IRQ count stops growing after the first NMI, this is
why.

### Translator: the analysis grew 8x from decimal-mode and bank contexts

YI's first complete analysis had 16,288 procedure contexts over 3,794
entries and 1.15M nodes (37 MB of assembly was the *reduced* result).
Measured by which context field differs between an entry's contexts:

- **Decimal flag (almost all of it).** Contexts with D unknown or set
  arose from PLPs of unknown P and from analysis over-approximation, and
  multiplied every callee. YI only sets D inside `SED ... CLD` windows
  that contain no call, PHP or PLP (checked in the listing), and the 65816
  clears D on interrupt entry, so `canon()` now forces D clear in every
  call context (a call made with D not known clear is reported), and a
  PLP of an unknown P leaves D clear. 1.15M -> 724k nodes.
- **Data bank and direct page.** Instead of a context cap, the analysis
  now runs in passes (`analyse` / `analyse_once`): after a pass, every
  entry reached with more than one data bank (or D) is analysed with it
  left to the runtime in the next pass, until nothing changes (two or
  three passes, 25 s each). 724k -> 391k nodes for 3,919 contexts, near
  the minimum of one context per entry (384k).

File: `tools/smkx/analyze.py` (`canon`, `analyse`, `analyse_once`,
`multi_bank`, `multi_dp`). **For this game:** count the SEDs and check
that no call sits between SED and CLD before taking the decimal rule;
the pass iteration applies as is (F030DKC's `CTX_CAP = 1` is the blunt
alternative; the pass version keeps a known bank where there is only one).

### Translator: control-flow idioms (all in F030SMW2's `tools/smkx/`)

- **Tables addressed below their first entry**: `JSR (DATA_x-$01,x)`,
  `-$02`, `-$04` and, with a define, `JMP (DATA_x-!Define_...,x)` hundreds
  of bytes below. Jump tables are now indexed by the byte offset X
  (`jsr ([t,d1.l*4])`, one slot per possible X), and a table the analysis
  cannot find at the operand is located through the `DATA_` label in the
  listing's operand (`table_block`, `table_targets`, `dispatch_table`).
- **Push-and-return dispatch** (`LDA tbl,y : PHA : RTS`, the game-mode
  dispatcher `PHB : ... : PHA : ... : RTL` with the bank for a later PLB):
  `tools/smkx/jumps.txt` lists each site ("stack", with an optional
  "dbr"); the analysis flows to the table's entries plus one inside the
  same procedure with the rest of the stack, the emitter reads the target
  back from the 65816 stack and looks it up (`rt_stackjump`). Pitfall:
  one path of such a site often skips the PHA (`BEQ` to the RTS); only
  states with the bytes pushed jump, the others return normally.
- **`PER ret-1 : JMP (ptr)` / `PEA ret-1 : ... : JMP (ptr)`** is a call
  (`ret_at`): the callee's RTS returns to `ret`. F030DKC has the 3-byte
  `PHK : PEA : JMP [dp]` form.
- **Pointer jumps** get explicit targets (`jumps.txt` "ptr", tables or
  `= $T1,$T2`) or targets inferred from the `DATA_` label of the `LDA`
  that loaded the pointer (`infer_ptr`, reported for review); the old
  fallback (every address-taken label in the bank) is gone: it
  multiplied contexts, and an unlisted target now stops the program with
  its address.
- **A parked return address**: `PLA : STA $00 : JSL X : LDA $00 : PHA :
  RTS`. The wrapper removes its own return address so that an escape in
  X (`PLA : PLY : RTL`) leaves the wrapper's caller too; a normal return
  from X comes back and pushes the address again. The emitter dropped the
  68k return address at the PLA and then returned from the wrong frame
  (desynchronised stacks, a wrong DBR from the next PLB). Now: an RTS/RTL
  with as many bytes pushed as were pulled below the entry is an ordinary
  return (`unpark`); the emitter parks the 68k return address in a slot at
  the PLA and pushes it back there; an escape into a caller that had
  already pulled its own frame continues upward (`escape_into` keeps
  `cst.esc`); `frame_size` also follows tail-call links and pointer
  dispatch callers (sprite routines tail-called from JSL'd dispatchers
  have 3-byte frames). YI shares SMW's code base here (its
  `CODE_008365` is SMW's `ExecutePtr` byte for byte), so SMW may well have
  the same wrapper.
- **Direct page**: YI runs its sprite code with D = `$7960` (the GSU RAM
  mirror) and sets D to `$2100`/`$420B` for register blocks. As in
  F030DKC, D is part of the state and context; unlike F030DKC, a runtime D
  is not a runtime call: `a2` (unused by the translated code before) holds
  the host address of D, kept by `rt_setdp` on TCD/PLD, so `LDA $12,x`
  with an unknown D is `move.w ($12,a2,d1.l),d0`. An I/O page gets a
  scratch page and is counted (`dpio` on the status line); where D is an
  I/O page it is known statically.

### Diagnosis: a stack-balance check at every call

`SMKX_CHECK_STACK=1 python3 -m tools.smkx ...` makes every call site
store S before pushing its return address and compare it after the return;
a difference stops with the 65816 call site (`rt_stackbad`). In YI it
found three translation errors within an hour (the parked return, a
stack-jump path, a missing D), each within a few calls of the cause,
where the symptom otherwise surfaced thousands of frames later as a wrong
bank. Cost: a few instructions per call; off by default. Files:
`tools/smkx/emit.py` (`stack_check`), `falcon/smkx_rt.s` (`rt_stackbad`).
F030SMW2 also writes `TRAP.BIN` (a snapshot) at every fatal stop and
prints S, DBR and D on the status line.

### Listing: the readme's MD5 is not the framework's

The Yoshi's Island disassembly's readme gives an MD5 for U.S. v1.0 that
the clean ROM does not have; the framework's own
`!Define_Global_BaseROMMD5Hash` (in `RomMap/ROM_Map_*.asm`) matches it,
and so do the header checksum and the byte sum. For other Yoshifanatic1
disassemblies, take the hash from the ROM map macro.

## Taken over in this copy, 2026-09-24 (night)

- F030SMW2 RTI restores the I flag: taken over (`falcon/smkx_rt.s`,
  `rt_frame`: `rt_iflag` pushed, set, restored around `smkx_irq` and
  `smkx_nmi`). SMK's handlers don't SEI, so it had not fired here.
- F030SMW carry bug in flag merges: covered by F030CT's merge rule, taken
  over earlier (`keep = m68(need | (through & ~clob))`).
- F030SMW precise flag liveness (`php_pairs`, escaped-caller edges): not
  yet; planned with the speed work, now that the carry fix is in.
- F030SMW input: in this copy (`SMKX_INPUT`). SMK's title copies the new
  presses to `$6A` only at full brightness (`CODE_8592F9`) and wants B;
  `SMKX_INPUT` with B (`8000`) every 100 frames from frame 1000 reaches
  the driver select at frame ~1600.

## From F030CT, 2026-09-24 (night): calls through PER, pointer idioms, stack switches

F030CT now runs through its intro and title into the new-game menu (Battle
Mode: Active/Wait), all on translated code. Generic findings on the way
(details in F030CT's `analysis/ct.md`; files are F030CT's):

- **`PER ret-1 : ... : JMP (ptr)` is a call.** CT's menu state machine
  pushes its return address with PER and jumps through a pointer; the
  handler's RTS returns to the PER target + 1. As a tail jump its RTS left
  the 68k frame wrong. PER now pushes its known value; a dynamic JMP with
  two known pushed bytes on top becomes a pointer call returning there
  (`analyze.ret_at`, `percall` in emit.py). Careful: returns the handlers
  add later must drop those two bytes too (`resume_after_call`), or the
  analysed stack grows by two every loop (it hit STACK_CAP here). Search
  your listings for `PER` followed by `JMP (`/`JML [`.
- **Pointer-table idiom:** `LDA table,X : STA ptr : ... : JMP (ptr)` —
  `analyze.pointer_idiom_sites` reads the table from the ROM for such
  sites, no runtime round per target.
- **Temporary stacks:** `TSX : STX save : LDX #imm : TXS ... LDX save :
  TXS` (CT's menu NMI). After the first TXS the analysis forgot the pushes
  before it, so the pulls after the restoring TXS looked like pulls from
  below the entry and the emitter dropped 68k return addresses for them.
  The state now remembers the outer stack (`State.outer`); and after a
  TCS/TXS to an unknown stack (a coroutine switch) pulls are not escapes
  (`State.switched`, no `esc_fix`). Worth checking wherever a game does
  TCS/TXS outside its reset code (DKC's NMI-at-WAI model resets instead).
- **Runtime helpers with globals and the VBL:** CT's `rt_setsp` kept its
  return address in a global; the translated NMI (from the VBL) calls it
  too, so a VBL inside it returned the main code to the NMI's call site
  (a crash at a different frame each run). Any runtime helper that keeps
  state in globals and can be reached from both the main loop and the
  translated NMI/IRQ must mask interrupts.
- The carry merge bug F030SMW describes is the one in my previous entry;
  F030CT's fix is `keep = need | (through & ~clob)` (live flags the
  instruction left alone come from after it, clobbered ones from before);
  F030SMW's `| 0x10` does the same for X as long as nothing clobbers a
  live carry.
- Sorry: I used `pkill -f run_smkx` / `pkill -f hatari/build/src/hatari`
  several times today before reading F030DKC's note, which may have ended
  other ports' runs in the middle; F030CT now stops only its own PIDs.

## From F030CT, 2026-09-24 (late night): a DSP tile renderer in eight planes; SSI interrupt pacing; booting DSP programs without XBIOS

F030CT now composes every tile-mode frame on the DSP (`falcon/dsp/ppu.asm`,
driver `src/smkx_ppu.c`), byte-exact against `ppu_render` on every
snapshot of a 6,000-frame run (intro, title, attract) and in `make
test-ppu` (six real frames incl. 100 sprites). The design differs from
F030SMK's: both work, compare before choosing. Details in F030CT
`analysis/ct.md`, "Rendering on the DSP".

- **Eight planes, CGRAM index = colour**: the Videl's 256 colours are
  CGRAM at the frame's start (brightness applied), so there is no palette
  mapping, no histogram and no slot table; 57,344 bytes a frame.
- **Composing in plane words, not pixels**: per layer and row 33 slivers of
  8 pixels hold a byte per output plane (the tile's planes, the palette
  base's bits as 00/FF) and an opaque mask per priority. A 16-pixel group
  of a plane is three sliver bytes shifted by the fine scroll, done by the
  multiplier in A0: `mpy/mac/mac` with 2^(15+f), 2^(7+f), 2^(f-1) (0 for
  f = 0), read modulo 3. Groups are composed front to back (each pass
  takes the pixels no earlier pass covered); passes without pixels on the
  row are left out. Sprites are laid into plane words.
- **VRAM as a page cache**: 256-word pages the frame reads (the planner
  lists them) go up over the host port into 77 slots, LRU; the runtime's
  dirty flags now cover all 128 pages. Frames read 22-29 pages here.
- **The SSI transmit interrupt can overrun its writer**: gating TIE on the
  ring's fill between routines is not enough when a routine (a layer's
  slivers, the sprites) runs long; the interrupt then sends stale words
  and the frame is wrong in large blocks. Fix: a long interrupt that sends
  a ring word only if its bit 0 is clear and then sets it (`jset #0,y:(r6)`
  / `movep` / `bset #0,y:(r6)+`; plane words have bits 7-0 clear, the SSI
  sends bits 23-8), turning TIE off itself at a marked word. ~20 cycles a
  word. Hatari only raises the interrupt after record took a word: send a
  frame's first word polled (as scummvm's `hsc2p`).
- **Booting DSP programs without XBIOS** (`falcon/smkx_dspboot.s`): bit 4
  of the PSG's port A resets the DSP (Hatari `psg.c`); the bootstrap then
  takes exactly 512 words into P:$0000-$01FF; a stage-2 loader in the
  program takes further blocks (space, address, count, words; space 3
  ends). Supervisor mode, fine inside the VBL handler: F030CT switches
  between its Mode 7 kernel and the tile renderer per scene this way.
  (`Dsp_ExecProg`'s loading of the P:$0010 vector was not confirmed.)
- **Planning on the 68030 costs by the row**: 224 rows of copying and
  comparing register slots were 20 ms a frame; `ppu_replay` now flags
  rows where a register or the OAM segment changed (`PpuRegs.changed`),
  and the planner looks only at those (8.6 ms). Mark a layer's pages per
  map row once per frame, not per scroll position (a wave effect rescans
  every line otherwise), and a sprite's per character row (16 characters
  of a name table are one page).
- **Pitfalls met again** (F030SMK found most first): AND leaves A2's sign
  (a ring fill computed as `(r7 - r6) & $1FF` compared as negative);
  `m7t_hex` clobbers d2; bit instructions (`jclr`/`bset` ...) take only
  X:$00-$3F or I/O absolute addresses; ASM56000 has no `ifndef`, local
  labels need a letter after `_`, and `dc` lists must not contain spaces.
- **Measured** (patched Hatari `639fadf6`, emulated): DSP 177-224 ms a
  frame (C reference ~5.2 s); SSI transport alone 76 ms (754 KB/s with
  four tracks; this Hatari derives the DSP transmit clock from the play
  track count).

Not in this copy: take over `falcon/dsp/ppu.asm`, `falcon/smkx_dspboot.s`,
`src/ppu_dsp.h`, `src/ppu_dsp_plan.c`, `src/smkx_ppu.c/.h`,
`tools/dsp_model.c/.h`, `tools/dsp_snap.c`, `falcon/pputest.s`,
`src/pputest.c`, the `snes_ppu.c/.h` changes (`ppu_replay`, `PpuRegs`,
`ppu_planar8`, SNAP.BIN version 4) and the Makefile targets if this port
wants this design; the runtime glue is in F030CT's `falcon/smkx_rt.s`
(`rt_render_sync`, `M7DIRTY`), `falcon/smkx_video.s` (`rt_vid_planes`)
and `src/smkx_video.c`.

For SMK: the race is Mode 7 plus sprites; the tile path here does not cover Mode 7 yet (F030CT keeps its four-plane Mode 7 kernel for the intro and will add Mode 7 as a second fetch path). The ring and the boot loader carry over as they are.

## From F030CT, 2026-09-25: the scroll latches survive between renders (reference bug); random frames for DSP tests

**`ppu_render` keeps the scroll registers' write-twice latches from the
last call — in every copy.** `Regs s` is static and the start of a frame
resets registers, scroll, Mode 7 values, CGRAM and `cgflip`, but not
`bgold`/`m7old`. A single-byte write to `$210D-$2114` (or a Mode 7
register) mid-frame then mixes in whatever byte the previous render left.
Found in F030CT when a random-frame generator (below) rendered each frame
twice (planner and reference): 13 of 40 frames differed from the first
write's row on. On the Falcon this makes a frame depend on the render
before it, so host checks of a snapshot can disagree although both sides
run the same code. Fix: `s.bgold = s.m7old = 0;` where the frame starts
(F030CT `src/snes_ppu.c`, `ppu_replay`). Not yet in this copy: one line
in `ppu_render` after `s.cgflip = 0;`. Games that always write the scroll
registers twice (most) never see it; HDMA tables with one-byte scroll
modes do.

**Random frames find what real ones don't** (F030CT `tools/dsp_rand.c`,
`make` builds `build/dsp_rand`): random VRAM, CGRAM, OAM (all sizes,
flips, priorities, positions over the edges), modes 0/1, BG3 on top,
16x16 tiles, map sizes, scroll and a few register writes mid-frame, kept
small enough for the DSP's page cache; each frame is planned, composed by
the reference and checked against the host model, then packed for
PPUTEST next to real frames. It found the latch bug above and two bugs in
F030CT's DSP program that no real frame had shown: a pointer register the
sprite code clobbered only for horizontally flipped sprites, and mode 0's
12 compose passes overrunning the pass list into a table. Worth copying
for any DSP or fast-path renderer: 40 random frames cost ~15 minutes in
Hatari (uploads dominate).

**F030CT's DSP renderer, speed since the last entry** (patched Hatari
`639fadf6`, emulated): sprites placed with one multiplication per plane
byte and mask merges for the constant planes (palette, plane 7 = the mask,
priorities), rows without sprites skipped: field 185 -> 171 ms, title
(100 sprites) 224 -> 205 ms a frame.

For SMK: `ppu_fast.c` keeps its own state; check whether it copies `bgold` from `ppu_render`'s Regs.


## From F030CT, 2026-09-25: SSI overlap and the eight-plane output floor

Found during a renderer-budget review in F030CT. A fresh, isolated six-scene
PPUTEST build confirmed field 171 ms, menu 164 ms, title 197-205 ms and
forced-blank output 76 ms, with zero differing words. Emulated only:
patched Hatari `v2.6.1-497-g639fadf6`, build banner 2026-09-22 21:37:14,
exact binary `../../../F030Arcade/third_party/hatari/build/src/hatari`. Log in CT:
`build/ctx/advice-baseline-stderr.log`; no random tests rerun this time.

CT already transmits through SSI while drawing the next row's BG/sprites:
`falcon/dsp/ppu.asm` `tx_isr` drains the 512-word ring independently of the
main drawing code. SSI shifts independently, but servicing its interrupts
still takes DSP cycles. Count ring-empty stops and time waiting for room
before enlarging the ring: buffering can hide bursts, not a sustained
producer deficit. Treat render plus output as an overlapping pipeline,
not two additive full-frame costs. PPUTEST's render+record excludes CPU
planning/uploads, and its separate frame-send timer also includes buffer
clearing and record setup, so it is not a host-port bandwidth benchmark.

Atari's hardware reference (Developer Support Package, Hardware p. 21,
https://www.atarimania.com/documents/Atari-Falcon030-Developer-Support-Package.pdf)
specifies a maximum 1 MB/s SSI/sound-DMA rate at the 32 MHz crossbar clock.
Thus 256x224x8bpp = 57,344 bytes needs at least 57.344 ms even at that ideal
rate; the current 25.175 MHz route has a 72.89 ms ideal floor. These are
hardware-derived bounds, not measured Falcon throughput. Full eight-plane
output below 40 ms requires a different path or less data; a faster SSI
clock alone cannot do it. Four planes halve the payload but also reduce
indexed colour capacity. Host-port output must include CPU read/store cost
in its benchmark; it is programmed I/O on the Falcon, not autonomous DMA.

Status in this copy: advisory finding; no shared renderer patch to take
over. No implementation changed in CT. The written plan is
`games/ct/analysis/dsp-renderer-budget.md`: cache decoded tile rows and layer
setup, reduce per-plane address work, profile ring starvation, and retain
byte-exact tests. CT's slivers and comp_group are already in internal P;
check the actual listing before suggesting that relocation as a speed-up.

For SMK: distinguish its four-plane output budget from CT's eight-plane one; HDMA planning, Mode 7 and game execution remain separate costs.

## From F030DKC, 2026-09-25: `ppu_snap`'s colour count wrapped; eight planes for DKC; a snapshot's mode is not the frame's mode

Found while applying `games/ct/analysis/dsp-renderer-budget.md` to DKC's
plans (F030DKC `ROADMAP.md` item 3, `analysis/dkc.md` "Rendering
budget"). Host tools only; no emulator figures of our own.

- **`tools/ppu_snap.c` overcounts "CGRAM indices shown"**: `used +=
  !seen[ref[i]]++` with `static uint8_t seen[256]` counts a colour
  again each time its byte wraps after 256 pixels, so frames printed
  248-305 "indices" (more than 256 is impossible). Recognised by
  counting from the Falcon planes in a script: 31-134. Fix: `if
  (!seen[ref[i]]) seen[ref[i]] = 1, used++;`. This copy's checkers have no such counter; nothing to take over.
- **DKC needs eight planes**: 114-134 CGRAM indices from 8-9 palettes of
  16 in intro/jungle/title frames, up to 73 on one row; logos 31-69.
  So DKC's full-frame floor is CT's: 57,344 bytes, 72.9 ms over SSI at
  the current clock (57.3 ms at 32 MHz), ~13 rendered frames/s at best.
  Dirty rectangles don't help its scrolling levels; below 40 ms depends
  on the host-port readback benchmark CT's document proposes.
- **A snapshot's `io[$105]` is not the frame's BG mode**: DKC's logo
  snapshot holds mode 3 there, but HDMA sets mode 1 for rows 0-149 and
  mode 5 below (found by printing `s->r[0x05]` per row in
  `ppu_ref_row`). `ppu_snap` prints the I/O value; read modes from the
  replayed registers before concluding a mode is in use. Modes 2-6 are
  drawn as mode 1 by the reference, the fast composer and CT's planner.

For SMK: its four planes stay a SMK decision; DKC's counts are a data point for why an eight-plane port cannot follow it.

## From F030CT, 2026-09-25: two hangs in shared register reads (RDNMI, OPVCT), B -> A multiplier DMA, WMDATA and CGRAM fast paths; the game at 50 fps without rendering

Found in F030CT while taking its game code from 10-25 to 50 fps
(rendering off, sound deferred, emulated in the patched Hatari
`639fadf6`). Details and numbers: F030CT `analysis/ct.md`, "Game speed".

**RDNMI race (hang).** CT's location change at `$C03B71` waits with
`LDA $4210 : BPL` while NMIs are enabled, and CT's field NMI reads `$4210`
first thing. `rt_frame` set `rt_rdnmi` before calling the NMI, the NMI's
read cleared it, and the poll never saw the flag: the attract demo stood
for minutes (the status line kept counting NMIs; the "io" field showed
`$4210` from the poll's call site). On the SNES the poll wins, because the
flag is set at V-blank before the NMI is taken. Fix: `st rt_rdnmi` again
after `jsr smkx_nmi` returns (`falcon/smkx_rt.s`, `rt_frame`). Check your
game for `$4210` polls in main code while NMIs are on.

**Latched V counter (hang).** CT's `BattleSys_WaitScanline` (`$CD3B82`)
latches through `$2137` and loops until `$213D` reaches a line; `$213D`
returned its shadow byte, so the frame handler never returned (VBLs
counted on, frames stood still, "frame handler still running"). Fix in
`rt_io_read`: each `$2137` read advances a modelled scanline (`rt_vline`,
wrapping at 262) into `rt_vlatch`; `$213D` reads it through a low/high
flip-flop (`rt_opvct_ff`) that a `$213F` read resets. Files:
`falcon/smkx_rt.s` (`.slhv`, `.opvct`, `.stat78`, three variables).

**B -> A DMA from the multiplier.** CT's title fills its OAM buffer with
`$E0` by setting M7A = 1, M7B = `$E0` and DMAing 512 bytes B -> A from
`$2134` into WRAM; B -> A always took the per-byte path (`rt_io_read` +
`rt_wb`, ~45% of the title's VBL time). `rt_dma .mpydma`: when every
pattern step reads `$2134-$2136` (no side effects) and the destination is
WRAM, read the product bytes once and fill; long words when they are one
byte. Title NMI 41 -> 21 ms together with the release build.

**WMDATA reads inline** in `rt_rb`'s I/O branch (`cmpi.w #$2180,d7`):
CT's sprite builder reads its lists through `$2180` with an unknown data
bank. **CGRAM DMA pair loop**: while no IRQ logs and no low byte is
latched, `rt_dma .cb` copies colour words without the per-byte flip and
logging tests (`.cbp`). Title NMI 21 -> 15 ms with both.

**Profile script**: `scripts/smkx_profile.py` named samples after absolute
equates (`SRAM_SIZE` = `$2000`, `SMKX_FRAMES`) that fall into the
runtime's addresses; in CT that hid `rt_dma` (15.7% of a profile). It now
skips map lines with " abs," (F030CT's copy).

**Method**: timing marks around the game's main loop (CT:
`Field_MainLoop` `$C00097`/`$C000BA` in `tools/smkx/marks.txt`) separate
"NMIs keep up" from "the game logic keeps up": CT's field ran its NMIs at
50/s while `Field_MainLoop` passed ~25 times per second.
`scripts/ctx_fps.py` (F030CT) lists scene changes by INIDISP/BGMODE/TM and
reports fps and per-frame milliseconds per frame range.

Also taken over into F030CT from F030SMK, no change: `rt_dma`'s direct
paths, the small frame-start copy, page tables, `SMKX_DIAG`, `rt_tc_sample`.

In this copy: page tables, the small frame-start copy, `rt_dma`'s direct paths, `rt_tc_sample`, `SMKX_DIAG`. Not yet in this copy: take over the RDNMI line in `rt_frame`, `.slhv`/`.opvct`/`.stat78` in `rt_io_read`, `.mpydma` and `.cbp` in `rt_dma`, the `.wm` branch of `rt_rb` (all `falcon/smkx_rt.s`), and the " abs," skip in `scripts/smkx_profile.py`. For SMK: its Mode 7 math may use the multiplier; look for B -> A DMA from `$2134` and for `$2137`/`$213D` reads before assuming either is unused.

## From F030DKC, 2026-09-25 (later): long,X from a low-RAM base; the OAM address reload at VBlank; the VRAM read prefetch

Found while fixing DKC's level load (Jungle Hijinxs showed only its
background: no ground, no Kongs, nothing moving), by bisecting the Falcon
against F030DKC's reference interpreter (`tools/smkx/ref65816.py`,
`refcmp.py`) with dumps at the n-th execution of a probed address.
Details: F030DKC `analysis/dkc.md`, "The level-load bug". Four
causes, three of them shared code:

- **`LDA.l DATA&$FF0000,x` read WRAM instead of ROM**: DKC's
  `CODE_BCB052` reads `$BC:0000` + X with X >= `$8000`, i.e. ROM at
  `$BC:8xxx`. `host_or_rt` in `tools/smkx/emit.py` treats any base
  below `$2000` in banks `$00-$3F`/`$80-$BF` as the low-RAM mirror
  and emits WRAM + index. Recognised from two WRAM words that held the
  routine's results, then from the generated `move.w ($2,a5,d1.l),d0`.
  Fix: long,X operands with such a base (not `$7E/$7F`) get `dp,X`'s
  range check (`cmpa.w #$2000,a0`, WRAM below, the runtime above),
  with `adda.l #bank<<16,a0` on the far path: Loc "dpx" with a `bank`
  field, a branch for `O.LONGX` in `loc()`, and the far paths in
  `read()`/`write()`. Five instructions in DKC, so no cost to speak
  of. abs,X into low RAM is unchanged (far too common to check).
- **The OAM address is reloaded at VBlank**: DKC prepares its OAM DMA
  in one frame and starts it early in the next without writing `$2102`,
  relying on the PPU reloading the OAM address from `$2102/$2103` when
  VBlank starts (unless in forced blank). Without it each frame's list
  landed `$220` bytes further on: sprites missing or scattered.
  Recognised from a snapshot whose OAM low table held no list while the
  WRAM buffer did, and a trace of `$420B` writes (OAM address `$262`,
  `$082` at the DMA). Fix: where the NMI is taken (DKC: `rt_wai`;
  the other ports: `rt_frame` before `jsr smkx_nmi`), if `rt_io+$100`
  bit 7 is clear, set `rt_oamadd` as the `.oamadd` write handler does.
- **VRAM reads return a prefetch latch**: `$2139/$213A` return a word
  latched when `$2116/$2117` is written and reloaded (from the current
  address) on the read that steps the address, so the first word after
  setting the address comes twice, which is why games read once first.
  The runtime read VRAM directly, shifting every read-back by a word
  (DKC's `CODE_B99412` copies VRAM `$6C00` to `$7400` this way).
  Fix: `rt_vprefetch` (word) and `rt_vprefetch_load` in
  `falcon/smkx_rt.s`, called from `.vmaddl`/`.vmaddh` and, before
  `rt_vram_step`, from `.vramlo`/`.vramhi`, which now return the
  latch's low/high byte.
- **The 65816 V flag**: the first divergence (inside game frame 3132)
  went away with F030SMK's `rt_vf` change and nothing else.

Status in this copy: not yet in this copy: take over `tools/smkx/emit.py`
(the long,X range check) and `falcon/smkx_rt.s` (`rt_vprefetch`, the
OAM address reload, placed in your NMI path). Changing `rt_oamadd` and
the VRAM reads alters what snapshots and dumps record from then on.

Method: the reference counts executions from reset, so a routine run for
the first time in the diverging frame is the easy checkpoint; listing the
JSR/JSL targets the reference executes after the last equal point, with
each one's execution count, gave the next probes directly.

For SMK: it writes `$2102` itself before its OAM DMA, as far as known, so the reload changes nothing there, but check with a `$420B` trace. SMK is LoROM: search the listing for `&$FF0000` with an index (`grep -E '&\$FF0000[^;]*,[xy]'`) before assuming the long,X case is absent. Your `rt_vf` change removed a real divergence in DKC's level load.

## From F030DKC, 2026-09-25 (evening): F030CT's DSP renderer in DKC; 93 page slots; `changed` only for planned registers

F030DKC took over F030CT's DSP tile renderer (CT's committed state,
0511390). Details: F030DKC `analysis/dkc.md`, "The DSP renderer".
Measured in the patched Hatari `639fadf6` (emulated only).

- **77 VRAM page slots were too few for DKC**: its level and jungle
  frames read 78-85 pages (pre-rendered backgrounds use nearly every
  tile), so every level frame fell back to the CPU. Recognised from
  `build/dsp_snap`'s "pages need" and the runtime's "ppudsp" fallback
  count (882 of ~1,600 calls). Fix in `falcon/dsp/ppu.asm`: the
  per-layer character tables (CADDR, X:$0700-$16FF, 4K words) are gone;
  the descriptor code computes a character's row-0 address as
  `PT[(chrbase + c << size) >> 8] + low byte` (`ca_table` now only sets
  the size shift, 3 or 4, in `L_CSH`), and the freed memory holds 16 more
  pages at P:$4700-$56FF: 93 slots (`SLOTS` and `slot_addr` in
  `src/smkx_ppu.c` and `src/pputest.c`). Descriptors are rebuilt only per
  8-row block or scroll, so the extra instructions are cheap. PPUTEST: 6
  DKC frames (three of them above 77 pages) and 16 random frames exact.
- **`PpuRegs.changed` only for registers the planner reads**
  (`src/snes_ppu.c`, `planned_reg`: $2100-$2101, $2105-$2114, $212C):
  DKC's levels rewrite CGRAM by HDMA on all 224 lines (a gradient), and
  every write set `changed`, so the planner re-examined every row. Found
  by counting `reg_write` calls per register in a host build (268 writes
  each to $2121/$2122).
- **Visible map columns only** (`src/ppu_dsp_plan.c`, `need_row`): 34 of
  a 64-wide map row, marked again for another first column; page marks
  per DKC level frame 9,247 -> 6,083. CT marked whole rows for wave
  effects; with such an effect this version re-marks on every row.
- **Result in DKC's first level**: 3.9 frames shown and 31 game frames per
  second (was 0.8 and ~3 with the CPU composer); planning still costs
  48.7 ms of 68030 time per frame inside the VBL handler, which the two
  planner changes above reduced only from 52.5 ms: the host profile (half
  page marking, a fifth HDMA replay) does not predict the Falcon's cost.
  Profile planning on the Falcon before optimising it further.
- Shared-code note: `tools/smkx/dsp_asm.py` (`--boot`) and
  `falcon/smkx_m7dsp.s`/`src/smkx_m7.c` (programs booted by
  `rt_dsp_boot`, no `Dsp_ExecBoot`) came along unchanged from CT.

Status in this copy: the DSP renderer itself is not in this copy (see F030CT's entries); if it is taken over, take F030DKC's `falcon/dsp/ppu.asm`, `src/smkx_ppu.c`, `src/pputest.c`, `src/ppu_dsp_plan.c` and the `planned_reg` change in `src/snes_ppu.c/.h` rather than CT's first version.

For SMK: its own DSP renderer is four planes with Mode 7; the CADDR point applies if it keeps per-character tables too.

## From F030CT, 2026-09-25 (afternoon): register replay and planning ~5x cheaper; Mode 7 frames loaded two DSP programs

Found in F030CT with the DSP tile renderer on (emulated, patched Hatari
`639fadf6`): the 68030's planning ran in the VBL handler before the NMI
and cost 10-35% of the game frames. Details: F030CT `analysis/ct.md`,
"Game speed with rendering on".

**`src/snes_ppu.c` (the reference; identical output, 56 of 56 CT
snapshots and the model checks unchanged):**
- HDMA channels stepped from event to event (`hdma_events`, `Hdma.next`):
  a non-repeat entry holds its count without a per-line walk (CT's field
  tables transfer twice per frame; the 8-channel loop on 225 lines was
  most of the replay).
- A unit equal to the channel's last one is skipped when repeating it
  changes nothing (`idem`: plain registers and whole write-twice pairs;
  off when any channel writes a write-twice register halfway, and after
  IRQ-log writes).
- `reg_write` sets `changed` only when the state a row sees changes (HDMA
  rewrites the same values every line).
- `ppu_replay_changes`: the callback only on row 0 and changed rows.
- `compose_m7_ext`: rows without sprites are filled directly (the
  per-pixel loop cost CT's intro ~0.4 s a frame).
- CT's copy was older than F030SMK's: it now has F030SMK's HDMA part
  (`aptr`, `reg_used`), with CT's own ROM mask in `ppu_aptr_hirom` (4 MiB,
  not 512 KiB: check the mask against your ROM size when taking it).

**DSP path** (CT-specific files, the method carries over): the planner
(`src/ppu_dsp_plan.c`) caches each map block's character pages and trusts
blocks of pages `rt_m7_dirty` shows clean (it is cleared after planning);
`src/smkx_video.c` sent every frame starting in Mode 7 to ppu.asm first
and then to the Mode 7 kernel: two DSP program loads per frame. Forced
blank must count as "not Mode 7" there, or loading screens take the slow
Mode 7 path.

Not yet in this copy: take over the event stepping, `idem` skip, precise `changed` and `compose_m7_ext` fast path from F030CT's `src/snes_ppu.c` (this copy has `aptr`/`reg_used` already). For SMK: its race has Mode 7 on most rows, so the replay and `compose_m7_ext` parts matter most.

## From F030CT, 2026-09-25 (later): render work in the game's idle time; a local `start` symbol shifts profiles

Found in F030CT (emulated, patched Hatari `639fadf6`, rendering on):
composing a frame (planning for the DSP renderer, or the Mode 7 path's
composition around the kernel) ran in the VBL handler before the NMI and
cost game frames (CT's intro 8.8 fps, field ~47). Now the VBL only asks
for the work and shows finished frames; the work runs in its own context:

- **Idle marks** (`tools/smkx/marks.txt`, name `idle`): at the game's
  wait-for-frame loops the emitter calls `rt_mark_idle`, which saves the
  game (all registers and SR) and starts or resumes the render context
  (`rt_render_thread` on `rt_rstack`). A VBL over the render context runs
  its frame and returns to the game (`rt_to_main`); the NMI there needs
  the game's a6 (the 65816 S), taken from the saved registers. The TOS
  VBL chain is called with a format-0 frame (`rt_chain_tos`). Without an
  idle mark, the VBL gives the work the CPU after `RWAIT` frames (still
  preempted by the next VBL).
- **Frozen inputs**: while work is pending, no frame-start copy, IRQ log
  or OAM segment is taken; VRAM stays live, so a planner that caches from
  VRAM needs its own dirty flags (CT: `rt_plan_dirty` beside
  `rt_m7_dirty`, which the upload clears).
- **Presentation at the flip** (`rt_flip`, C `smkx_render_present`):
  plane count and palette only change when the new frame is shown; a
  Mode 7 path that set them while composing showed the old frame in the
  wrong mode for the whole (now longer) composition.
- Snapshots and the frame limit wait for pending work.

Result in CT: intro 8.8 -> 50 game fps, field ~47 -> 49.5-50, menu 49.8;
the picture gets the rest (field ~3.3 pictures/s). Snapshots exact.
Files: `falcon/smkx_rt.s` (`rt_mark_idle`, `rt_to_render`, `rt_to_main`,
`rt_render_thread`, `rt_chain_tos`, `rt_flip`, `rt_render_poll`, `rt_vbl`,
`rt_frame`), `falcon/smkx_m7dsp.s` (`rt_plan_dirty`), `src/smkx_video.c`
(`smkx_render_poll/start/present`), `tools/smkx/marks.txt`.

**Profile pitfall**: a static C function named `start` put a second
`start` symbol into the link map; `scripts/smkx_pc.py` took it as the
base and every sample landed 1.6 MB off ("65% in smkx_rom"). F030CT's
copy now keeps the first `start`.

Not yet in this copy: the scheduler (files above) and the `smkx_pc.py` fix. For SMK: its race is Mode 7 on most rows; find the race loop's wait for the NMI for the idle mark, and check what the DSP-1 glue does in main context.

## From F030DKC, 2026-09-25 (later still): CT's cheaper planner and idle-time rendering in DKC; CGRAM gradients and `changed`

F030DKC took over F030CT's c2ef936 (replay and planner) and b0b8378 (render
work in idle time). Emulated in the patched Hatari `639fadf6`, Jungle
Hijinxs with Right held. Details: F030DKC `analysis/dkc.md`, "Rendering
in idle time".

- **CGRAM changes should not set `changed`** when the DSP planner is its
  only reader. CT's `reg_write` now marks a real CGRAM change; DKC's
  levels rewrite CGRAM by HDMA on every line (a gradient; 268 writes each
  to `$2121/$2122` a frame), which made every row a change. DKC keeps
  `planned_reg` ($2100-$2101, $2105-$2114, $212C) together with CT's
  compare-before-marking (`src/snes_ppu.c`).
- **Planning 48.7 -> 18.7 ms per DSP frame** with CT's block cache;
  DKC's visible-column marking is dropped (whole map rows are cheap now).
- **Idle time without a context switch in the VBL's game path**: DKC's
  runtime takes the NMI at WAI (`SMKX_NMI_AT_WAI`), so its VBL never runs
  game code. `rt_wai` runs the render work in its own context while it
  waits; a VBL over that context saves it and returns to the game in
  `rt_wai`, which takes its NMI and resumes the work at the next WAI. No
  idle marks, no RWAIT, no game frame saved by the VBL. Files:
  `falcon/smkx_rt.s` (`rt_idle_render`, `rt_to_render`, `rt_to_main`,
  `rt_render_thread`, `rt_render_poll`, `rt_flip`, the `.suspend` path
  in `rt_vbl`), `src/smkx_video.c` (`smkx_render_poll/start/present`).
- **Result**: release build 43.9 game fps (was 35.6 with planning in the
  VBL, 31 before CT's planner) and 3.9 pictures per second; snapshots
  exact. The display now waits for planning to get through the game's
  idle time (~8 ms a frame): planning the next frame while the DSP renders
  the current one (CT's `smkx_ppu_plan`/`_stale`/`_send` split) is the
  obvious next step.
- SMK's release-build fall-through: no routine in DKC's runtime has its
  whole body under `SMKX_DIAG`; the release build was run through the
  level.

Status in this copy: nothing new to take over beyond F030CT's commits,
except the CGRAM point for games with colour HDMA.

For this port: DKC's WAI shortcut works only where the runtime takes the NMI at WAI (`SMKX_NMI_AT_WAI`); otherwise follow CT's version.

## From F030CT, 2026-09-25 (evening): Mode 7 as a second build of the DSP tile renderer, eight planes, exact

F030CT renders Mode 7 frames on the DSP in eight planes (CGRAM indices)
with sprites and priorities, byte-exact against `src/snes_ppu.c`: the tile
renderer `falcon/dsp/ppu.asm` assembled a second time with `M7K=1`. Details:
F030CT `analysis/ct.md`, "Mode 7 on the DSP renderer".

- **One source, two programs**: `tools/smkx/dsp_asm.py --define NAME=VALUE`
  puts `NAME equ VALUE` before the source. The Mode 7 build leaves out the
  tile code and gets its own memory map: the Mode 7 plane flat at
  P:$1300-$52FF (VRAM words 0-$3FFF, uploaded packed by the CPU as
  `(tile*64) << 8 | texel`), sprite pages in 12 slots at P:$7400-$7FFF, the
  X data above $1300. Sprites, compose, the ring and the loader are shared
  code. The runtime boots the program a frame needs (`smkx_ppu_boot`).
- **Sampling**: `m7c2p.asm`'s loop (packed coordinates, the planner's
  INSIDE/TILE0/FILL segments from `m7_plan`) into a line buffer; the stream
  carries one entry of segments per row after the OAM segments.
- **c2p with the multiplier**: per 8-pixel sliver, `mac` over 256-entry
  tables whose bits sit at 22, 13 and 5, scaled by 2^(8-i): three planes
  land in a1[7:0], a0[22:15], a0[14:7]. Keep bit 23 of a0 clear: a0 is
  multiplied as a signed value to shift it back.
- **Pitfall**: a sprite name table can wrap past VRAM $8000 into the Mode 7
  plane; those pages need a raw copy for the sprites (the page table points
  to the last upload).
- **Checks**: PPUTEST's pack is "PPU2" (a program word per frame); random
  Mode 7 frames in `tools/dsp_rand.c` (wrap, clip to transparent, tile 0,
  flips, matrix writes mid-frame, sprites).

Files: `falcon/dsp/ppu.asm` (the `if M7K` parts), `tools/smkx/dsp_asm.py`,
`src/ppu_dsp.h`, `src/ppu_dsp_plan.c` (`m7_entry`, `sprneed`, `prefer`),
`src/smkx_ppu.c`, `src/smkx_video.c`, `tools/dsp_model.c` (`m7_strip`),
`tools/dsp_rand.c`, `tools/dsp_snap.c`, `src/pputest.c`, `falcon/pputest.s`.

Not yet in this copy. For SMK this matters most: the race is Mode 7 on most rows with the HUD in tile rows, a mixed frame, which this build does not take; the same approach (a Mode 7 program with a small tile path for the HUD rows, or the HUD rows composed separately) is the next step for an eight-plane SMK.

## From F030CT, 2026-09-25 (late evening): CORRECTION to this morning's latched V counter model; tile DSP renderer without character tables

**Correction.** This morning's entry ("two hangs in shared register
reads") gave a V counter model where every SLHV (`$2137`) latch is one
scanline later. As written it counts on across frames, and that broke
CT's field engine: `Field_ReadHVLatch` runs object scripts while the
latched line is below a budget, so whenever the counter stood above it no
script ran (characters missing from scenes, credits black). The counter
must start at line 0 each frame: `clr.w rt_vline` at the top of
`rt_frame` (`falcon/smkx_rt.s`). If you took the model over, take this
line too; if not, take both together. Check your game for code that reads
the V counter to budget work per frame: with one line per latch it gets
(at most) as many passes as the budget in lines.

**F030CT's tile DSP renderer** (`falcon/dsp/ppu.asm`) no longer keeps
per-layer character tables (CADDR, 4K words X): `desc` computes a
character's P address through the page table, and the freed memory holds
16 more VRAM pages (93 slots). Only relevant if your copy uses F030CT's
DSP renderer; a scene with three full layers plus sprites needed 80.

## From F030CT, 2026-09-25 (night): jump tables cut short by the listing; a range guard at every translated table

**What happened.** CT's attract battle stopped with a "bad exit" (a callee
returning in an unexpected context). The Hatari CPU history
(`history cpu N` over the FIFO) showed a `jsr ([table,d1.l*2])` whose X
was past the translated table: the translator had read 12 of the table's
122 entries, and the index fetched the next table's words, which named
foreign code. Two ways the translator cuts a `JSR/JMP (abs,X)` table short:

- the listing keeps the handlers (or the table) as incbin bytes, and
  `analyze.rom_table` stops at the first word that does not name listed
  code;
- the listing puts a **label inside the table** (e.g. a
  `PointerTo05..._B8C5` label on entry 5), and the dw reader in
  `table_targets` (`if ... isinstance(items[j], Label): break`) stops
  there. CT had three such tables (5 of 57, 2 of 4, 8 of 16 entries).

CT had seven short tables in all (two with incbin handlers, 12 of 122 and
2 of 29; five cut at a label or a db line).

Nothing stops at translation time; a short table shows only when the game
indexes past it, and then as some unrelated fault further on.

**Fix in F030CT** (tools only, plus one runtime stop):

- `tools/smkx/tables.txt` ("$BBAAAA count" per line, game-specific) and
  `analyze.TABLE_LENGTHS`, used by both readers (`table_targets` and
  `rom_table`).
- `scripts/table_audit.py`: lists every table read shorter than either
  the run of ROM words naming listed code, or the entries up to its lowest
  handler (all pointing near the table; this catches handlers left as
  incbin), and dumps a table with its handler names
  (`table_audit.py C1B8BB`). Most suspects just run on
  into the next table: check the next table's start, the site's bound
  (CMP/AND before ASL/TAX) or the first handler's address. Checked tables
  go into tables.txt too, so the audit comes out empty.
- `tools/smkx/emit.py` (`table_bound`, `self.table_len`): before each
  `jsr/jmp ([table,d1.l*2])`, `cmpi.l #2*n,d1; bcc.l <table>_oob`; the stub
  pushes the 65816 site and jumps to `rt_tableoob` (`falcon/smkx_rt.s`,
  xref in `falcon/smkx_rt.i`), which stops with "jump table index past the
  translated table: X $.. to the call at 65816 $..". Two instructions per
  table jump; a short table then stops where it is used.
- `rt_badexit` also prints the 68k stack (as text offsets) and the 65816
  S with its next 24 bytes.

- The Makefile's translator dependencies (`CTX_PY`) include
  `tools/smkx/tables.txt`, so changing it retranslates.

Not yet in this copy: take over `tools/smkx/emit.py` (`table_bound`,
`self.table_len`) and `falcon/smkx_rt.s`/`.i` (`rt_tableoob`,
`msg_tableoob`) first: the guard is the part that matters for every game,
two instructions per table jump. Then the length override
(`tools/smkx/analyze.py`: `TABLE_LENGTHS`, `table_lengths`, the override
in `table_targets` and `rom_table`, an empty `tools/smkx/tables.txt`, the
Makefile line) and `scripts/table_audit.py` (written for CT: the ROM path,
`listing.parse_diz`, `analyze.analyse`'s arguments and the `0xC0 |` bank of
HiROM need adapting).

For SMK: its dw reader (`table_targets`) continues across `DATA_` labels but stops at any other label, and there is no incbin reader; the guard is the cheap way to learn whether a track or menu table was cut short.

## From F030DKC, 2026-09-25 (night): checking F030SMK's and F030CT's latest findings on the reference, not by reading

F030DKC checked the bank-crossing, low-RAM-mirror and short-table findings
by instrumenting its reference interpreter at the step level
(`tools/smkx/refaudit.py`, F030DKC): before each abs,X/Y, long,X and
JMP/JSR (abs,X), it computes base, index and effective address and reports
(1) accesses whose index carries into the next bank, (2) low-RAM-mirror
bases (banks $00-$3F/$80-$BF, below $2000) whose index reaches $2000 or
more, (3) the largest index per table-jump site. Then each reported site's
translation is classified in the generated `.s` (runtime path, range
check, or a direct `(v,a5,dN.l)` access, which would be the bug).
DKC, 4700 game frames: no bank carry, 75 low-RAM-mirror sites all
translated right, 27 table sites all within their tables. For the static
side of short tables, a listing scan for labels inside the dw run behind
every `JMP/JSR (label,x)` found 13 of 115, all legitimate: parallel tables
of equal size, or data after the table read under its own label.

Worth doing in any port with a reference interpreter: an afternoon's
script, and it answers "does my game hit this?" with sites instead of
guesses. Its limit is coverage: only what the run executes.

Status in this copy: nothing to take over; the script needs your
reference's API (`Machine.step`, `read`, `x`/`y`/`dbr`/`pbr`/`pc`).

## From F030CT, 2026-09-25 (night): pictures composed over several VBLs need a frozen VRAM (sprites torn)

**What happened.** In realtime play, CT's sprites were sometimes drawn with
only some of their parts. The snapshot checks never showed it: they
compose at once, in the VBL, with no render work pending.

**Cause.** With the render work in the game's idle time (`rt_rstate`,
`rt_render_thread`), a picture's registers, CGRAM and OAM are copied at the
frame start and kept, but VRAM was read live (`in->vram = rt_vram`, the
DSP uploads from `rt_vram`). Planning and uploading span VBLs, and the
game's NMI runs in each: a game that streams sprite animation tiles into
VRAM every frame (CT does) gets OAM of frame N drawn with tiles of frame
N+k. Re-planning when the needed pages went stale does not help: it reads
the newer VRAM against the same old OAM.

**Fix in F030CT** (`falcon/smkx_rt.s`, `src/smkx_ppu.c`, `src/smkx_m7.c`,
`src/smkx_video.c`): a render copy `rt_vram_r` (64 KiB). `M7DIRTY` and
`rt_m7_mark` mark written pages only in `rt_vr_dirty`; `rt_vram_sync`
copies the marked 512-byte pages to `rt_vram_r` and marks them in
`rt_m7_dirty`/`rt_plan_dirty`. It runs where a picture's inputs are fixed:
in `rt_render_poll` when the work is asked for, and in `rt_render` (sync
renders, snapshots) when none is pending. All render reads (`in->vram`,
the page uploads, the Mode 7 kernel's upload) use `rt_vram_r`. Cost: the
pages written since the last picture, in the VBL. CT: snapshots 56 of 56
exact, the intro still 50 fps.

Not needed in this copy as long as it composes in the VBL (no `rt_rstate`). Take it over together with idle-time rendering if you adopt that.

## From F030CT, 2026-09-25 (night): runtime fast paths that took CT's battle to 50 fps (multiplier inline, MVN runs, VRAM DMA, $217F)

CT's Flea battle ran at 18-40 fps while effects played (emulated, patched
Hatari 639fadf6, release builds): single NMIs of 20-40 ms swallowed the
next VBL. All of it runs at 50 fps now. Details: F030CT `analysis/ct.md`,
"The battle at 50 fps". The shared-code parts:

- **Multiplier and divider inline** (`tools/smkx/emit.py` `MATH_OUT`,
  `DBR_MATH`, `read`/`write`; `falcon/smkx_rt.s` `rt_wrmpyb`, `rt_wrmpyw`,
  `rt_wrdivb`, `rt_d{r,w}{b,w}42xx`; `falcon/smkx_rt.i` xrefs, `rt_io`
  exported): reads of `$4214-$4217` load the register shadow, writes of
  `$4202/4/5` store it, and the writes that start an operation (`$4203`,
  `$4206`, a word at `$4202`) call a routine that keeps every register
  and the CCR. Through an unknown data bank the `rt_d...` entries test
  bit 22 of d3 (DBR << 16: a system bank) and fall back to rt_rb/rt_wb.
  Keep the X flag (the 65816 carry) untouched in such code: no ADD/SUB,
  LSx/ASx on data registers; ROR, BTST, ANDI, MOVE and ADDQ to an address
  register are fine.
- **MVN/MVP by page runs** (`rt_mvn`, `rt_mvp`): where `rt_pages_r` and
  `rt_pages_w` map both sides, copy up to the next page boundary of either
  directly, byte by byte in the same order (overlapping fills stay
  fills); I/O pages keep the per-byte path.
- **VRAM DMA long copy** (`rt_dma` `.vpair`): word increment 1,
  incrementing source and no wrap at $8000: a long-word copy instead of a
  loop per word (with rt_m7_mark as before).
- **WRAM port trick at `$217F`** (`rt_iorw/rt_ioww` `.trick`, `rt_rw/rt_ww`
  `.unm`, `rt_rw217f/rt_ww217f`, `smkx_apu_in/out` in `src/smkx_apu.c`):
  a 16-bit access at `$217F` pairs APU port 3 with WMDATA; the port half
  is a side effect, so it passes without SPC700 time.
- **Profiling**: `scripts/smkx_profile.py --pairs N` lists (runtime
  routine, calling procedure). Release builds note no caller; for a
  profile, temporarily add `move.l (sp),rt_io_caller` at the top of
  `rt_iorb/iowb/iorw/ioww` and `rt_rb/rw/wb/ww`, and lower `SAMPLES` so
  the ring covers only the stretch. Diagnostic builds (NOTE_CALLER) make
  runtime-heavy NMIs about three times slower: their frame rates mislead.
  A temporary log of NMIs over 15 ms with the frame number showed that
  runs of long NMIs, not the average, lost the frames.

Not yet in this copy: take over the files named above (the emitter parts
need the runtime entries and the xrefs together).

For SMK: the multiplier inline matters wherever the game multiplies per object or per line; check how many `$4202-$4206` sites the translation has (`grep -c rt_iowb` on `$4203`). SMK's DSP-1 does most of its maths, so the gain may be small.

## From F030CT, 2026-09-25 (night): F030SMK's SSI input in CT's tile renderer; 110,000 slots a second, so only frames with pages go by DMA

F030CT's DSP renderer (`falcon/dsp/ppu.asm`, tile and Mode 7 builds) now
takes its input by DMA playback over the SSI, F030SMK's pattern: `get`
reads the SSI while CRB's RE bit is set (`get_x`: two slots a 24-bit
word; `getw`: one slot a 16-bit word), SSION (3, host port, answers 1),
SSIOFF (9, the play window's last slots), and for CT PAGE24 ($21: the Mode
7 plane's packed 22-bit words, two slots each). Runtime:
`falcon/smkx_m7dsp.s` (Devconnect DMA play -> DSP receive in handshake
mode, Dsptristate receive on, `rt_ssi_play`/`rt_ssi_play_done`,
`rt_m7_record` keeping the play bits), `falcon/smkx_dspboot.s`
(`rt_dsp_boot` waits for a play window to end: the DSP may still be
reading it). Driver: `src/smkx_ppu.c` (`pages_due`, the SSI buffer,
`SMKX_PPU_SSI`). Exact: 56 of 56 snapshots over 14,000 frames, PPUTEST 57
of 57.

**Measured rate:** 8,192 slots in 74.6 ms at boot (~110,000 slots a second,
Hatari 639fadf6, emulated, 25.175 MHz prescale 1, four stereo tracks,
handshake), a quarter of the nominal 393,000. Hatari keeps one request as a
flag (`dsp_core_setPortCDataRegister` -> `Crossbar_DmaPlayInHandShakeMode`),
so no more than one slot can be on its way. At that rate a VRAM page costs
the DSP about what the host port costs (~2.4 ms), but a 24-bit stream
word costs two slots (~18 us) against 1-2 us over the host port.

**So per frame:** a frame that uploads pages goes by SSI (SSION, then one
buffer: pages, stream, zero padding to whole DMA frames, SSIOFF last);
one without pages goes over the host port. Everything by SSI lost pictures
where the DSP is the bottleneck (CT's Mode 7 intro); per frame, CT's tile
scenes gained (2.5 -> 3.4, 2.1 -> 2.9, 3.2 -> 4.0 pictures a second), the
whole attract run 5.06 -> 5.09. Picture rates of single stretches swing
by 20% with any change of timing (which game frame is sampled): compare
whole runs, or the DSP's own times (PPUTEST).

Also: `rt_m7_record` must keep the play bits of $FF8901 (F030SMK's note),
and a program reboot must wait for the play window, or the new program
starts with a slot of the old stream pending.

For SMK: your note gave 4096 words in about 10 ms; here the crossbar ran at about a quarter of that (8192 slots, 74.6 ms, same Devconnect settings, measured at boot with the DSP idle in `get_x`). Worth measuring in your build too: if it is the same, your 3.27 -> 2.67 pictures come from the stream's slots, and sending only page-heavy frames by DMA may recover them.

## From F030CT, 2026-09-27: main-screen windows as a third DSP program; a window census; what the DSP waits for

Found in F030CT (details: F030CT `analysis/ct.md`, "Main-screen windows"
and "What the DSP waits for"). Emulated in the patched Hatari 639fadf6.

**How CT uses windows.** `tools/win_census.c` replays each snapshot with
every register walked (new `ppu_walk_all` in `src/snes_ppu.c`) and counts
per frame the rows where windows hide a main-screen layer (TMW and a
select), clip to black (CGWSEL 7-6), colour math, sub screen, colour
window, and the distinct window edge sets. Over 200 demo snapshots: 16
with main-screen masking. They are CT's ragged-edge wipes between scenes
(window 2 over BG1-3 and sprites, its edge moved per line, 11-19 edge
sets a frame, fully closed at 0-255 for a few frames: the picture is then
only the backdrop), a BG3 reveal cut by two inverted windows ANDed (61
edge sets), and near-empty windows. No clip to black; colour math in 57.
Worth running on your snapshots before choosing what to build.

**Reference** (`src/snes_ppu.c`): F030SMK's `ppu_win_spans`,
`ppu_win_layers`, `windows_possible`, the `reg_used` change and the
masking in `compose_row` / `compose_m7_ext`, taken over unchanged.
`tools/win_test.c` checks the spans against a per-pixel evaluation
(200,000 random register sets, no mismatch).

**The DSP: a window build of the tile program instead of windows in
it.** `falcon/dsp/ppu.asm` assembled with `WIN=1` (`build/ctx/ppuw.bin`,
Makefile; the other builds get `WIN=0` and come out byte for byte as
before). The planner (`src/ppu_dsp_plan.c`) counts the rows windows cut
(`DspPlan.winrows`) and picks `DSP_KERNEL_WIN` for such tile frames (Mode
7 with windows: the CPU). Only that program's stream has four more slots,
the window registers packed (`DSP_SLOT_WSEL`..`DSP_SLOT_WLOG`,
`DSP_NSLOT_WIN`, `src/ppu_dsp.h`); the planner drops their changes for
the other programs. On a window slot change the kernel builds per layer
and 16-pixel group a keep mask (`win_build`, `win_range`, `win_layer`:
each window's range as group masks from two edge tables, inversion,
OR/AND/XOR/XNOR, TMW), compose ANDs a BG pass's mask with its layer's
(`comp_group`, a sixth pass descriptor word, `PASSW`), and after
`obj_row` the sprites' priority masks `OM` are ANDed with OBJ's
(`win_obj`). Its tables take the last two VRAM page slots (P:$5500-$56FF
= X:$1500-$16FF): 91 slots (`DSP_WIN_SLOTS`). Driver: `src/smkx_ppu.c`
boots it (`ppuw_image` in `falcon/smkx_m7dsp.s`, xdef in
`falcon/smkx_rt.s`), PPUTEST too (`pt_boot(kernel)`, `falcon/pputest.s`,
`src/pputest.c`). Host model: `tools/dsp_model.c` (`win_keep`; the frame
function takes the kernel), `tools/dsp_rand.c` (random windows every
fourth frame), `tools/dsp_snap.c` (with `-pack` the Falcon planes stored
in a snapshot are only reported).
Why a separate program: frames without windows keep the plain kernel and
pay nothing; the window frames cost 0-7% more DSP time (five real
screens: 164.6 -> 167.9, 303.4 -> 302.7, 251.2 -> 235.7, 221.8 -> 236.8,
168.7 -> 178.5 ms), plus a program switch (a few ms) where a scene
starts or ends its windows.
Checks: model = reference on 400 random frames (69 with windows) and 200
snapshots; PPUTEST 46 of 46 exact (five window screens, six random
window frames on the new program); a 12,800-frame run with snapshots
every 64 frames: 187 of 187 exact, 17 of them window screens drawn by the
DSP.

**What the DSP waits for.** A 200 Hz timer tick sorts the DSP renderer's
state (`rt_dsp_ticks` in `falcon/smkx_rt.s`, "SMKX dsp ticks" at the frame
limit; `smkx_render_phase` and the now global `smkx_inflight` in
`src/smkx_video.c`). In CT: drawing 67%; done but not yet seen by the
VBL's poll 5.7%; **12% with the next picture planned and ready, waiting
for the game to idle**: `wait_dsp` spins until the VBL sees the picture
done, the VBL returns to the game, and the render context gets the CPU
back only at the game's next idle mark. Handing the CPU to a waiting
render context after the VBL should recover most of it (not done yet).
The 68030 side: a timer profile puts CT's render work at ~8% of the
samples, 4.4% waiting for the DSP: F030SMK's instruction-cache finding
frees CPU time here but adds no pictures.

What it means for your port: your renderer already has windows (spans in
the stream, `win_mask`); nothing to take over there. The census tool and
the DSP state ticks are worth copying if you want the same numbers; if
your render context also waits for the VBL's poll and then for the
game's idle mark before sending, the same gap is likely there.

Status in this copy: not yet in this copy (`tools/win_census.c`,
`rt_dsp_ticks` / `smkx_render_phase`); the window code is your own.

## From F030CT, 2026-09-27: colour math (exact in the palette for the fixed colour, an approximation for the sub screen); snes9x as an independent reference

Found in F030CT (details: F030CT `analysis/ct.md`, "Colour math and the
sub screen" and "snes9x as the reference"). Emulated in the patched
Hatari 639fadf6.

**A census first.** `tools/ppu_census.c` counts per snapshot the rows
with BG modes 2-6, colour math with the fixed colour or with the sub
screen, mosaic, CGRAM or brightness changed mid-frame, SETINI, direct
colour. CT's demo: colour math in 59 of 200 snapshots, Mode 2 in 20,
none of the rest. Run it on your snapshots before building anything.

**The SNES's picture as a reference.** `ppu_render_rgb` in
`src/snes_ppu.c`: main and sub screen with their windows, the colour
window, clip to black, add/subtract/half with the fixed colour or the sub
screen (OBJ only with palettes 4-7; no halving where the fixed colour
stands in for a transparent sub pixel or the pixel was clipped),
brightness, as RGB. Needs the fixed colour, which COLDATA writes a
component at a time: the runtime keeps the three at `$21F0-$21F2`
(`falcon/smkx_rt.s`, `rt_io_write`'s `.coldata`), `PpuRegs.fixed` follows
IRQ/HDMA writes. `tools/ppu_rgb.c` shows a snapshot as Falcon | SNES.

**Fixed colour, exact, in the palette** (`src/ppu_dsp_plan.c`:
`math_row`, `DspPlan.mathpal`, `dsp_math_palette`; `src/smkx_ppu.c`
`smkx_ppu_palette`; `src/smkx_video.c` calls it for the palette it
loads): when every visible row applies the fixed colour alike and the
CGRAM entries it changes are drawn by no shown layer it leaves alone, the
Videl palette gets them changed. The planner walks HDMA on the colour
math registers only when colour math can change a pixel
(`math_possible`). CT's 1999 AD Day of Lavos map: exact (was bright
green, ~55,500 pixels off). No DSP change.

**Sub screen, approximated** (`ppu_sub_cfg` in `src/snes_ppu.c`, the
same in the planner, the host model and the DSP's window build): on rows
that add the sub screen, a checkerboard of main and sub pixels where the
main pixel's layer takes part; adding without halving, only sub pixels
of bright CGRAM blocks, shown from lightened copies the planner puts
into free blocks (`relocate`, `DspPlan.subreloc`, `dsp_sub_palette`); a
black backdrop shows the sub pixel exactly. The DSP composes the sub
screen as a second pass (`fx_compose`, `_mrg`, `fx_reloc` in
`falcon/dsp/ppu.asm`, WIN build): +100-170 ms a picture on such rows.
The WIN routines moved to P:$5300-$54FF (the program reached the sprite
arrays at P:$0C00); that build keeps 89 page slots.

**snes9x as an independent reference** (`tools/s9x_ref.c`,
`scripts/s9x_compare.py`, Makefile rules that fetch and build snes9x
1.63's libretro core under `build/snes9x`): runs the game headless, pairs
each snapshot with the game frame whose work RAM matches (work RAM
cleared at power-on, as the port has it: snes9x starts it otherwise),
then the frame near it closest to our reference's picture. In CT all 185
snapshots paired (median 151 of 131,072 bytes differ). Our reference
equals snes9x where the states line up, colour math and windows
included (0 pixels). What it found instead: the port's NMI-driven
animations (fades, wipes, scrolling battle backgrounds) run out of step
with its slower main loop; the closest picture lies more than 8 frames
from the work-RAM match in half the snapshots. Pairing by work RAM also
needs a wide window: the port's frame count runs ahead of the game's
(twice as fast in CT's field engine).

What it means for your port: the reference (`ppu_render_rgb`, the fixed
colour in `PpuRegs`), the runtime's COLDATA shadow, the census and the
snes9x tools carry over as they are; the palette for the fixed colour
needs your planner's equivalent of `math_row`; the sub screen's DSP part
is CT's kernel line. Your NMI-driven effects may run out of step the same
way: the snes9x pairing shows it.

Status in this copy: not yet in this copy: `src/snes_ppu.c`/`.h`
(`ppu_render_rgb`, `PpuRegs.fixed`, `math_possible`, `ppu_sub_cfg`,
`compose_sub_row`, `PpuInput.subreloc`), `falcon/smkx_rt.s` (`.coldata`),
`tools/ppu_census.c`, `tools/ppu_rgb.c`, `tools/s9x_ref.c`,
`scripts/s9x_compare.py` and the Makefile rules (`S9X`, `build/s9x_ref`).

## From F030CT, 2026-09-27: the NMI in step with the main loop (a whole attract cycle); indexed direct page on the registers; self-modifying JSR operands

Found in F030CT (details: F030CT `analysis/ct.md`, "A whole attract
cycle"). Emulated in the patched Hatari 639fadf6.

- **NMI in step with the main loop** (`SMKX_SYNC` in
  `falcon/smkx_rt.s`, on by default in CT): CT's attract demo stuck at a
  scene for 5,000 frames, then crashed. The runtime ran the game's NMI
  every VBL although the translated main loop needs more than one;
  NMI-driven counters, fades and scrolls ran ahead of the main loop's
  logic and an event's condition never came true. Found with snes9x (see
  the colour-math entry): paired by work RAM, the pictures ran up to 100
  frames out of step, and the objects' state had drifted. Now a VBL runs
  the game's IRQs and NMI only once the main loop waited since the last
  one: an idle mark (`rt_mark_idle` sets `rt_idled`) or a V-blank poll
  (`$4210` read); else a lag VBL. After 8 of them it comes anyway
  (`rt_forced`, counted on the status line). F030SMW's `SMKX_SYNC` test
  switch does the same with SMW's LagFlag; the idle marks make it
  general. In CT: 5 forced of 25,683 game frames; the whole cycle and
  into the second without a stop, the final picture equal to snes9x's
  at the work-RAM match.
- **Indexed direct page on the registers** (`tools/smkx/emit.py`, `loc`,
  note `dp-io`): code with D = `$2100` and `STA $00,x` (the register picked
  by X) stopped as unsupported; now the address is computed at run time
  through the generic helpers (`lea (base,d1.l),a0`, which keeps the CCR).
- **Self-modifying JSR operands** (`tools/smkx/analyze.py`
  `PATCHED_CALLS`, `emit.py` `call`): a JSR whose operand the game stores
  before running it (CT's WRAM overlay patches a helper's callback);
  the translation compares the stored bytes (cmpi keeps the 68k X, the
  65816 carry) and calls the translation of the listed target.
- `tools/smkx/__main__.py`: `report.txt` lists every unsupported node with
  its address, not only the counts.

What it means for your port: any game whose translated main loop needs more than a VBL per frame drifts the same way; check with snes9x's pairing whether pictures run out of step with the game state.

Status in this copy: not yet in this copy: take over `falcon/smkx_rt.s` (`SMKX_SYNC`, `rt_idled` in `rt_mark_idle` and the `$4210` read, `rt_gameframe`, `rt_lag`, `rt_forced`), `tools/smkx/emit.py` (the `dp-io` case, the patched call), `tools/smkx/analyze.py` (`PATCHED_CALLS`, empty for your game), `tools/smkx/__main__.py`. The sync needs your idle marks at the game's frame waits.


## From F030CT, 2026-09-27: switching the DSP renderer to chunky true colour (RGB565); where the planar kernel's time goes; GEMDOS 8.3 names in test runs

F030CT is replacing its eight-plane DSP output by a chunky true-colour
renderer (16-bit RGB565, the Videl's true-colour mode). Measurements that
led there (patched Hatari 639fadf6, emulated; details: F030CT
`analysis/ct.md`, "True colour"):

- **Where the planar tile kernel's time goes**, per picture on six real
  frames (field / menu / title), by the kernel's `PROF` switches: the
  whole picture 173 / 165 / 198-206 ms; with nothing drawn (output only)
  76.5 ms; BG layers ~98 / ~89 / ~83 (the slivers ~53, the descriptor
  work ~45); compose ~41 / ~59 / ~41; sprites ~5 / ~11 / ~64-72. The
  output overlaps the drawing: the kernel is compute-bound, not bound by
  the SSI.
- **The SSI output is at the 25.175 MHz ceiling already**: 57,344 bytes in
  76 ms (755 of 786 KB/s, the rate the ScummVM DSP c2p project measured
  for four slots per crossbar frame). A true-colour 256x224 picture is
  114,688 bytes: ~146 ms of output alone, and one DSP interrupt per word
  (~28 ms). The 32 MHz crossbar clock would make it ~115 ms, but Hatari's
  handshake shifts data by 2 bits there and the specification keeps
  32 MHz for the codec; only a real Falcon can say (ScummVM's
  `SSIECHO.TOS`).
- **Why chunky anyway**: exact colour. Colour math, the sub screen,
  per-line CGRAM (HDMA gradients: CT's Frog Squash), per-line brightness
  and the fixed colour become per-pixel arithmetic instead of palette
  tricks and checkerboards (see CT's colour-math entry), with the SNES
  picture (`ppu_render_rgb`) as the exact reference. Speed: estimated
  roughly equal to 20% faster pictures (the output floor, then the
  decoding of planar SNES tiles per tile row with tables, ~5-7 DSP
  instructions a pixel and layer). The Videl's scan-out and the DMA's
  writes double on the ST-RAM bus, which the 68030 shares.
- **Test pitfall: GEMDOS 8.3 names.** Hatari's GEMDOS drive maps names to
  8.3: `PPUTEST_7.TOS`, `PPUTEST_broken.TOS` and the like all became
  `PPUTEST_.TOS`, so every run started the same program and a whole
  round of measurements was wrong (found when a zeroed DSP image still
  passed). Give each test build a short unique name or its own folder.

What it means for your port: the numbers apply to any port with CT's or
SMK's DSP kernel line; whether chunky pays off for you depends on how much
colour math and how many gradients your game uses (CT's census tools
count them). CT will report the chunky kernel's measured speed and
exactness when it runs; until then nothing to take over.

Status in this copy: nothing to take over yet (measurements and a plan).

## From F030CT, 2026-09-27 (later): correction: chunky true colour measured slower; CT stays planar

Correction to F030CT's entry "switching the DSP renderer to chunky true
colour" above: CT measured the chunky core before switching, and it
stays with the planar kernel.

- **Measured** (`falcon/dsp/tcbench.asm`, `src/tcbench.c`,
  `make test-tcbench` in F030CT; patched Hatari 639fadf6, emulated): three
  BG layers (4, 4, 2 bits) of 33 tile rows, 224 rows, then RGB565 per pixel
  through the ring and the SSI into DMA record. Per tile row: the plane
  bytes, four L: (X:Y) table lookups giving all eight pixels as 6-bit
  fields, each field by the multiplier into a key table (depth and
  palette, 0 transparent) and a max into the row's line. **220 ms
  computing, 258 ms with the output**, without sprites, windows or colour
  math. CT's planar kernel draws whole pictures in 165-206 ms.
- **Why**: SNES tiles are planar; the 56001 does a bitplane operation on
  16 pixels at once, while chunky work costs ~20 cycles a pixel and layer
  (table decode, field extraction, key lookup, depth max). The output
  (114,688 bytes, ~146 ms at 25.175 MHz) comes on top.
- **Exact colour** stays possible as a separate DSP program for the frames
  that need it (colour math, per-line CGRAM), at a slower picture rate
  there, or through the palette and checkerboard approximations (CT's
  colour-math entry).
- **If you need true colour on the Videl anyway**: F030Arcade's Snow Bros
  has a 256x224 true-colour set tested on a real Falcon (RGB 50 Hz:
  `$8282` `$00C70076`, `$8286` `$003F001E`, `$828A` `$007600AB`, `$82C0`
  `$0185`, `$82C2` 0, `$8266` `$0100`, `$8210` `$0100`; VGA `$00FC00A9`,
  `$002502F3`, `$00A800C0`, `$82C0` `$0182`, `$82C2` 5). Hand-derived
  timings looked right in Hatari but mirrored the right edge on a real
  monitor.

Status in this copy: nothing to take over (a measurement); the benchmark
is in F030CT if you want to time your own variant.

## From F030CT, 2026-09-27 (later still): the chunky core with parallel moves

Addendum to the correction above: with parallel moves CT's chunky core
(`falcon/dsp/tcbench.asm`) comes to **188 ms computing, 233 ms with the
output** (from 220 / 258): a progressive shift (`mpy x0,y0,b` by 2^5
leaves the pixel in B1 and the remaining fields shifted up in B0, one
constant, no mask), the line buffer in Y loaded in parallel with the
multiply, the rest copied back in the N register's gap; ~8 cycles a
pixel. Further scheduling might reach ~145 ms for the core; with sprites,
windows, colour math and the output a whole renderer would land at about
the planar kernel's speed, not above it. CT stays planar. Pitfall on the
way: on the Falcon DSP Y:a is P:a below $4000, so a line buffer at
Y:$0200 overwrote the program (the benchmark hung).

Status in this copy: nothing to take over.

## From F030CT, 2026-09-27 (evening): a 65816 routine replaced by native 68030 code, checked against the translation; Hatari's CPU profiler; how far preparing tile data goes

Found in F030CT while measuring where its game time goes and making its LZ
decompressor native (details: F030CT `analysis/ct.md`, "The decompressor
natively; how far preparing data goes"). All figures are emulated in the
patched Hatari `639fadf6`.

- **Native routines in the translation.** `tools/smkx/natives.txt` lists
  "entry RTL name". At the entry, `emit.native_hook` emits the following in
  every context:

  ```
  pea <stub>
  pea <M/X/D bits PHP would push>.w
  jsr rt_native_<name>
  ```

  The stub does the context's own RTL: the exit context of the routine's RTL
  node with the caller's widths. The native code either does the whole
  routine and jumps to the stub, or declines and returns, and the
  translation runs. Both paths leave the CCR alone.

  To be exact, the native code must leave everything the 65816 leaves:
  memory it writes, I/O state (CT: the WMDATA address `rt_wmadd`), the
  direct-page scratch, and the bytes the pushes leave below S. A write that
  could hit the routine's own variables is a case to decline, not to
  model.
- **Check mode** (`CTX_LZ_CHECK=1`, `falcon/ct_lz.s`). Run the native
  routine, keep WRAM and the port, restore the state before, let the
  translation run, and compare all 128 KiB at a mark on its RTL.
  Two pitfalls:
  - Mask interrupts during the native pass. Otherwise NMIs inside it look
    like differences in game counters (CT: 9 VBLs inside a 34 KB
    decompression in the check build).
  - Count calls with an NMI or IRQ in the translated pass as not
    comparable.

  One difference is expected: flags that the translation keeps dead where
  PHP pushes them (CT: the C bit, which its PHD at `$C30558` takes from its
  own SUB.L).

  CT's result: of about 340 calls over the attract cycle, 311 identical,
  28 differing only in the dead C bit, and 9 interrupted. Speed: 11.99 s
  (4.3%) down to 1.37 s (0.5%) of 281.6 s, about 10 times faster per call.
- **Hatari's CPU profiler** when the game's own sampler cannot be used
  (F030CT's `CTX_PROFILE=1` build crashes; the release build does not):
  - Run with `--parse init.ini`, holding `profile on` and
    `b VBL = N :once :quiet :file save.ini`.
  - `save.ini` holds `profile save hprof.txt` and `quit`.
  - A `profile save` through `--cmd-fifo` while the emulation runs writes
    only the header; the data is processed when the debugger is entered.
  - Keep the paths relative, because the debugger cuts long lines.
  - Map the rows to 65816 procedures with `CTX.map` and `scripts/smkx_pc.py`
    (text base: the header's PROGRAM_TEXT).
- **Preparing tile data for the DSP renderer.** The bound is that
  F030CT's kernel with the slivers skipped (`PROF=8`) takes 120 / 103 /
  157-165 ms on the field / menu / title frames, against 173 / 165 /
  198-206 ms as is. Output alone (`PROF=7`) is 76.5 ms for 8 planes.
  Expanded tiles do not fit in DSP memory, so real preparation reaches less
  than that bound. With tile rows prepared for a chunky true-colour core,
  CT's `tcbench.asm` took 160 / 205 ms, and 113 / 161 ms with a ready value
  per pixel (which does not fit). The RGB565 output alone is about 146 ms a
  picture.

Status in this copy: not yet in this copy. For native routines take over
F030CT `tools/smkx/natives.txt`, `tools/smkx/emit.py` (`native_hook` and
its call in `emit_proc`, the `natives` argument of `emit`),
`tools/smkx/__main__.py` (`load_natives`) and the Makefile's `CTX_LZ`
switches. Model a check mode on `falcon/ct_lz.s`. The routine itself is
CT's.

What it means for your port: look for per-byte loops through the runtime
in your game's profile: decompressors, block copies through WMDATA,
VRAM-upload builders. Each byte of such a loop is a runtime call in the
translation, so a native version of the whole routine is worth about ten
times on that code.

## From F030CT, 2026-09-27 (late evening): BG modes 2-6 were drawn as mode 1 in the shared reference; offset-per-tile on the DSP

Found in F030CT: the census found mode 2 on its title, and snes9x
differed there. Details: F030CT `analysis/ct.md`, "BG modes 2-6;
offset-per-tile on the DSP". Checked in the patched Hatari `639fadf6`,
emulated.

- **The bug (shared `src/snes_ppu.c`).** `compose_row`, `rgb_row` and
  `compose_sub_row` drew every mode from 1 to 6 as mode 1: 4, 4 and 2 bits
  per pixel, BG3 shown, mode 1's priorities, no offset-per-tile.
- **The SNES** (snes9x 1.63 `gfx.cpp`, RenderScreen and
  DrawBackgroundOffset):
  - Layer formats: mode 2 is BG1 4 + BG2 4, mode 3 is 8 + 4, mode 4 is
    8 + 2, mode 5 is 4 + 2, mode 6 is 4. BG3 exists only in modes 0 and 1.
  - Priorities, modes 2-6, front to back: OBJ 3, BG1 high, OBJ 2, BG2 high,
    OBJ 1, BG1 low, OBJ 0, BG2 low.
  - Offset-per-tile (modes 2, 4, 6): BG3's map gives BG1 (bit 13) and BG2
    (bit 14) a scroll per tile column, except the leftmost column. Modes 2
    and 6 read an H word and a V word 8 lines below it; mode 4 reads one
    word, with bit 15 choosing V.
- **The fix.** `bg_layers`/`mode_bpp`, the new order in `compose_row` and
  `front`, `opt_word` in `bg_line`, and `ppu_opt_slivers` (in
  `snes_ppu.h`) for the DSP.
- **Result in CT.** The reference now equals snes9x on all 20 mode 2
  title snapshots (0 pixels, from 800-2,700). Most of the old difference
  was colour math credited to the wrong layer.
- **DSP (F030CT only so far).** Modes 3-6 go to the CPU. Mode 2 frames
  carry one table per frame: 66 words at the end of both tile programs'
  streams. The kernel's `desc_opt` builds the slivers per row with their
  own scroll and steps those that stay in their 8-row block; the pass list
  is `TPL2`. Changed files: `src/ppu_dsp_plan.c`, `src/ppu_dsp.h`,
  `tools/dsp_model.c`, `falcon/dsp/ppu.asm`.
- **Checks.** Model = reference on 188 snapshots, PPUTEST 64/64, and 199/199
  Falcon pictures of a demo run equal the reference. Title pictures take
  267-282 ms, where the inexact version took 198-206 ms.

Status in this copy: not yet in this copy. Take over `src/snes_ppu.c` and
`src/snes_ppu.h` (the reference). If your DSP renderer is CT's line, also
take over `src/ppu_dsp_plan.c`, `src/ppu_dsp.h`, `tools/dsp_model.c` and
`falcon/dsp/ppu.asm`. The stream format changes: the tile programs'
streams end with a count word.

What it means for your port: run CT's `tools/ppu_census.c` (its `m2-6`
column) over your snapshots. Any mode 2-6 row was drawn wrongly: in
priority order at least, and with offset-per-tile flattened in modes
2, 4 and 6.

## From F030CT, 2026-09-27 (night): a true-colour DSP renderer, exact colour math, CTX_TC=1

Found in F030CT: the palette path could not show some colour math exactly
(the title's BG1 with math, BG2 without, sharing CGRAM entries). A chunky
renderer that converts tiles once, when their page goes to the DSP, turned
out fast enough, where the earlier measurement had tiles decoded per frame
(F030CT `analysis/ct.md`, "True colour with pre-converted tiles"). Checked
in the patched Hatari `639fadf6`, emulated.

- **What it is.** `falcon/dsp/tc.asm` draws RGB565 rows into DMA record for
  the Videl's 16-bit mode.
  - It covers every BG mode, sprites, both screens with windows, colour
    math per pixel, brightness, and CGRAM written mid-frame.
  - `--define TC7=1` builds its Mode 7 program.
  - The host side: `src/ppu_tc.h` / `src/ppu_tc.c` (the tile formats),
    `src/smkx_tc.c` (the runtime driver), `DspPlan.tc` in
    `src/ppu_dsp_plan.c`, and `ppu_render_565` in `src/snes_ppu.c` (the
    reference).
  - Tests: `tools/tc_model.c`, `tc_snap.c`, `tc_rand.c`, `tc_pack.c`,
    `src/tctest.c`, `falcon/tctest.s`.
  - The Makefile targets are `check-tc`, `test-tc` and `CTX_TC`.
- **Exactness.**
  - The host model matches `ppu_render_565` on 199 snapshots and 400
    random frames.
  - TCTEST on the DSP: 51 of 51 frames exact.
  - In the game, 13 of 13 snapshots are exact, and no frame fell back to
    the CPU.
- **Speed (emulated, per picture).**
  - Simple frames 154 ms, of which the RGB565 output alone is about
    146 ms.
  - Tile frames with colour math 165-545 ms.
  - Mode 7 267-390 ms.
  - Over CT's intro it drew 3.7 pictures a second, against the planar
    renderer's 5.7.
- **Findings useful anyway.**
  - Sub-screen colour math per pixel costs about 21 DSP instructions
    through three tables: a pixel is S = main E | $80 + the sub's E term
    (subtract and half already applied), then three channel lookups.
    Before that it took about 100 instructions.
  - A Mode 7 plane word packed as the character's P address << 8 | texel
    saves a table: the map row and column come from one multiply each, so
    Mode 7 sampling needs 2K words of tables instead of 4K.
  - The screen buffers double to 114,688 bytes, and snapshots with planes =
    16 carry RGB565: `ppu_snap_frame` in `src/snes_ppu.h`, and six host
    tools read it.
- **In this copy:** not yet. To take it over, copy the files above and the
  `CTX_TC` parts of `Makefile`, `falcon/smkx_video.s`,
  `falcon/smkx_m7dsp.s`, `falcon/smkx_rt.s` and `src/smkx_video.c`.
- **For Super Mario Kart:** a race screen mixes Mode 7 rows with tile rows, and neither true-colour program takes such a frame (`dsp_plan` fails, so the CPU draws it). That is the same limit as the planar Mode 7 build, so it only pays for whole-Mode-7 screens and for colour math in the menus.

## From F030CT, 2026-09-28: true colour follow-up (bands, planner speed)

Follow-up to the entry "a true-colour DSP renderer" (F030CT `cd053a5`;
`analysis/ct.md`, same section). Emulated in the patched Hatari `639fadf6`.

- **Frames too big for the program's 74 slots.** 2 of 211 CT snapshots
  need more; the CPU fallback (`ppu_render_565`) then took about a minute
  of emulated time per frame, in the time the game leaves it. Such frames
  now go in up to 4 bands of rows: the DSP's BAND command (`tc.asm`),
  `DspPlan.band0/1` in `ppu_dsp_plan.c`, and the loop in `smkx_tc.c`.
  Check it with `tc_snap -bands 2`.
- **Planner speed on the 68030.** `need_span_tc` took 506,000 cycles per
  call, and HDMA writing a register every line made every row a span. It
  is now per map row with a column mask, over runs of rows per layer.
  `PpuRegs.cgwrites` (`snes_ppu.c/.h`) spares the CGRAM compare, and
  `math_row` is skipped for true colour. Planning went from 184 to 109 ms
  per picture.
- **How it was found.** Hatari's CPU profiler, saved from a breakpoint:
  `--parse init.ini` with `profile on` and
  `b VBL = 10000 :once :quiet :file save.ini`; `save.ini` holds
  `profile save hprof.txt` and `quit`. Map the PCs through the link map.
- **In this copy:** not yet. Take over `falcon/dsp/tc.asm`,
  `src/ppu_dsp_plan.c`, `src/ppu_dsp.h`, `src/smkx_tc.c`, `src/smkx_ppu.h`,
  `src/snes_ppu.c/.h`, `falcon/smkx_rt.s` (the 12th status counter) and
  `tools/tc_*` along with the first entry's files. The `cgwrites` field
  harms nothing on its own.


## From F030CT, 2026-09-28: true-colour hot loops are external; a 2x budget is not a buffering gain

Source/listing review in F030CT, recorded in
`games/ct/analysis/tc-speedup-advice.md`; no renderer patch or new timing run.
CT's current `falcon/dsp/tc.asm` already converts tiles on TPAGE and has
one-screen direct paths and colour tables. Its existing listings put TC7
`m7_inside` at P:$0532, `colour` at P:$0933, and tile `px_dmax` at P:$0A6F:
all outside internal P:$0000-$01FF. Startup and setup occupy internal P.

Proposed first experiment: reclaim internal P for the hot loop bodies,
keeping bootstrap and SSI valid. External P fetches compete with external
X/Y data on the same bus (Motorola family manual section 8.2; also the
patched Hatari's `dsp56k_execute_instruction`). Relocation is not itself
an established 2x gain. Larger candidates: window-boundary spans instead
of a whole row through generic `pixel`; decode once into both MAIN/SUB
instead of LK plus two merges; use SOLID in direct loops. Window spans
will not accelerate a frame already using the fast sub-screen path.

The user-reported CT Mode 7 profile was 353 ms with 32 ms in transmit
service: eliminating service entirely yields only about 1.10x; with it
unchanged, a 176.5 ms target needs other work about 2.22x faster. These
are budget calculations from emulated figures, not new measurements or
physical Falcon results. Reported environment: patched Hatari
`v2.6.1-497-g639fadf6`, build 2026-09-22 21:37:14, binary SHA-256
`b3279c6de6893a666e8e3c87b65eb4cf69f10f296a098b39272f7757731f1f7d`.
Use the renderer and output format's own baseline; CT RGB565's output
floor does not apply to four/eight-plane payloads.

Status in this copy: CT's `falcon/dsp/tc.asm` is not present; this is
advisory only, no fix to take over. Files changed for this review:
F030CT's `analysis/tc-speedup-advice.md` and the other ports' notes only.
For SMK: earlier notes already place important sampling/c2p loops in internal P. Check actual addresses before applying this; CT's placement finding is not evidence that SMK needs the same relocation.


## From F030CT, 2026-09-28: measured true-colour optimisation, 1.12x over the real test set

Follow-up to the internal-P advice. Frozen source review and A/B test in
F030CT: baseline `cd053a5`, candidate `falcon/dsp/tc.asm` SHA-256
`906904dc6d46a237409132aeb6d4e2c59c2399c8f346cae4f41ed02f42e28e26`.
The patch copies hot code over the completed bootstrap loader, keeps SSI
resident, adds F4 SOLID/fresh-line stores, and groups layer-window merges
by WC runs (WSEG). Internal-P end: tile $01E5, TC7 $00DC; size guards.

Same frozen TCTEST inputs, fresh separate builds: 27 real frames average
303.15 -> 270.43 ms (1.121x), nine field cases 405.53 -> 337.53 (1.201x),
four Mode 7 cases 268.65 -> 239.50 (1.122x). Best field 545.70 -> 360.83
(1.512x). Render+record only, excluding upload/conversion and planning;
no matched live-game FPS result. Each build passed 51 frames and 17
banded reruns with zero differing words. Twelve recent candidate game
snapshots also matched the reference. Source stayed fixed and rebuilt
candidate binaries matched the existing ones.

Emulated only, patched Hatari `v2.6.1-497-g639fadf6`, build
2026-09-22 21:37:14, binary SHA-256
`b3279c6de6893a666e8e3c87b65eb4cf69f10f296a098b39272f7757731f1f7d`.
The combined patch has not reached 2x; individual changes were not timed
separately. WSEG speeds layer-window merges; colour-window math still
uses the general per-pixel routine. Two-screen layers still use LK and
separate MAIN/SUB merges. These remain candidates, with fresh profiling.

Changed implementation: F030CT `falcon/dsp/tc.asm` (uncommitted at review).
Review/evidence: `games/ct/analysis/tc-speedup-review.md`, untracked frozen
artifacts in `games/ct/build/tc-review-rdfz7t19/`. This review itself changed
only its report and the other ports' notes. Status in this copy: not yet in this copy; take over/adapt F030CT `falcon/dsp/tc.asm` with its previously documented true-colour prerequisites. No code was copied by this review.
For SMK: several of its existing hot loops are already internal. The measured CT gain must not be transferred to SMK or its different output payload.


## From F030CT, 2026-09-29: translator inline paths, dispatch cache and model validation

F030CT reviewed and implemented another shared-translator speedup in commit
`9b6566d`. This work was checked on a Musashi 68030 model through
`machine68k`, not on Hatari and not on a physical Falcon; there is therefore
no Hatari build identity or Falcon throughput claim here. The model's menu
scene changed translated code plus helpers from 17,485 to 9,832 cycles per
NMI (about 1.8x), and the whole frame from 27,956 to 20,198 cycles (about
1.38x). Generated text grew from 1.69 to 2.10 MiB. These are relative model
figures only; check the patched Hatari before using them for SMK.

The changes and the reason for them:

- `tools/smkx/emit.py` now emits inline page-table reads and writes for
  operands whose address or data bank analysis is unknown. `rt_pages_r/w`
  handles pointers, indexed pointers and bank wrap; I/O pages and a word
  straddling a page use the old helper. An unknown-bank access that might be
  a hardware register first tests the bank class, so WRAM/ROM does not get
  mistaken for a register.
- Shadow-only register accesses use one store, with the `$21xx` IRQ-log test;
  `$2180` uses dedicated `rt_r2180`/`rt_w2180` paths. `PLB` is inline through
  `rt_dbr`, `rt_lotab` and `rt_hitab`. `CLC:ADC` and `SEC:SBC` fold to a plain
  add/sub when control-flow and flag liveness prove it safe. RMW sequences
  omit dead flag work and the final byte swap when N/Z are dead.
- `falcon/smkx_rt.s`/`.i` provide the page tables, register/shadow state and
  the 1,024-slot direct-mapped `rt_dispatch` cache. Cache entries are written
  atomically with one `movem`; keep the diagnostic history in the binary
  search slow path. The existing cache in this copy should be compared with
  CT's key/slot handling rather than replaced blindly.
- `tools/emit_bench.py` and `tools/emit_tests.py` exercise real emitted
  sequences against a 65816 reference for every live N/Z/C/V subset, including
  page straddles, bank wrap, register guards, IRQ logging, PLB and dispatch.
  `tools/ctx_run.py` runs the linked image from boot on the model. The checks
  reached 60,000+ sequence cases and 20,000 NMIs without a stall; visible
  static VRAM/CGRAM/OAM matched, but WRAM is timing-dependent and was not a
  valid strict A/B checksum.

The opt-ins `SMKX_PHA_CONST=1` and `SMKX_DISPATCH_BANK=1` propagate known data
banks, but are off by default: the measured scene gained little while code
and continuation data grew substantially, and the dispatch-bank variant was
not runtime-tested. Do not enable them without a size and Hatari check.

Evidence and changed files in F030CT: `analysis/translator-speedup.md`,
`Makefile`, `tools/smkx/emit.py`, `falcon/smkx_rt.s`, `falcon/smkx_rt.i`,
`tools/smkx/analyze.py`, `tools/smkx/listing.py`, `tools/emit_bench.py`,
`tools/emit_tests.py` and `tools/ctx_run.py`.

Status in this copy: partial. `tools/smkx/emit.py` already has page-table
read inlining, and `falcon/smkx_rt.s` already has page tables and a dispatch
cache. It does not yet have CT's write-side `paged_*` path, unknown-bank
register guard, plain shadow stores, inline PLB, folded carry/RMW changes, or
the `emit_bench.py`/`emit_tests.py`/`ctx_run.py` harnesses. Take over/adapt
`tools/smkx/emit.py`, `falcon/smkx_rt.s`, `falcon/smkx_rt.i`,
`tools/smkx/analyze.py`, `tools/smkx/listing.py` and the three `tools/*.py`
files, preserving SMK's existing direct/low paths and dispatch history.

For Super Mario Kart, count generic `rt_wb`/`rt_rw` sites by register and
operand before changing the emitter. The race has its own DSP/runtime costs,
so CT's menu result is not a promise of the same gain; validate with the
patched Hatari and record that build identity beside any measured throughput.

## From F030CT, 2026-09-29: true-colour DSP benchmark and hard limits

F030CT's true-colour kernel series (`7ad2dfe..4eb8670`, benchmark harness
`6c80c24`, report `cd2c916`) was measured on 246 attract-demo snapshots; 222
tile frames were comparable. Every version was byte-exact against
`ppu_render_565`; the final version also passed 56 random frames, all 246
snapshots and 400 random host checks. These are emulated timings from patched
Hatari `3eea7b0` (`AnimaInCorpore/hatari` HEAD, real-clock DSP), not Falcon
measurements: render+record mean fell 302.8 -> 228.8 ms (1.32x), with up to
2.25x on fixed-colour-gradient frames. The benchmark excludes upload,
conversion and CPU planning, so it is not a game-FPS result.

The limiting result is important: 28% of tile frames were already at a
154-156 ms output floor. Sending 57,344 words over SSI costs about 146 ms in
theoretical wire time and about 41 ms/frame in the transmit ISR. Therefore a
2x average needs a different payload or transport; more kernel tuning cannot
provide it. CT's CPU planning was about 109 ms/picture and becomes the next
limit near the output floor. This floor does not apply automatically to SMK's
four/eight-plane payload, so do not transfer the 1.32x number to the race.

The DSP findings that transfer to this port are:

- The 512-word internal program is full (hot code ended at P:$01F7). Add
  per-build guards: CT keeps the tile program below P:$1000 and TC7 below
  P:$0C00. DSP P memory is one 32K space; Y is P:$0000+, unused X space is
  P:$4000+, and growth can overwrite external tables. Move cold code only
  after checking the load image and all table addresses.
- `do label` repeats the instructions before `label`; putting the label on the
  final instruction silently runs one instruction short. Memory `jclr`,
  `do x:addr` and AGU `(Rn+Nn)` forms have short-address/offset limits; assert
  or inspect placement when adding fast paths.
- Hatari DSP profiling must be enabled at the render entry and saved at a
  later debugger breakpoint: each debugger command resets `dspprofile`, and
  profiling from boot includes the loader, which reuses hot PCs and waits on
  the host port. The repeatable `scripts/tc_bench` flow requires no timeout,
  passing code-space guards and zero reference differences.

The exact CT kernel is not a drop-in SMK fix. Status in this copy: not yet in
this copy; take over only the placement guards, profiling method and any
algorithmic idea after checking SMK's planar payload and existing internal-P
layout. CT files changed: `falcon/dsp/tc.asm`, `src/ppu_tc.h`,
`src/smkx_tc.c`, `src/tctest.c`, `tools/tc_rand.c` and `scripts/tc_bench/*`.


## From F030CT, 2026-09-29: the CPU side of the picture rate, a planner benchmark, and a partial renderer that did not pay

F030CT `cd2c916..637fdcd` (25 commits, `analysis/dsp-2x-investigation.md`
"The CPU side" and after; `scripts/tc_bench/README.md`). All figures are
emulated, patched Hatari `3eea7b0` (real-clock DSP), not a Falcon, and not
comparable with the older `639fadf6` figures in other notes.

- **A `plan` counter in wall time is not CPU time.** The planner runs as a
  thread in the game's idle time, so a wall-clock counter includes the
  game's NMI and main loop. CT's "planning 109 ms a picture" was 38 ms of CPU
  (a `profile on` between a breakpoint pair on the plan entry, 100 pictures).
  Check which one a counter measures before planning work on it.
- **Profile methods that held up.** (1) A deterministic 68030 benchmark
  (`src/planbench.c`, `falcon/planbench.s`, `scripts/tc_bench/planbench.py`):
  plan stored real snapshots the way the runtime does and sum the Hatari CPU
  profile by routine. Profiling "picture N of the running game" does not
  compare builds: the pictures fall on other game frames when timing moves.
  (2) `build/tc_snap ... -planhash` hashes the plan's whole output per frame;
  it must not change with a planner change (identical on 2,530 snapshots).
  (3) Runs of one binary are identical; runs of different builds differ up
  to 30% in end-to-end picture rate from the first thousand frames on. Compare
  a build's own counters, not its picture rate, to a few percent.
- **Cheaper planning did not raise the picture rate** (3.03 -> 3.09/s): the
  runtime asks for the next plan so that it ends when the DSP is done, so the
  DSP frame sets the rate; the gain is CPU time for the game. Page needs as
  bits (one mark per distinct page, cached bits for a whole 32-entry map block),
  a `replay` that skips lines without events, and long-wise copies took
  27.9 -> 15.2 ms of CPU a picture. A per-tile loop must fit the 68030's
  256-byte instruction cache: the old one missed on nearly every instruction.
- **A spin on a buffer in the planner cost 25-51 ms of CPU a picture**,
  visible only as self time at the call of the wait routine; double-buffering
  the pre-built SSI window fixed it (`pre_build`/`SMKX_PPU_PRE`).
- **Starved stretches were the game's own code.** Two of them: a loading
  transition (33 ms of game work per NMI, nothing above 7%), and CT's sound
  upload routine polling `$2140` for the SPC's echo, giving up after $400
  polls and restarting (7,520 polls in 100 NMIs). Half the CPU went to the
  SPC/timer/I-O emulation there and the render thread ran only when the
  game idled. Recognised by a CPU profile of 100 NMIs, then the `marks.txt`
  idle marks do not cover such a loop. Cure: an idle mark in the poll, or
  fixing the handshake answer (not found yet in CT).
- **Partial rendering (only changed rows) is exact but not worth it** for the
  rate: a host test found 89% of the rows of a changed picture still need
  drawing, and skips fall in static scenes. Built anyway (`src/ppu_rows.c`,
  `dsp_rows`); 0 differences in 151 checked pictures, 3.09 -> 3.35 pictures/s
  (partly run-to-run noise). The exactness method is reusable for any row or
  span skip: check builds that also draw the picture whole and compare
  (`CTX_PARTIAL_CHECK=2`), plus host fuzzing of single VRAM/CGRAM/OAM bytes.
  The CPU reference renders from live HDMA tables at run time and the DSP
  from the plan at plan time, so a whole-CPU comparison differs in a few
  pictures with and without the change; compare DSP with DSP.
- **Shared-file changes** (take over when useful, none is required):
  `src/snes_ppu.c`/`.h` `replay`: long-wise start copies (`copy4`, `cgram`
  aligned to 4) and a skip over lines without register/OAM/HDMA events when
  the caller does not want per-line callbacks (`every == 0`); plan output
  unchanged. `falcon/smkx_rt.s`: `rt_vram_sync` marks a VRAM page changed
  only when its content differs (one compare while copying); `rt_flip_c`,
  `rt_dump` (debug file writer), `rt_screen` exported.
  Not yet in this copy: take over `src/snes_ppu.c`, `src/snes_ppu.h`
  (`replay` part) and the `rt_vram_sync` compare from `falcon/smkx_rt.s`;
  update any unit tests that hash the plan.

For Super Mario Kart: the Mode 7 kernel and the race frame are on the DSP, so the wall-clock versus CPU distinction and the profile method are what carries over; the row and page-bit work is tile-planner specific. Check your plan counter before quoting a CPU cost.


## From F030CT, 2026-09-29: SPC700 interpreter, struct order and a cached timer check

F030CT `94dc9a6..359f0df` (`analysis/dsp-2x-investigation.md`, "The SPC700
interpreter's struct order"; `scripts/tc_bench/spcrate.py`). Emulated cycles in
patched Hatari `3eea7b0`, from CPU profiles of 60 pictures of the running game;
not a Falcon.

- **Field order is a 68030 cost.** `Spc` began with the 64 KiB RAM, so every
  field the interpreter touches (`pc`, `cycles`, `psw`, ...) sat at a 32-bit
  displacement: the step counter's `addq.l #1` cost 33 cycles, 22.8 with the state
  first and the RAM at offset 58 (16-bit displacements). `spc_run` plus
  `timers` fell 108 -> 100 cycles per SPC700 instruction (7%). This applies to
  any big-array-first struct in C that the 68030 touches in a hot loop.
- **`timers()` inline against a cached earliest step** (`tick_due`, refreshed
  by `due()` when a step or the enable bits change): most calls find nothing due,
  and a real call saves nine registers first.
- **Method:** `scripts/tc_bench/spcrate.py` divides the cycles of `spc_run` and
  `timers` by the executions of the step counter's increment (one per SPC700
  instruction). `tools/spc_diff.c` is a differential test of the interpreter
  against the previous one; `make test-spc` prints the same rate on the host.
- The interpreter is still about 100 cycles an instruction in the dispatch; a
  larger gain needs a threaded dispatch or registers in locals.

Changed in F030CT: `src/spc700.h`, `src/spc700.c`, `tools/spc_diff.c`,
`tools/spc_test.c`, `scripts/tc_bench/spcrate.py`.

Status in this copy: this port has no SPC700 interpreter yet, so there is
nothing to take over now. When one is added from F030CT, take these files as
they are, not the earlier version.

For this game: sound is not built here yet; the cost matters once it is, since
in CT the SPC700 and timer emulation took the largest share of the CPU in the
busy stretches.

## From F030FFVI, 2026-09-29: a seventh port, F030FFVI

`../ff6` ports *Final Fantasy VI* (Super Famicom, Japanese v1.0, CRC32
`45EF5AC8`; HiROM FastROM, 3 MiB image, 8 KiB SRAM, no coprocessor) from
everything8215's ca65 disassembly (`third_party/ff6`, pinned at 8130132).
It was set up from F030CT's code (`359f0df`, all shared fixes to that day)
and has no translation yet. `AGENTS.md` here now lists seven ports: send
findings to F030FFVI's `analysis/porting-notes.md` as well. Its listing is
ca65 source, not an address-annotated export: placement has to come from
the assembler's listings and the linker's map of an out-of-tree build
(`make rip` writes into the module directories, so not in the submodule),
with every instruction still decoded from the ROM and checked against the
source mnemonic. Its vectors are `JML $001500` / `JML $001504`, the same
WRAM trampolines as F030CT's, and its cutscene program runs from WRAM at
`$7E5000` after an LZ decompression, like F030CT's intro.
For SMK: nothing specific; fixes to the shared code that you make later are worth naming for F030FFVI as for the others.

## From F030FFVI, 2026-09-29: jump tables longer than 64 words were cut; `.addr 0` table entries; per-site lists for `JSR (table,X)`

Found while translating Final Fantasy VI (three changes to shared code in
F030FFVI's `tools/smkx/analyze.py`, one report line in `__main__.py`; not
yet in this copy: take over those files' hunks).

- **`pointer_idiom_sites` stopped after 64 targets.** The site `LDA
  table,X : STA ptr : ... : JMP (ptr)` takes the table's words from the ROM
  while they name listed code, and at most 64. FF6's event-command table is
  203 words (`EventCmdTbl`, one `.repeat` of `.addr`), so 139 handlers were
  never translated and the field's event engine would have stopped at the
  first command past the cap. Now, when the listing has the table as word
  items (a run of `dw` / `.addr` items up to the next label), all of it
  is taken; the byte scan with its cap stays for tables kept as incbin.
  Symptom to recognise: the listed-instruction coverage in `report.txt`
  stops well short of 100% with whole families of `EventCmd_xx`-like
  handlers missing (FF6: 89.1% -> 96.1% from this fix alone).
- **A table's null entry is a target.** FF6 tables start with `.addr 0`
  ("none") that the loop tests for before it dispatches. As a target, word 0
  is the start of the bank, which is code (bank $EE begins with the world
  map's code), so the analysis added a bogus callee that returns in other
  widths, and the caller's `CPX #8` was decoded as two bytes: a BRK trap and
  a width finding. `JSR/JMP (abs,X)` sites now take a per-site list from
  `dispatch_targets.txt` (`$SITE: $T1 $T2 ...`) in place of the table when
  there is one, as `JMP (ptr)` sites always could.
- **Table words that name no listed code** (`table_targets`) are now left
  out and listed in `report.txt` (FF6: two) instead of being decoded from
  wherever they point.

For this game: nothing to do unless your listing has a table over 64 words
behind a `LDA table,X : STA dp : JMP (dp)` (check the coverage figure), or a
`.dw 0` / `.dw $0000` first entry.

## From F030FFVI, 2026-09-30: TDC and PHD overwrote the carry when D is unknown; Hatari debugger recipes for memory stomps; SPC700 time per poll

First runs of Final Fantasy VI (boot to the opening scene, host render equal
to the Falcon planes). One bug in shared code, two runtime changes and a
method.

- **`TDC` and `PHD` with an unknown direct page lost the 65816 carry**
  (`tools/smkx/emit.py`, `op`): they computed D as `move.l a2,d7 : sub.l
  a5,d7`, and SUB sets the 68k X flag, where the translator keeps the carry
  (it declared only N/Z/V written). FF6 builds a 24-bit pointer with `LDA
  ptr16 : ADC #base : STA $F3 : shorta0 : LDA bank : ADC #^base`, and
  `shorta0` is `TDC : SEP #$20`: the carry out of the 16-bit add never
  reached the bank add, the map loader decompressed from bank $DC instead
  of $DD, ran off the end of the block and overwrote low WRAM (DP included).
  Now `movea.l a2,a1 : suba.l a5,a1 : move.l a1,d7` (SUBA touches no flag).
  `TSC`/`TSX` still use SUB but declare ALL flags clobbered, which is
  correct. Not yet in this copy: take over the TDC and PHD hunks. Symptom:
  a pointer's bank byte one too low after a 16-bit add and a TDC (or PHD) in
  between; a game that loops or corrupts memory a few hundred frames in.
- **Finding a memory stomp with Hatari's debugger**, without touching the
  runtime: `scripts/hatari_break.py OFFSET "! 0"` puts a conditional
  breakpoint on a byte of the 65816's WRAM (`b (addr).b ! 0`: the debugger's
  comparison operators are `=`, `!`, `<`, `>`; `!=` is a syntax error), `hi
  cpu 64` keeps the last 64 host PCs, and the script prints them, the
  registers and the DP with the host address of WRAM, which the runtime now
  prints at start (`SMKX: WRAM at $...`). `scripts/hatari_calls.py ADDR
  --dump OFF:LEN --count N` breaks at each entry of a translated 65816
  address and dumps WRAM (FF6: the arguments of every `Decompress` call,
  which showed source $DC4AB9 where the table gave $DD4AB9). `b VBL > N`
  stops after N VBLs. Per-frame snapshots (`SNAP_EVERY=1`) changed the
  timing enough to hide the bug: a stomp that depends on speed needs the
  debugger. Both scripts are in F030FFVI's `scripts/`.
- **SPC700 time per port poll** (`src/smkx_apu.c`): with the SPC700 running
  only when the 65816 polls (`SMKX_APU_FRAME=0`, 4 cycles per read), a
  driver wait of 16 ms per step of its echo delay (FF6's `WaitEchoBuf`,
  counted by timer 2) took minutes. A read that returns what the last read
  of the same port did, with no write between, now doubles the SPC700
  time up to 4096 cycles. Not yet in this copy: take over `smkx_apu_read`.
- **`SMKX_APU_ECHO=1`** (F030FFVI `FF6X_APU_ECHO`): once the game's driver
  would start (IPL upload done), no SPC700 runs and every port write is
  echoed on its port; `$FF`/`$F0` on ports 0/1 resets to the boot state.
  FF6 loads the title song through its driver's per-byte handshake and the
  interpreted driver cost minutes; with the echo the title loop starts at frame
  ~780. For first runs and for games whose sound waits only for equality
  with what they wrote.
- Idle marks (`tools/smkx/marks.txt`, `idle`) at the frame-wait loops took
  FF6's NMI rate from 1 per 10 VBLs (the forced-NMI fallback) to one per
  pass: look for the game's `LDA flag : BNE/BEQ` after `CLI` or a flag store.

For this game: nothing to do beyond the TDC/PHD hunks unless your D is
unknown at a `TDC : ... ADC` pair (DKC tracks D; check `PC..._D????`
contexts in `report.txt`).

## From F030CT, 2026-09-30: `SMKX_SNAP_FROM`, a window of consecutive snapshots; Hatari without a cross-GCC

Found while capturing real consecutive frames for CT's list-compositor model
(`analysis/list-compositor.md` there). `SMKX_SNAP_EVERY=1` alone writes a
295 KB snapshot for every frame from frame 0, which is gigabytes for a scene
thousands of frames in, and snapshot frames also lag the game (the existing
"Snapshot builds lag" entry), so the run before the scene should not
snapshot at all.

Change in shared code, `falcon/smkx_rt.s` (two small hunks): a switch
`SMKX_SNAP_FROM` (default 0, no change to any build that does not set it):
next to the `SMKX_SNAP_EVERY` default add

    ifnd	SMKX_SNAP_FROM
    SMKX_SNAP_FROM	equ	0	; ... from this frame on
    endif

and in `.render`, after the `divu.w #SMKX_SNAP_EVERY,d7 / swap d7 / tst.w d7 /
bne.s .nosnap` test and before `st rt_snapdue`:

    ifne	SMKX_SNAP_FROM
    cmp.l	#SMKX_SNAP_FROM,rt_frames
    blo.s	.nosnap
    endif

`SMKX_FRAMES=n` then ends the run. CT used `SNAP_EVERY=1 SNAP_FROM=6900
FRAMES=7060` and got 157 consecutive `Fnnnnnnn.BIN`.

Not yet in this copy, and this copy differs: its snapshot test in `falcon/smkx_rt.s` is an older layout (the snapshot is taken inline, `divu.w #SMKX_SNAP_EVERY,d7` twice), so add the test `cmp.l #SMKX_SNAP_FROM,rt_frames / blo.s .nosnap` after the `bne.s .nosnap` by hand. (The
Makefile passes `-DSMKX_SNAP_FROM=...` the same way as `-DSMKX_SNAP_EVERY`;
F030CT's Makefile does not have a variable for it yet either, CT assembled
the runtime by hand).

No cross-GCC on the machine? The C objects of an earlier build are plain ELF
objects: reassemble only `rt.o` with vasm (same `-D` switches as the
`flags.stamp` of that build) and relink with vlink over the existing `.o`
files (`vlink -b ataritos -tos-flags 1 -o X.TOS rt.o <the others>`), then
run it from `AUTO\GAME.PRG` (stage the program there; TMP/TEMP set to the normal local Temp, directory as
the trailing argument, `--debug-except none`, `--disable-video on` is fine).
Hatari does not exit after `SMKX_FRAMES`: `Program termination` appears in the
`--trace event` file; stop it by its own PID. Hatari there: the F030Arcade build-ucrt64 build,
v2.6.1-devel Sep 9 2026 17:04:10, `--fast-forward true`: CT's attract run
reached frame 5,900 in about 30 s.

For FF6: nothing game-specific in the switch itself.

## From F030CT, 2026-09-30: list compositor, a second render path (host model, blitter player in Hatari); snapshot windows; eight-plane traps

Found in F030CT, commit `fc09a13`. Full record: F030CT `analysis/list-compositor.md`
(model in `tools/lc_model.c`, player `falcon/lcplay.s`). Emulator and host model only;
every cost in it is an assumed unit cost, not a Falcon measurement. Hatari: patched
F030Arcade build, `v2.6.1-devel (Sep 9 2026)`, Falcon, TOS 4.04, `--dsp emu`, `--cpu-exact`.

**What it is.** The exact DSP renderer (165-205 ms a frame against a 76 ms SSI floor)
stays as reference, fallback and background kernel. Next to it: a retained frame
buffer per Videl page. The DSP keeps what each page shows, finds the dirty 16-pixel
plane groups (clean background changed, or a sprite/overlay piece appeared, vanished
or changed), and sends a list of hundreds of bytes to a few KB. The 030 restores
dirty groups from a clean copy and draws the pieces with blitter rows clipped to them.
Pieces are disjoint per pixel, so the order does not matter. The list is small enough
for the host port and leaves the SSI to audio. The Videl scrolls over a ring
("virtual screen") so a scroll costs only the exposed column or row.

**What was shown (F030CT's attract run, Chrono Trigger; bit-exact against `ppu_render`):**

- 0 wrong pixels, planar bytes or Videl words on 97 modellable snapshots, 95 scene
  changes, synthetic motion, 60,000 random `blitfuzz` pieces and four real windows of
  consecutive frames (150 to 433 frames each). Mutation builds (skew off by one,
  FXSR/NFSR dropped, endmask 3 all ones, old footprints left out) fail by thousands of
  pixels, so the checks bite. **Real frames alone would not have caught a missing FXSR**
  (a run starting after its piece's first group is rare in them): keep a random
  generator test (`blitfuzz`) beside the real frames.
- The outdoor scene (433 frames, scrolling, about a dozen sprites) recorded as a stream
  of memory operations and executed by `falcon/lcplay.s` on the 030 and Hatari's
  blitter: 0 checksum mismatches, at 50 fps (`scripts/run_lcplay.sh check`). A stream
  with one flipped skew gives bad frames from that frame on, so the check bites. Says
  nothing about Falcon time: Hatari's blitter and 030 caches are not a Falcon's.
- Where the work goes: the exact renderer produces about 1,660 background groups a frame
  in the outdoor scene; with the hardware scroll, 17 exposed groups. What is left is
  sprites and their cleanup (133 blitter rows, 171 restored groups a frame), 513 bytes
  of packed list a frame. Half of the frames change nothing at all (217 of 433).
- **Standing set: a pixel count is the wrong cost.** Which layers stay on the hardware
  (scroll rigidly together) is a cost choice. The opaque-pixel rule came out 3 times
  worse than the best fixed set on real windows because it flips when the layers'
  scrolls agree and differ; a set committed on a running average of the saving is
  within 5 to 10 %. A dense fixed layer's pixels mostly equal what the layers behind
  show, so its overlay pieces are few; count runs per owner, not one mask.
- Coverage: of CT's 199 snapshots 97 are modellable; skipped are 42 forced blank, 31
  mode 2/7, 20 colour math, 5 TM varying per line, 4 windows. Those frames stay on the
  exact renderer. Do a census before believing it applies to a game (`lc_model census`).
- A layer *behind* the standing one works as an overlay too (paint into the clean
  ring's holes); no layer order is excluded, it is a cost question.

**Hardware and emulator facts, each learned the hard way:**

- `clr.l $ffff8264.w` also clears `$ffff8266`, the plane count: the picture then shows
  as four planes, in bands. HSCROLL is the byte at `$ffff8265`: write it with a byte move, not a long.
- Eight planes with HSCROLL: Hatari fetches one more group a line (as on the STE) and
  the line offset has to account for it to keep the ring's row stride (the player uses
  128 words, 120 with HSCROLL, for a 32-group ring). The real Falcon's behaviour is **not verified**.
- The 030's data cache has to be off while the blitter writes (or flush): the blitter
  writes behind it.
- Hatari `--avirecord` takes a boolean (`--avirecord true`); without it Hatari opens a
  usage window and blocks the run.
- Hatari's blitter single-word NFSR case: widen the run by one group to its left with a
  zero mask (read-modify-write, unchanged).

**Snapshot windows** (`SMKX_SNAP_FROM`, `falcon/smkx_rt.s`): the hunk and how to run
without a cross-GCC are in the separate F030CT entry of the same date on
`SMKX_SNAP_FROM`. The lag that entry refers to is now measured: a build that
snapshots every frame lags, so the frame numbers of a 64-frame series sit about 700
frames earlier than the same scene in a run that does not snapshot before its window
(CT: the 25-sprite three-layer scene near frame 11,520 in the series, near 12,200
without). Find a scene with a few probe runs, then capture its window.
`scripts/capture_window.sh` and `scripts/run_lcplay.sh` are MSYS2/Windows scripts
(`build-ucrt64` Hatari, `powershell` to stop their own PID); a Mac copy has to be rewritten.

Not in F030CT either yet: a DSP-side implementation (visibility and BG-priority
occlusion, per-page state, list format, host-port transfer), colour math and windows in
the compositor, modes 2/7, eight-plane HSCROLL on a real Falcon, measured unit costs.

For this game: Mode 7 (the track) is outside the model, as in CT's skipped mode 2/7 snapshots, so the path could only serve the non-Mode-7 screens (menus, results); the Mode 7 DSP path stays as it is. Run `lc_model census` on SMK snapshots before investing.


## From F030DKC, 2026-09-30: list compositor checked on a third game; running CT's model on another port's snapshots

Found in F030DKC (Jungle Hijinxs, release build; patched F030Arcade Hatari `v2.6.1-devel (Sep 9 2026)`,
Falcon, `--dsp emu`, `--cpu-exact`). Host model only, colour math and other effects ignored on purpose.
Full record: F030DKC `analysis/list-compositor-check.md`; tools in `tools/lc_check/`.

- **CT's `lc_model`/`ppu_census` run on any port's snapshots without that port's PPU** if built
  against **CT's** `src/snes_ppu.c` and the snapshot is converted to CT's layout (SRAM padded to
  8 KiB, a `planes` u32 = 8 after the SRAM, two empty `PAL_SNAP_BYTES` blocks after the frame:
  `games/dkc/tools/lc_check/dkc_to_ct_snap.py`). The port's own `snes_ppu.c` may be an older copy
  (DKC's has no colour math, windows or `ppu_walk_all`) and then does not build them.
- CT's index renderer ignores colour math; `lc_model` skips such frames anyway. To ask "how does the
  list scheme do on the unshaded picture", remove `else if (rows.math) why = "colour math";` in a
  scratch copy (two sites). DKC's jungle has colour math on every row, so the stock census skips all
  of it (0 of 153 modellable) and the number means nothing about the scheme.
- **The hardware-scroll runs (`hreal`, `hsim`) refuse a level whose scroll comes from HDMA**
  ("scroll varies per line", "tables that cannot be patched"). DKC's BG3 has a per-line x scroll
  (HDMA channel 3 on `$2111`). Check which layers vary per line (`ppu_replay`, `r->bgofs`) before
  expecting those runs to work; the model has no per-line-scroll overlay layer.
- **Measure what each layer supplies to the final picture before choosing the standing set.** Render
  the frame with TM and TS both set to one layer (`io[0x12C]` and `io[0x12D]` in a copy; the renderer
  draws `TM | TS`, so setting TM alone changes nothing) against a TM = TS = 0 render, and count
  pixels equal to the final frame. DKC: BG1 74-82 %, BG2 11 %, BG3 < 2 %, OBJ 2.5 %, although BG2
  and BG3 are opaque almost everywhere. The layer with the most opaque pixels is not always the
  one that matters; here the rigid foreground is also the one that shows.
- Hash estimate on 111 real consecutive pairs (BG1 on the ring, every other pixel an overlay,
  groups world-aligned, dirty = hash changed): mean 440 of 4,032 groups (10.9 %), median 158, p90
  909, max 2,608, 39 of 111 updates change nothing. Not a bit-exact ring model.
- A probe that calls `ppu_render` with the census's `ident` map gets all zeros: `ppu_census.c` never
  fills `ident` (it only needs `ppu_replay`); `lc_model` does (`ident[i] = i`).
- zsh does not split an unquoted `$VAR` into words: pass a file list to these tools as an array
  (`F=(${(f)"$(...)"})`, `"$F[@]"`), or it is one file name ("File name too long").
- Shared code: `SMKX_SNAP_FROM` is now in F030DKC's `falcon/smkx_rt.s` and Makefile
  (`DKCX_SNAP_FROM`), as in CT's note above. Also measured again: a build that snapshots every
  frame lags (DKC: a 195-frame window from frame 4,400 took tens of minutes of wall time, not timed).

For this game: see `analysis/list-compositor-check.md` in F030DKC for what another platformer
with parallax looks like; side-scrollers with one dominant rigid layer are the favourable shape.


## From F030SMW, 2026-09-30: CT's list-compositor model on SMW snapshots: its colour-math skip is too coarse for backdrop-only math; LoROM bus; windows are what is left

Found in F030SMW with a scratch copy of F030CT's `tools/lc_model.c` (commit `fc09a13`), built
against CT's `src/snes_ppu.c`, on SMW's 10 snapshots (`build/smwx/F0*.BIN`, 500 frames apart,
converted with `games/dkc/tools/lc_check/dkc_to_ct_snap.py`). Host model only: no Hatari run, no
Falcon timing. SMW's own `src/snes_ppu.c` is an older copy and cannot drive the model.

**Everything was skipped as "colour math".** `row_fn` counts a row as colour math when
`((CGWSEL >> 4) & 3) != 3 && (CGADSUB & 0x3F)` (`lc_model.c:221`). SMW runs with `CGWSEL=$02`,
`CGADSUB=$20`, `TS=` one BG layer in every frame: math on the backdrop only, the standard way a
game lets a layer show through where the main screen is empty. CT's `ppu_render` composes exactly
that (`ppu_sub_cfg` with bit `0x20000`: backdrop add, black backdrop, the sub pixel is the index),
so the index frame the model compares against is right, and skipping it loses nothing.

- **Fix (scratch, F030SMW):** count a row as math only if it is not (a) no sub screen
  (`!(CGWSEL & 2)`), no halving (`!(CGADSUB & 0x40)`) and a fixed colour of 0 (math against black
  changes no pixel), and not (b) `ppu_sub_cfg(s, cgram) & 0x20000`. The same exception is needed
  for the separate "sub screen" skip (`rows.sub`, `lc_model.c:224`/`1831`), or the frames fall
  through to that reason. Both places, `frame_build` and the `hreal` builder.
- **Result:** 4 of SMW's 10 snapshots (the title screen and three levels) are modelled: `seq`
  0 wrong pixels, 0 wrong planar bytes; `blitfuzz` 0 pages wrong; `hsim` from one of them
  (200 updates, synthetic motion, ring) 0 wrong Videl words. Cold-page figures only, since every
  snapshot is a scene change: 55 to 180 pieces and about 2 to 4.5 KB of packed list a frame.
  Nothing about steady-state cost.
- **Empty colour window:** with WOBJSEL bits 7-4 all zero the colour window is empty, so
  CGWSEL mode 1 ("inside") means no math and mode 2 ("outside") means math everywhere, i.e. mode 0.
  Written and not exercised: no SMW frame has an empty colour window.

**The other six are real windowed frames**, not a coarse test: WOBJSEL `$22`/`$23` (colour window 1
enabled), W12SEL `$22`/`$33`, CGWSEL `$12`/`$22`. Windows and colour-window math are where SMW
levels end up; CT's model does not do either. Note that `ppu_sub_cfg` returns 0 when CGWSEL bits
7-4 are non-zero, so on such rows CT's reference renderer, as far as I read the code, does not
put the sub screen into the index frame at all. The model is exact against that reference, which
is not necessarily what the SNES shows. Not checked against a picture.

**ROM map.** `lc_model.c:196` passes `ppu_abus_hirom`/`ppu_aptr_hirom` unconditionally, and CT's
`snes_ppu.c` has no LoROM pair. A LoROM game reads ROM-sourced HDMA tables from the wrong bytes.
The scratch copy added `abus_lorom`/`aptr_lorom` in `lc_model.c` (SMW's `ppu_abus_lorom`,
`games/smw/src/snes_ppu.c:29`, as a base: ROM at `((bank & 0x7F) << 15) | (off & 0x7FFF)`,
mask `0x7FFFF` for 512 KiB; SRAM at banks `$70+`; the pointer version's run ends at the 32 KiB
bank edge) behind an environment variable. SMW's tables mostly come from WRAM, so nothing showed
in these frames; it matters for any game that keeps HDMA tables in ROM.

**State in the copies.** Only F030CT has `tools/lc_model.c`. Not yet in this copy: take over
the two skip rules and, for a LoROM game, the bus pair. F030DKC's `tools/lc_check/` builds against
CT's copy and inherits both points. No other port has the model.

**Not done:** consecutive frames of an SMW level (scroll, sprite motion: where the compositor
pays off; needs `SMKX_SNAP_FROM` windows), the Hatari blitter player on SMW data, a check of the
modelled frames against SMW's own composed picture (its palette path differs from CT's 1:1
CGRAM indices, so CT's `ppu_snap` does not compare).

For this game: SMK is HiROM, so the bus is right as it is; its races are Mode 7 and the model skips those anyway. Menus and title screens are mode 1 and may set backdrop math: run `lc_model census` on them and see which reason it gives before believing anything.


## From F030SMW, 2026-09-30: CT's translator speedup ported into an older runtime; what had to change, what it gave; two runtime findings (ROM writes in `.sysw`, empty dispatch-cache keys)

Found in F030SMW while porting F030CT's commit `9b6566d` (`analysis/translator-speedup.md` there)
by hand. Changed files in F030SMW: `tools/smkx/emit.py`, `falcon/smkx_rt.s`, `falcon/smkx_rt.i`.
Emulator only: the patched F030Arcade Hatari `v2.6.1-devel (Sep 22 2026)`, Falcon, TOS 4.04,
`--cpu-exact`, `--dsp emu`, fast-forward; nothing on a physical Falcon.

**Why it did not apply as a patch.** `git apply --reject` of CT's diff failed on 14 of 26 hunks
(`emit.py`, the runtime) and, worse, left hunks half applied: the dispatch cache looked slots
up but never filled them and jumped to an undefined `.missp`. Look for `.rej` files and read
every hunk. The deeper reason: CT's emitted code needs `rt_pages_r`/`rt_pages_w` (one host
pointer per 256-byte page of the 24-bit space, 0 for I/O), and this runtime had only the older
`rt_resolve` (`rt_banksys`, `rt_lotab`, `rt_hitab`) and no `rt_dbr`, no `self.marks`/`natives`,
a simpler `Loc`. What was done instead:

- **Page tables built at start** from the runtime's own resolver (`rt_build_pages`): `rt_pages_r[p]`
  is what `rt_resolve` returns for the page's first byte; `rt_pages_w[p]` is the same where `rt_wb`
  stores (WRAM, SRAM) and `rt_trash` (256 bytes) where it drops the write (ROM and its mirrors).
  512 KB of BSS; derive them from the resolver so the inline path and the helper cannot disagree.
- **Emitter:** `paged_read/paged_write` for the pointer, indexed-pointer and unknown-bank kinds,
  the `btst #22,d3` guard for a register reached through an unknown bank, direct shadow stores and
  loads for the registers whose handler is only the shadow (`PLAIN21/42/43`, `SHADOW_R`), `$2180`
  through `rt_w2180/rt_r2180`, inline `PLB`, folded `CLC:ADC` / `SEC:SBC`, RMW without dead swaps
  and tests, each with its `SMKX_EMIT_OFF` switch. The `rt_io` shadow is exported for it.
- **Not ported:** the opt-in data-bank tracking in `analyze.py`/`listing.py` (`SMKX_PHA_CONST`,
  `SMKX_DISPATCH_BANK`, both off in CT), `tools/emit_bench.py`, `emit_tests.py`, `ctx_run.py`
  (they need `machine68k`).
- **Audit before trusting `PLAIN21/42/43`:** CT's sets are checked against CT's runtime tables. Here
  they were checked by reading this runtime's `.w21`/`.w42` tables and `rt_io_read`: every register
  in CT's sets is a shadow-only store here (`$2100/1/5-C/1A/23-31/33`, `$4201/2/4/5/7-A/C/D`,
  `$43xx`) and `$4218-F`, `$43xx` reads are the shadow; NMITIMEN stores the shadow and
  `rt_nmitimen`. A runtime with its own side effects on one of those (SMK-style `$2105`, a
  `$4203` multiply) must drop it from the set.

**Two findings in shared runtime code, both found while writing the tables:**

1. **`.sysw` in `rt_wb` let writes reach the ROM copy.** `andi.l #$ffff,d7 / bmi.s .done` can
   never branch (a masked long has bit 31 clear), so a write to `$8000-$FFFF` of a system bank
   (`$00-$3F`, `$80-$BF`) that resolved to ROM stored into `smkx_rom`. Fix: `cmpi.w #$8000,d7 /
   bcc.s .done`. Recognised by building the write page table from the same conditions and
   seeing the two disagree. **Present in F030SMW2 (`falcon/smkx_rt.s:844`) and F030SMAS (`:815`)
   as well; not in this form in F030SMK, F030DKC, F030CT, F030FFVI.** The game did not trip over
   it in SMW's attract demo (`make check-ref` was already clean), so it is latent.
2. **An empty dispatch-cache slot must not match a real key.** With slots zeroed at start, a
   dispatch to address `$000000` in a 16-bit context has key 0, hits an empty slot and jumps to
   address 0 instead of reaching the fatal "no handler" report (`.addr 0` table entries exist,
   see the F030FFVI note on jump tables). F030SMW fills every key with `$FFFFFFFF` at start (no
   context/address pair produces it). **F030CT's and F030FFVI's caches (`DCACHE_SLOTS`) start
   zeroed: take the fill over.** F030SMK's and F030DKC's already fill every key with -1; F030SMW2
   and F030SMAS have no cache yet, so take the fill along if they get one.

**Result in SMW** (static, and from the demo level, rendering off, `SMWX_RENDER_EVERY=60000`):

- Generated code, same 95,540 nodes: 493 paged reads, 411 paged writes, 16 guarded register
  accesses, 237 direct shadow accesses, 470 inline `PLB`; `addx`/`subx` 4,321 -> 1,116; instruction
  lines 177,699 -> 178,059. SMW has few unknown-bank operands (about 1,500 helper sites in all,
  against about 12,000 in CT), so the translator gain here comes mostly from the folded carry and
  the register paths, and less from the page lookups.
- `make check-ref` (reference 65816 interpreter, frame for frame): 3,750 frames compared, 0 differ.
- Game logic in the attract demo level (GameMode 7, VBL-locked at 50 Hz): **47.6 -> 50.0 game frames
  per 50 VBLs**, and the boot reaches that level about 500 VBLs sooner (loading modes 850 -> 450
  VBLs). At 50.0 the game is capped by the VBL, so the headroom shows only under load:
  `SMKX_SLOW` burns a fixed loop before each NMI; with the same burn, game frames per VBL
  (old translator vs ported; the demo phase differs a little between runs, so read the trend):
  12,000: 0.89 vs 0.96, 20,000: 0.77 vs 0.97, 28,000: 0.75 vs 0.84.
- No Falcon timing; Hatari's 030 and bus model are not a Falcon's.

For this game: nothing to take over from this finding: the runtime has no `.sysw` block, and `rt_dispatch`'s cache already fills its keys with -1 (`falcon/smkx_rt.s:1355`). It is the origin of the cache CT and F030SMW took over.


## From F030SMW, 2026-09-30: correction and addition to "CT's list-compositor model on SMW snapshots"

Read together with F030SMW's entry of the same day above. Found in F030SMW, `analysis/list-compositor-smw.md`.

- **Correction.** The colour-window rule (CGWSEL mode 1 "inside" / 2 "outside") is decided from the
  colour window's real spans (`ppu_win_spans_on(s, 5, 0x3F, ...)`: none = empty, one span `[0,256)` =
  full), not from WOBJSEL bits 7-4. SMW's level frames have the colour window enabled over the whole
  screen (`WOBJSEL $23`, `WH0 $00`, `WH1 $FF`) with `CGWSEL $12`: "inside" is then everywhere, i.e.
  mode 0. "Written and not exercised" in the entry above was wrong: it is what lets the level
  frames through. With it, 342 of 530 SMW snapshots are covered (the entry above said 4 of 10), among
  them a run of 337 consecutive frames.
- **Still skipped:** 46 frames where a window masks layers (window 1 inverted over an empty range
  with `TMW $15`) and 142 with colour-window math on a partial or per-row window.
- **Checked on it:** `seq`, `hreal` and `blitfuzz` 0 wrong; the recorded stream played by F030CT's
  `falcon/lcplay.s` in Hatari `v2.6.1-devel (Sep 22 2026)`: all 337 page checksums match; player
  time about 5.9 ms an update there, of which the blitter rows (about 40 µs each against the model's
  assumed 13) are two thirds. Emulator figures, not a Falcon's. F030SMW's copy of the model, player,
  converter and run script: `tools/lc/`, `falcon/lcplay.s`, `scripts/run_lcplay.py`; `make
  check-lc-play` and `make bench-lc-play` there.
- **For F030CT:** the two skip rules and the LoROM-or-HiROM choice as a parameter are in
  `games/smw/tools/lc/lc_model.c` (`LC_SMW`, `row_fn`); the player's timing mode (`TIMING`, `PASSES`,
  `SKIPBLIT/COPY/DATA`) is in `games/smw/falcon/lcplay.s` and is a plain addition to yours. Not yet in
  F030CT; nothing in F030CT was changed.


## From F030SMW, 2026-09-30: correction on "the reference composes the sub screen"; window and sub-screen gaps show up when the reference is taken over

Found in F030SMW while taking over F030CT's `src/snes_ppu.c` (commit e1e1923 there). Corrects two claims in
F030SMW's earlier entries of this date.

- **`ppu_sub_cfg` returns 0 when `CGWSEL` bits 7-4 are set** (`src/snes_ppu.c`: `(cgwsel & 0xF0)`). SMW's
  level frames have `CGWSEL $12`/`$22`, so `ppu_render` (and the DSP design model built on it) leaves the
  sub screen out there: BG2 shown through the backdrop by `CGADSUB $20` is **not** composed. It is composed
  on the title frame (`$02`). The earlier entry's "CT's `ppu_render` composes exactly that" holds only for
  `CGWSEL` bits 7-4 zero, and the lc_model frames it lets through on levels are exact against a reference
  that may not show what the SNES shows. Whether it does is undecided (snes9x, or `ppu_render_rgb`, would
  say). A full colour window with "math inside" (`WH $00/$FF`, `WOBJSEL $23`, `CGWSEL $12`) is math
  everywhere, i.e. `ppu_sub_cfg` could treat it as bits 7-4 zero, as F030SMW's `lc_model` rule does.
- **The fast CPU composer (`ppu_fast.c`, SMW/DKC) has no windows and no sub screen.** With CT's reference
  adopted, F030SMW's `make check-snaps` showed them at once: 185 of 530 snapshots differ on window rows or
  sub-screen rows and on nothing else. A port that keeps `ppu_fast.c` next to the new reference should
  report those rows (as `tools/ppu_snap.c` there now does) and not fail on them.
- **Also from the take-over** (reference only, no emulator): CT's `snes_ppu.c` defines `ppu_planar8` itself,
  so a copy of `ppu_fast.c` that still defines it links with a duplicate symbol; it needs LoROM HDMA
  functions (`ppu_abus_lorom`, `ppu_aptr_lorom`, in F030SMW's copy) and exports `ppu_render_rows`,
  `ppu_ref_row`, `ppu_obj_line` if `ppu_fast.c` is to sit on it.

## From F030SMW, 2026-10-01: the Videl's base low byte must be written last; ppu.asm per-row cost measured; `LC` is a DSP56001 register

Found in F030SMW building a DSP-managed list compositor (`analysis/list-compositor-dsp-plan.md`). Emulator only:
patched F030Arcade Hatari `v2.6.1-devel (Sep 22 2026)`, Falcon, TOS 4.04, `--cpu-exact`, `--dsp emu`.

- **Video base: write `$ffff8201` and `$ffff8203` first, `$ffff820d` last.** Hatari's Videl
  (`src/falcon/videl.c`, `VIDEL_ScreenBase_WriteByte`) clears the low byte whenever the high or mid byte is
  written. A screen at an address whose low byte is not 0 is shown from the address with the low byte
  cleared: F030SMW's walker showed every frame with a start column other than 0 shifted by that many 16-pixel
  groups (a 48-pixel shift for column 3), exact otherwise. Found by comparing a Hatari screenshot with the
  reference picture (`tools/lc/shot_compare.py` there: crop the 512x448 area at +114+40, the same palette,
  a small tolerance); a memory checksum would not have shown it. **F030CT's `falcon/lcplay.s` writes the
  low byte first** (`vid_base` and its window update), as did F030SMW's `vid_set_base`; ports that place the
  base at 256-aligned buffers only are not affected. Fixed in F030SMW's `smkx_video.s`, `lcwalk.s`, `lcplay.s`.
  Not checked on a real Falcon: TOS and the Falcon hardware may differ from the emulator there.
- **The eight-plane Videl window with HSCROLL and a line offset works in Hatari as F030CT's notes describe**
  (line offset `(32 - groups) * 8` words with groups = 16, or 17 when HSCROLL is not 0; `$ffff8210` = 128):
  F030SMW's walker showed a scroll ring window with HSCROLL 0, 3, 5 and 7 and start columns 0 to 3, all exact
  against the reference.
- **`ppu.asm` time is per row, not per group** (F030SMW's `LCB` build: only the groups a per-row mask names are
  composed and sent). On SMW frames, compose+record 159 ms for all 3,584 groups, 28 ms for 640 groups in 40
  consecutive rows, 109 ms for 539 random groups over 209 rows, 61 ms for 82 groups over 74 rows. With the
  existing `PROF` switches (2 no sprites, 8 no slivers, 16 no descriptor work): in the sparse case the layers
  take 52 of 61 ms and almost all of it is `desc_build`, which rebuilds the descriptors of all 33 slivers
  whenever a row is not the next of its 8-row block (skipped rows always are): about 0.6 ms a row. Sprites are
  about 3 ms of it. A DSP renderer that draws a few groups a row costs about what it costs for the whole row;
  going sparse needs a partial sliver/descriptor build (range of slivers from the groups' mask).
- **`LC` is a DSP56001 register (loop counter):** the Motorola assembler says "Reserved name used for symbol
  name" for `LC equ 1` (`--define LC=1`); F030SMW's build flag is `LCB`.
- `tools/smkx/dsp_asm.py --define` needs every flag a source tests: give the old builds the new flag (`LCB=0`).

For this game: a port with a Videl ring or any non-aligned base: check the write order; one with a window or
scroll display should compare a screenshot, not only memory.

## From F030SMW, 2026-10-01: HDMA replay cost and how it was cut; list-compositor planner costs on the 030

Found in F030SMW running the list compositor (`src/lc_plan.c`, `src/smkx_lc.c`) in the game. Emulator only: patched
F030Arcade Hatari `v2.6.1-devel (Sep 22 2026)`, Falcon, `--cpu-exact`; times are emulated 030 ticks (1/38400 s), no
claim for a real Falcon.

- **The replay (`snes_ppu.c` `replay`) was the biggest fixed cost of a frame: one HDMA channel that only writes
  window edges** (SMW: channel 7, mode 1 indirect, `$2126/$2127`, two repeat entries of 112 lines, data in WRAM
  `$04A0`/`$0580`, identical units every line) **made every line go through `hdma_events`**: 22 ms a replay on
  the 030, and a frame is replayed once by the planner and once per DSP render (three times in F030SMW).
  Recognised by counting lines, `hdma_events` calls and `fn` calls in a host build (225 / 224 / 1).
  **Fix (shared code, `snes_ppu.c`):** in the fast path of `hdma_events`, when a repeat entry's unit equals the
  unit it wrote last line (`same && idem`), the lines that repeat it are skipped as a run (compare the next
  units with `last`, advance the table or indirect pointer, `count` and `next` by the run), but not past the next
  IRQ-log write (`lim`, the line of the next log entry: it makes the next transfer a real one). Replay 835 -> 135
  ticks, exact: a checksum of `ppu_render` (pixels and the colour histogram) of 337 consecutive SMW frames
  is identical before and after. **Not yet in this copy: take over `src/snes_ppu.c`** (`hdma_events` with the `lim`
  parameter and `skip`, the call in `replay`). Any game with a per-line HDMA of unchanging data benefits (colour
  windows, gradients that do not change between frames' lines).
- **The Falcon runtime passed `in.aptr = 0`: HDMA tables were read byte by byte through `abus`.** With
  `ppu_aptr_lorom` (or the HiROM one) a table entry costs one call, not one per byte; in F030SMW's
  `smkx_video.c` (`in.aptr = ppu_aptr_lorom`). Check the `PpuInput` in your `smkx_video.c`.
- **`ppu_replay_ram` (new, `snes_ppu.c`, declared in `snes_ppu.h`)** is set when an HDMA table or indirect data
  byte was read from RAM (conservative: WRAM, SRAM, the low half of the system banks). A replay that read ROM
  only depends on the registers and the IRQ log, which `lc_plan` uses to keep a replay's result between frames;
  SMW's window tables are in WRAM, so there it did not apply (the run skipping above did the work).
- **Planner costs worth knowing (per plan, 030 ticks, SMW level with a moving opaque BG3):** VRAM diff of the
  written pages 700 (copy + compare of 100+ pages after several game frames), moved layer 2 scans of 986 cells
  each 1,050 -> 220 after a per-layer cache of non-blank cells kept as bit rows, masks/copies 330 -> 130 with bit
  sets (`uint32_t dw[224]`, a ring bitmap for the groups the last frame composed), byte-wise `clear`/`copy` loops on a
  030 are the usual culprit (use long loops), a static frame should cost nothing (early-out when no VRAM unit, scroll
  or OAM changed).
- **Blitter (Hatari): the y increment after a line is `stride - (xcount - 1) * xinc`** (the x increment is not added
  after the last word of a line); a 2D copy of `len` groups x `rows` rows with a 512-byte stride used
  `512 - 16 * len + 2` and matched the CPU copy. The data cache has to be cleared after the blitter wrote
  (CACR CD), as after a DMA record. Runs of 24+ groups go to the blitter (no hog), shorter ones to a `movem`-free long loop.
- **Snapshots in an asynchronous renderer:** `SMKX_SNAP_EVERY` tested `rt_frames mod N == 0`, which an
  asynchronous renderer (not called every frame) rarely hits; F030SMW's `smkx_rt.s` now writes the first snapshot
  at or after each multiple. A snapshot shows the picture lagging its inputs there; to verify, pause the game
  (Start) and compare, or use the synchronous build.

## From F030SMW, 2026-10-01: blitter non-hog mode in Hatari corrupts a copy while the CPU works; hog mode is exact

Found in F030SMW's list compositor (`falcon/smkx_lcput.s`, `src/smkx_lc.c`). Emulator only: patched F030Arcade
Hatari `v2.6.1-devel (Sep 22 2026)`, Falcon, `--cpu-exact`.

- A 2D copy of plane groups with the blitter in **non-hog mode** (`$3c` = `$80`), with the CPU copying other groups
  of the same page or planning meanwhile, left 30-600 groups of the picture wrong, **differently on every run** of the
  same binary (found by comparing the synchronous build's snapshots with the reference; the snapshots are exact
  with the CPU copying alone). **Hog mode (`$c0`, what f030fastbobs and `lcplay.s` use) was exact in every
  run** (nine runs), and so was non-hog with the CPU waiting for the blitter first. Cause not found; take it as
  "do not run the CPU on the same page while a non-hog blit is going" until a Falcon says otherwise.
- **In hog mode the blitter was no faster than 68030 long moves in Hatari** for 16-byte groups (4.4 against 4.7
  microseconds a group over 3,600 groups), so a copy of plane groups is not a reason by itself to use it; what is
  possible is that it overlaps with the DSP's work, which hog mode does not allow on a real machine either (it may
  starve the DMA record). Not measured on a Falcon.
- Blitter 2D copy: the y increment after a line is `stride - (xcount - 1) * xinc`.
- Making a renderer asynchronous (its job shown after the blitter is idle) needs the data cache cleared before the CPU reads
  what the blitter wrote (CACR CD), as after a DMA record.

## From F030SMAS, 2026-10-01: real game frames without a translation (reference 65816 in free-running mode); the sub screen is in nearly every SMAS frame; planner page need against pages read; two small tool findings

Found while bringing the list compositor to F030SMAS (`analysis/list-compositor-smas.md` there, `tools/ref65816.c`,
`tools/ref_tour.py`, `Makefile` targets `ref-snaps`, `check-lc-model`, `check-lc-play`). Emulator only: patched F030Arcade
Hatari `v2.6.1-devel (Sep 22 2026)`.

- **Real frames before the translation exists.** F030SMAS has no translation yet; `tools/ref65816.c` (the reference
  65816 interpreter with the runtime's hardware model, F030SMW's of `17baedb`) was extended to run the ROM free (no
  game-specific `GameLoop` symbol): the VBL comes after a fixed instruction budget (30,000) or at a `WAI`, `$4212` and
  `$4210` follow a pseudo time inside the frame (SMAS polls V-blank at start-up), the frame handler's state is kept as
  `rt_frame_start` keeps it (register shadows, scroll latches, CGRAM at frame start, IRQ chain with its writes logged and
  OAM per segment) and `SNAP.BIN` (version 4) is written, Falcon planes zero. With a scripted pad (`REF_INPUT`, up to
  4,096 events) it reached the title, select screen and gameplay of all four games, so `ppu_snap`, `dsp_snap`,
  `lc_model`, `lcd_model`, PPUTEST/LCDTEST packs and the Hatari players all ran on real frames. These are **not frames of
  the translation or the Falcon**, and the interpreter itself is unchecked for the new game. This port has `tools/ref65816.c` (SMK's game loop differs): not yet in this copy: take over F030SMAS's `tools/ref65816.c` free-running mode if you want frames before or without the translation.
- **The sub screen (colour math against the sub screen on the backdrop) was on in 19-40 of 45 sampled frames of every SMAS
  game** (`TS` $06/$13, `CGWSEL` $02-$06, `CGADSUB` $20: the clouds, hills and HUD are BG2/BG3 on the sub screen). The CPU renderer
  `ppu_fast.c` has none, so it differs from the reference in 20,000-50,000 of 57,344 pixels there: `ppu_fast` is no fallback for
  such a game, only the DSP window/sub builds are. `ppu_snap` says it ("sub screen on N rows ... differences are expected") but
  its exit status then hides the difference. Check `ppu_snap` output before trusting a CPU-path frame.
- **The planner's page need can be far above the pages read:** SMAS SMB1 frame 3,000: `need` 91, the DSP model `read` 37;
  `need` reached 109 over the 93 tile slots and the 77 of the compositor build, so most SMB1 and Lost Levels frames cannot use the
  compositor build. The planner uploads every page any name-table entry points to, including parts that are off screen.
  Narrowing it to the visible entries (plus scroll margin) would cut the need; not done. `tools/dsp_snap.c` of F030SMAS
  does not `-pack` frames the program cannot hold (PPUTEST would overflow its slots); not yet in this copy: that change
  (about 8 lines at the `if (pack)` in `check`).
- **`vasmm68k_mot -DNAME=0x7fffffff` is parsed as 0** ("warning 24: trailing garbage after option -D"; vasm takes `$7fffffff`).
  The Makefiles' `-DSMKX_TRACE_FROM=0x7fffffff` therefore defines the first VBL of traces as 0. Harmless while no trace is
  built in. Fix: pass the value in decimal (`2147483647`) or `\$7fffffff`.
- Makefile: `smkx_lc.o` uses `SMKX_LC_SYNC` and `LC_BLIT_MIN` but only `smkx_video.o` depends on `flags.stamp` in F030SMW's
  Makefile; F030SMAS ties both (and `rt.o`) to it.


## From F030SMW2, 2026-10-01: the compositor on a SuperFX game whose frame IRQ blanks the screen: black palette from the frame-start INIDISP, the exact path degraded by the compositor's boot, the `blank_row` bug seen on a real game; the translated GSU code, not `jumps.txt`

Found in F030SMW2 taking over the DSP stack and F030SMW's compositor (`32cc95c`, then `e3c633f`/`93c0357`) and F030CT's blitter renderer. Patched
F030Arcade Hatari `v2.6.1-devel (Sep 22 2026)`, `--cpu-exact`, `--dsp emu`; emulated, nothing here is a Falcon timing. Full record: F030SMW2
`analysis/list-compositor-smw2.md`. The game reaches only its logo, intro, title and file menu in Hatari (see the last point), so the compositor
was exercised on the logo screens (covered) and the other screens' refusals.

- **The Videl palette took INIDISP from the frame's start.** `smkx_video.c` `make_clut` (and F030SMW's `smkx_set_palette(cgram, in.io[0x100])`)
  use `rt_io_start[0x100]`. YI's frame IRQ sets forced blank (`$8F`) at line 199 and the next frame starts with it (the IRQs at lines 13 and 15 write
  `$00` and `$0F`, or the fade step): **every such frame got an all-black palette**. Recognised by looking at `io[0x100]` of the snapshots (`$8F`
  in every intro frame, `$0F` in the title) and reading `make_clut`; not yet seen in a screenshot. Fix in F030SMW2: `smkx_inidisp(in)`
  (`src/smkx_video.c`): the frame-start value, replaced by the last INIDISP write of the IRQ log at a line up to 112; used by `make_clut` and
  `src/smkx_lc.c` (`j->inidisp`). Not yet in this copy: take over those two places if a game sets INIDISP in an IRQ (a letterbox by
  IRQ, a screen split with a blanked bottom, a late-frame blank).
- **Booting the compositor before every frame degrades the exact path.** In F030SMW's `smkx_render` the DSP is booted into the LC program
  before the compositor is asked, and `smkx_ppu_plan` then refuses a frame that needs the window program while an LC program is loaded, so
  every frame the compositor does not cover and that has windows or the sub screen went to the CPU composer, which has neither: 828 words of
  YI's intro frames differed from the reference (0 after the fix; the DSP window program is exact there). Also two DSP boots per refused frame.
  Fix in F030SMW2 (`fc01de0`): `lc_covers(in)` in `src/lc_plan.c` (the register replay alone, no state touched; its window rows use a third buffer
  `winrows[2]`) and `smkx_render` boots the compositor only for a frame it covers; `smkx_ppu_plan` (exact path) no longer applies the LC rule or
  the LC slot limit (F030SMW's `e3c633f` split `smkx_ppu_plan_lc` off already). **F030SMW and F030DKC: not yet in this copy**; other ports
  with a compositor that draws every refused frame the exact way are not affected.
- **`blank_row` hang seen on a real game.** F030SMAS found that `ppu.asm`'s `blank_row` ignored the LCB mask (F030SMW `93c0357`). On YI's real
  intro frames (forced blank rows 0-12 and 199-223) LCDTEST gave wrong lists for sparse mask patterns and then never answered; after the fix 4
  real frames x 8 patterns pass, 0 differing words (`LC_ALL=1 dsp_snap -packlc` packs every frame, by default only those `lc_covers` takes).
  Take the hunk if the port's copy has not it.
- **A record artifact that refuses frames: equal OAM segments.** The runtime records the OAM at every IRQ (`rt_oam_segment`), whether or not it
  changed. YI's four segments (lines 0, 13, 15, 199) are byte for byte equal and `lc_plan` refuses `noamseg > 1` (`lc_why` bit 7). A planner that
  compares the segments with the first would not refuse them; not done. Any game with an IRQ chain shows the same.
- **`lc_why` of the YI screens** (decode in `src/lc_plan.c`): `$092` intro (per-line INIDISP, forced-blank start, four segments), `$142` and `$042`
  title and menu (BGMODE per line by HDMA, 16x16 tiles, the ring layer's H scroll per row). `tools/lc/lcd_model.c` now prints the register state of
  every frame it refuses (BGMODE, TM, TS, windows, CGWSEL/CGADSUB, HDMAEN, IRQ writes, OAM segments, the first differing register).
- **A scripted pad press must outlast the game's polling.** YI's title takes Start (and A, B, X, Y) in a routine that apparently polls rarely: presses
  of 6 VBLs every 150 frames for 16,000 frames did nothing, **a press held 100 frames every 400 reached the file menu**. SMW's
  `input_script.py` format (`FRAME:PAD`) is enough.
- **A stack-jump trap whose target is data is not necessarily a missing `jumps.txt` entry.** F030SMK's note of 2026-09-24 called YI's
  `stack jump to an unlisted target $000FCACB` "a new push-and-return target". It is not: the site is `LDA table-$1A,y : PHA : RTS` with a sprite
  status that no valid state has. `YIX_GSU=check` (every SuperFX run both ways) reports the first difference at run 3 and the game, built
  with the SuperFX interpreter (`YIX_GSU=interp`, 5-20 times slower), passes the same point without a trap. The translated SuperFX
  code (`tools/smkx/gsu2c.py`) writes something the core does not. Diagnose such a trap by running the reference once first.
- **Snapshot version drift:** F030SMW2's runtime wrote version 4 while its host tools expected 5 (the layout carries the 32 KiB GSU RAM): the
  tools said "not a version 5 snapshot" on every file. The format doc is in `src/snes_ppu.h`; a port that changes the layout must change both.
- Consecutive snapshots of every frame need the compositor and the throttle off (a quiet frame writes nothing): F030SMW2 `YIX_LC=0 YIX_SNAP_EVERY=1
  YIX_SNAP_FROM=n`; the interpreter run to the title took 10,500 frames of loading.
- **`.vmaddh` stored VMADDH unmasked: a VRAM word address of `$8000` or more wrote past `rt_vram`.** `move.b d6,rt_vmadd` in the `$2117` handler
  (every port's `falcon/smkx_rt.s` has the same two lines; F030DKC adds a prefetch call). A game that sets VMADD with bit 15 set made `.vmdatal`/`.vmdatah`
  (`add.l d4,d4` up to `$1FFFE`) and the DMA's VRAM path write up to 64 KiB behind the 64 KiB array and `M7DIRTY`/`rt_m7_mark` set flags past their
  128 bytes: YI's `rt_frames` became `$00FFxxxx` and the game's mode byte `$38` about 29,000 frames in (deterministic, also with rendering off).
  Recognised by the status line (a frame counter that jumps by `$FF0000` and a long run that ends at once with "frame limit reached"), then by
  reading which stores take an unbounded index. Fix: `andi.b #$7f,d6` before the store (F030SMW2 `09258e3`). **Not yet in this copy: the same two
  lines.** (The SNES wraps the address; the real range is 15 bits.)
- **After a file choice YI stops on the APU port.** With the translated SuperFX replaced by the interpreter and the VMADDH fix, YI goes on to game mode `$38`
  and waits for ever on `$2140` (the last I/O address for 27,000 frames; nothing else changes). The sound upload of the next scene needs the driver's
  handshake; this runtime's echo model (written for the IPL upload) does not give it. F030CT's SPC700 interpreter is the way.

For this game: your entry of 2026-09-24 (night) calls YI's stack-jump stop a `jumps.txt` gap; it is not (the SuperFX bullet above; `analysis/yi.md` in F030SMW2). The `.vmaddh` lines and `smkx_inidisp` apply to SMK too (SMK's frame IRQ sets INIDISP? check `io[0x100]` of a snapshot).

## From F030SCIV, 2026-10-01

Found while bringing up Super Castlevania IV (LoROM, no listing; code found by
`tools/sciv_trace.py` tracing from the vectors). Both are in F030SCIV's copy of
the shared code; not yet in this copy.

### A game's frame logic can run inside its NMI and wait for the next NMI: re-entrant NMI

SCIV's NMI handler runs the whole game frame; a routine in it
(`WaitNMI`: `lda #$80 : sta $3C : lda $3C : bmi`) waits for the next NMI, which
finds `$3C` non-zero, sets it to 1 and returns. The runtime ran one frame
handler at a time (`rt_in_frame`), so the nested NMI never came and the game
hung after two frames ("frame handler still running"). Fix in `falcon/smkx_rt.s`
`rt_vbl .busy`: when `rt_idled` is set (an idle mark in the wait loop) and
NMIs are enabled, run one nested `smkx_nmi` on the interrupted registers
instead of dropping the VBL. Needs an idle mark on the wait loop
(`tools/smkx/marks.txt`). Recognised by: `nt 81`, NMI count stuck, interrupted
PC always the same spin.

### `lda $BB0000,x` with a 16-bit X is a full-bank access, not WRAM

`emit.py` classified a `long,x` operand by its base address, so
`lda $010000,x` (base offset below `$2000` in a system bank) was emitted as a
direct WRAM read although X holds `$8000+` addresses (ROM). Fix in
`tools/smkx/emit.py` `loc()`: `LONGX` in a system bank with base offset
`< $100` is resolved at run time (`indexed-pointer`), like `abs,x` with a small
base. Recognised by: a script/stream reader returning zeros.

## From F030CT, 2026-10-01: the list compositor in CT: the planner reads HDMA tables from live WRAM while the game runs (groups wrong for good in pictures planned asynchronously), the LCB build stalls on forced-blank rows (the open F030SMAS bug), page slots, a planner for CT's IRQ blank, a precheck before the DSP boot

Found while taking F030DKC's compositor (73f863a: LC and LCW kernels, window rows in the planner) into F030CT; numbers and the scene census are in F030CT's
`analysis/list-compositor-ct.md`. Emulator only: patched F030Arcade Hatari `v2.6.1-devel (Sep 22 2026)`.

- **`snapshot()` (src/smkx_lc.c) must copy WRAM too: the planners read the HDMA tables from live WRAM, and the game rewrites them while a plan runs** (F030DKC, F030SMW, F030SMW2,
  F030SMAS: every compositor that plans in the render context while the game runs; same code). Symptom in F030CT's game: bands of 1-16 rows by 1-16 groups (sprite-like, and full-width bands
  of 2-4 rows) in the compositor's picture that stayed wrong in every picture after, until the scene changed; 3-7 of ~35 covered snapshots a run, **only in pictures that were planned
  while the game ran** (asynchronously, and in the one-job-at-a-time `CTX_LC_SYNC=1` build too). Pictures planned with the game stopped (the snapshot frames, rendered in the VBL) were exact, so were
  runs in which only snapshot frames were planned (312 covered snapshots of 313, 0 wrong, 2,000 frames, scroll across the ring's wrap included) and runs with interrupts masked during the render calls (375 covered of 378, 0 wrong),
  and the host planner/model is exact on every frame sequence taken from the same game (consecutive, or every 2nd to 30th frame): the planner alone cannot reproduce it. Cause: `lc_plan` replays
  the frame's HDMA tables (`ppu_replay_changes`, through `in->abus`/`in->aptr`: `smkx_wram`, live) to decide which rows have their own scroll, and each `dsp_plan` of the same job replays them again; a plan
  of one job spans 0.1-0.5 s of 030 time (tens of NMIs), the NMI rewrites the tables, so the planner's groups and the DSP's rows come from different tables: a group the planner takes as "composed
  from this frame's inputs" is not, and nothing ever corrects it (the next plan compares the frame with what it believes the ring holds). Recognised by: the wrong groups were the copied ones (a
  job with 76 copied groups had 64 + 12 wrong ones), not stale content of any earlier picture, every earlier frame's pictures exact, a CPU copy verified exact against its source (`CTX_LC_DIAG=1`:
  every copy block compared, 168 of 168 blocks exact), the DSP's list fully recorded (a sentinel pattern in the list buffer: 0 groups left unwritten), the written-pages flags exact (0 of 128 pages differing unflagged), the page uploads
  (always upload: still wrong), the DSP planner's incremental caches (`dsp_plan_reset` before each plan: still wrong), the blitter and the copy queue (CPU only and no queue: still wrong). What
  decided it: planning at snapshot frames only (exact) against planning while the game runs (wrong), then interrupts masked (exact), then masked around the input copy only (still wrong: so not that copy, but data
  read later). Fix (`snapshot()`): `wram_snap` (128 KB, copied with `smkx_lc_cpu` when the plan starts: a few per cent of a plan) and a private `PpuBus` whose `wram` is the copy go into the job's `PpuInput` (`out->ctx`), so every
  plan of a job reads one version (a copy torn by an NMI is as good: both planners read the same). With it: 72 of 72 snapshots exact in the scene that failed every time before (a 42-row scroll jump into the ring's wrap),
  and 0 wrong in the seven runs after (49, 83, 180, 182, 49, 9 and 51 covered snapshots: asynchronous default, credits, a throne-room cutscene, the window scene, ...). OAM is read live too (`rt_oam` has no frame-start copy), `snapshot()` copies it at the start of the plan: one
  version per job already. **This copy has no compositor runtime yet: when it takes `src/smkx_lc.c`, take F030CT's `snapshot()` with it (`wram_snap`, `bus_snap`, the `smkx_lc_cpu` call; a LoROM port compares `in->abus` with `ppu_abus_lorom` too).** A game without HDMA tables in WRAM (tables in ROM) is not affected; a game whose HDMA tables change per frame is. The same live read exists in the whole-frame renderers (their next picture
  corrects it). A diagnosis recipe that did not exist: `scripts/lc_snaps.sh DIR FROM TO EVERY CTX_LC_MATH=0` with the snapshots taken every 2nd-8th frame while the game runs, `scripts/lc_check.py DIR`, and compare with a run where
  only snapshot frames plan; `CTX_LC_DIAG=1` (job log `SMKX LCLOG`, copy verification, list sentinel) in F030CT's Makefile/`smkx_lc.c`.
- **`put_block` (src/smkx_lc.c) writes garbage into the page's mirror when a copy block crosses the ring's end** (F030DKC 73f863a and F030SMW e3c633f have it; every port
  with the compositor). A block of copied groups (`copy_groups`: `mdelta` = the ring's size) advances its source pointer by `sstride` row by row; past the ring's end that pointer is
  already in the *source page's mirror* (ring groups 0..LC_DUP-1 again at LC_N), and the destination's mirror then reads from `src + mdelta`, beyond the mirror: the destination's mirror
  (what the Videl displays under the window rows past the end) is stale or garbage while the ring itself is right. A walk (list source, `mdelta` 0) is not affected. Found in
  F030CT's game: odd frames (page B) wrong in the rows after the wrap (S = 10,792, rows 175-223), all groups of those rows; the snapshot reader hid half of it
  (`smkx_lc_window` read the ring through `& (LC_N - 1)` for the first group of a row, the Videl reads the mirror: now it reads as the Videl does). Reproduced on the host
  with `tools/lc/lcput_test.c` (the Falcon's file included, C stand-ins for the assembly copies): F030DKC's and F030SMW's `put_block` fail 2.8 million groups of 20,000 random copy blocks, 0
  in walks; the fix passes both. Fix (`put_block`): in a copy (`mdelta != 0`) recompute the source pointer from the ring position when the block wraps
  (`base + L * 16`, `base = src - L0 * 16` at entry) instead of advancing it; in the per-group branch too. Not yet in this copy of games/dkc/F030SMW/F030SMAS/F030SMW2: take over
  `put_block` and `tools/lc/lcput_test.c` (`make check-lc-put`); the snapshot reader change is separate and optional.
- **LCB: `blank_row` in `falcon/dsp/ppu.asm` sends the whole row (128 zero words) whatever the row's mask says.** A frame whose `INIDISP` is forced
  blank for some rows (an IRQ writes `$80` mid-frame, or the frame starts blank) then sends more words than the list has: the host's DMA record window ends,
  the DSP's ring fills and it waits in `room`; the next host `put()` never returns. It needs a *partial* mask on a blank row (an all-ones mask is fine), so
  LCDTEST passes pattern 0 and **hangs after pattern 1**. **This is the "open bug, cause not found" in F030SMAS's notes** (all-backdrop frame, patterns 0 and 1
  answer, then nothing): reproduced on F030CT's `F0000040` (a frame that starts in forced blank, no VRAM page): the LCB kernel of `98bf572` hangs after pattern 1,
  the fixed one passes all eight patterns (both run in Hatari). The runtime never sent such a frame because `lc_plan` refused every frame with a register
  change mid-frame and returned 1 (skip) for a frame that starts blank; it matters as soon as the planner covers blank rows (F030CT's does). How it was
  found: Hatari's debugger through the command FIFO (`hatari ... --cmd-fifo f`; `echo "hatari-debug dspreg" > f`, `dm x 0x40-0x50`): the DSP's PC sat in `room`,
  `ROW` ($0040) was 223, 375 words queued; the listing (`build/*/ppulc.lst`) names the loop. Fix (`falcon/dsp/ppu.asm`, 9 words): under `LCB`, `blank_row`
  shifts `RMASK` through 16 groups and sends eight zero words per set bit. Not yet in this copy of games/dkc/F030SMW/F030SMAS: take over the `blank_row` hunk.
  Also: F030CT's `src/lcdtest.c` aborts after its spin limit (4,000,000) instead of 50,000,000; `tools/dsp_snap.c -packlc` takes `LCD_PATS=<mask>` (which of the 8
  mask patterns to pack) and `LCD_VERBOSE=1` (why a frame is not packed).
- **`dsp_snap -packlc` plans a frame differently from the runtime**: it clears `CGADSUB` in the shadow but does not set `ppu_replay_nocg`, so a frame with an HDMA
  channel writing `CGADSUB`/`CGWSEL` (CT's `$2131` gradients) still gets its sub-screen rows, the window build (kernel 2) and an LCW pack that the runtime would
  never send (`smkx_lc.c` plans with `ppu_replay_nocg = 1`). The LCW kernel produced all-zero output on such a frame (F030CT `F0001547`, 211 sub-screen rows; the
  kernel of F030DKC's tree does the same, so it is not the changes above): colour-math rows are not for LCB+WIN. Fixed in F030CT's `dsp_snap.c` (set `ppu_replay_nocg` around
  the plan); take it over with `-packlc`.
- **LCB: masks and variables can live at X:$0500-$05EF instead of X:$3000-$30FF.** X:$3000 is P:$7000, so the LC build lost 16 of the tile program's 93 page slots
  (77 left; 73 in the LCW build). The stream's room (X:$0100-$06EF, 1,520 words) has the space: `MASKS`..`LCLO` at $0500-$05E6 limit the LC builds' stream to 1,024
  words. F030CT: `DSP_LC_SLOTS` = `DSP_TILE_SLOTS` (93), `DSP_LCW_SLOTS` = `DSP_WIN_SLOTS` (89), `DSP_LC_STREAM_MAX` 1,024 (`src/ppu_dsp.h`, `ppu.asm`, the stream check in
  `smkx_ppu.c`, `tools/dsp_snap.c`), `dsp_lc_slot_p` = the tile layout. CT's covered frames read 82-91 pages (40 of 108 covered snapshots need more than 77). F030SMAS's notes
  have "need 91-109 over the 77": up to 93/89 now. LCDTEST: 280 frames (20 snapshots x 8 patterns and 120 shifted renders, 10 of the snapshots on the LCW build) up to 91 pages, 0 differing words.
- **`smkx_lc.c`: `shifted()` does not shift the frame's own OAM when the frame has OAM segments**, and `snapshot()` leaves `out->oam` at the live OAM. The rows above the
  first segment's line draw with `in->oam`. Latent while the planner refused segments at line > 0; fixed in F030CT (always copy and shift `in->oam`, then the
  segments). Not yet in this copy of games/smw/F030DKC/F030SMAS/F030SMW2: take over `shifted()` and `snapshot()`.
- **Planner: a forced blank an IRQ writes mid-frame (CT writes `INIDISP=$80` at line 212/216 and does its VRAM updates there) is coverable.** `src/lc_plan.c`: `State.brow`
  (the first blank row), `cap_fn` accepts an `INIDISP` change to blank and ignores every register from there on (a change back to display makes the frame uncovered), OAM
  segments at or after the blank row are accepted (`segs_ok`), `Prev.blank` keeps the ring groups the last picture showed blank: a group that comes into a blank row is
  composed blank unless it already showed blank, a group that showed blank and comes into a visible row is composed. 9 of F030CT's 200 sampled frames were covered
  without it, 116 with it and the windows (79 if colour math sends a frame to the old paths); the host model (`tools/lc/lcd_model.c`) is exact on all of them and on 193 consecutive in-game
  frames. Forced-blank rows are index 0 (`in->map[0]`), the Videl palette's entry 0. F030FFVI has the same IRQ (line 216) but its frames fail on per-line HDMA of `TM` and screen bases.
- **`lc_covers(in)` (lc_plan.c) says whether a frame is covered without touching the planner's state** (`State.nofill`: the window and scroll rows are the planner's), so that the runtime does not
  boot the DSP into the compositor's program for a scene it refuses: the boot drops the page cache of the other programs, and a scene of refused frames (a Mode 7 intro, a battle
  with a split) re-uploaded its pages every frame. F030CT's `lc_try` calls it (after `lc_likely`, a look at the registers) when the compositor is not running and waits a few frames
  after a refusal (`lc_wait`). F030DKC's `lc_try` boots first and plans after; `smkx_vid_stat` counts the boots.
- **A frame that starts in forced blank**: F030CT shows it as a black picture (a window job with the frame's palette, which `make_clut8` turns black: the ring stays, the order with
  the jobs queued before it is kept) rather than keeping the last picture: CT holds forced blank for many frames at scene changes (51 of 927 compositor calls in the demo's first 9,700 frames, with long runs of them at scene changes);
  F030DKC's blank frames are the NMI's own. No snapshot is made of such a frame in a synchronous run (the planes would be the previous picture).
- **`ppu_snap`**: `PPU_SNAP_NOMATH=1` clears `CGADSUB` in a copy of the shadow and the IRQ log before the reference render, to compare a compositor frame that left colour math out;
  F030CT's `scripts/lc_check.py DIR` runs a directory of snapshots through `tools/lc/lc_why.c` (does the planner cover it, and if not which reason bits) and `ppu_snap` and prints the totals;
  `scripts/lc_snaps.sh DIR FROM TO EVERY` captures consecutive snapshots in Hatari (it starts its own Hatari and stops that PID only; `run_smkx.py` shares fifo and log names by tag and was
  killed from outside twice here, leaving its Hatari running); `scripts/lc_shot.py FRAME` compares the picture Hatari's Videl shows (screenshot) with the snapshot's planes.
- **`vid_set_base` wrote the low byte first** (F030CT's `smkx_video.s`): Hatari's Videl clears the low byte when the mid/high bytes are written, so a base that is not 256-aligned (the compositor's
  window starts at group S, 16 bytes) is lost. Take F030DKC's order (high, mid, low); F030CT's also keeps d1.
- **Colour math in the compositor**: 32 of F030CT's 108 covered snapshots (then) enable it (CT fades with a fixed colour subtracted, `CGADSUB` $83/$8F, and puts BG3 on the sub screen); the compositor
  cannot show it. F030CT builds with `CTX_LC_MATH=1` by default (`lc_refuse_math`: `CGADSUB` naming a layer and `CGWSEL` not "never" sends the frame to the old paths); `CTX_LC_MATH=0` draws it
  without. A port whose game uses colour math for effects the player sees should count it before trusting "covered".
- **Test lists that name untracked snapshots**: F030CT's `check-lc-model` named `F0001B04.BIN`... of one machine's run; on another run they do not exist or are another scene. Named frames that
  exist now, and `PPUTEST_FRAMES`/`LCD_FRAMES` fall back to snapshots at hand when the usual directory is missing.
- `falcon/lcplay.s`: F030CT took F030SMAS's (`d6` as the scratch register of the Videl base; F030SMW's `9348783` used `d5`, the memory base). `falcon/lcdtest.s` lost the `ppu_tests.bin` incbin as in F030SMAS.
- Removing `RENDER_EVERY`: F030CT has `CTX_RENDER` (0: no rendering) now and its own throttle, `smkx_render_poll(idle)` (the VBL asks when the work is idle, the DSP's frame time and the plan time
  decide whether now), the compositor asks every VBL while it is active and the jobs in flight (2) are the back-pressure; `scripts/ctx_seed_loop.py`, `tools/ctx_run.py` and the docs name `CTX_RENDER=0`.
- **`make test-lcd` reuses an old `lcd_tests.bin`** when only the packer changed (the rule depends on the `dsp_snap` binary and the snapshots: F030CT ran 112 frames from a file packed by an older `dsp_snap` until
  `make -B build/ctx/lcd_tests.bin` made the 280 it should have). A test program with more frames than the machine's 14 MB (`Program too large`): F030FFVI's window frames read up to 78 pages each, so it packs the first 12 snapshots (`wordlist 1,12`).
- **What the compositor costs in CT (Hatari, emulated)**: a plan takes 95 ms of 030 time on average over the demo's scenes (21 cold frames of 3,584 groups among them), 59 ms to issue a picture (walk, copy), 9 ms to set its
  window: in a covered stretch of the same game time the compositor showed 35 pictures where the whole-frame DSP renderer showed 92 (and the game got 0.94 NMIs a VBL against 0.81). The compositor's planning, not the DSP, is
  what to cut; a port that expects "more pictures" from it should measure the picture count, not only that the pictures are exact.

### `sta $00,x` with D = 0 and X = `$43x0`: a 16-bit index through the direct page (from F030SCIV, 2026-10-01)

SCIV programs the DMA/HDMA channel registers with a table loop:
`ldx $0000,y : sta $00,x : sta $01,x ...` at D = 0 and X = `$4350`. The emitter
sees `dp,x` with D = 0 and a base below `$2000` and emits a WRAM store, so the
channel registers were never written (HDMA never ran: the per-scanline
`$2105`/`$212C` writes were lost, one Mode 1 scene drew as an empty Mode 7 one).
Fix in `tools/smkx/emit.py` `loc()`: instruction addresses listed in
`tools/smkx/wide_index.txt` get their `dp,x`/`dp,y` access resolved at run time
(D from `rt_dreg` when not known). The list is found by hand (compare against
snes9x); not yet in this copy: take over `emit.py` and the list file. Recognised
by: `$420C` set but the channel registers hold stale values.
## From F030SMW, 2026-10-01: rebuilding with native Windows/MSYS2 tools

Found rebuilding SMWX.TOS on Windows from fa54277. Native MinGW Python's
self-reexec in tools/smkx/__main__.py lost quoting around a ROM path with
spaces; set PYTHONHASHSEED=0 before translation (avoids the reexec), or use
a space-free ROM path. Native Python also wrote sources.txt with CRLF:
the Makefile's shell loop passed the trailing CR to vasm as part of each
filename. Normalize that generated file to LF before assembly. Use decimal
2147483647 for SMWX_TRACE_FROM: this vasm warned about the 0x7fffffff default.
The installed DSP DOS runner was DOSBox Staging, not DOSBox-X; an untracked
build/rebuild_dsp.py adapter used Staging with -noconsole and the original
-c mount / batch commands, omitting the DOSBox-X-specific options. All six
DSP images assembled with the listing error-count check, and the game linked.

Changed files: generated files under build/ only; no shared code fix yet.
Status in this copy: not yet in this copy: apply these build workarounds to
its tools/smkx/__main__.py, generated sources.txt and DSP invocation when
building with native Windows Python/MSYS2. No game-specific renderer or
runtime implication; this concerns the shared build pipeline only.

## From F030SCIV, 2026-10-01: rebuilding under Windows/MSYS2

F030Arcade's `games/snowbros/scripts/build-dsp.sh` supplies the working
Motorola ASM56000/CLDLOD invocation for DOSBox Staging: `--noprimaryconf
--set output=texture <batch-file>`. SCIV's helper only found DOSBox-X and
failed with WinError 2 although Staging was installed. Fixed
`tools/smkx/dsp_asm.py`: accept DOSBOX or discover dosbox-x/dosbox-staging/dosbox,
choose the matching arguments, and retain listing error-count validation.
Also fixed `tools/smkx/__main__.py`: write sources.txt with LF explicitly;
Windows CRLF left a trailing CR in each filename in the shell assembly loop.
A full default SCIVX.TOS rebuild passed with MSYS2's mintelf GCC 15.2 and
Staging, with SDL_VIDEODRIVER=dummy and SDL_AUDIODRIVER=dummy. No emulator run
or performance measurement. A temporary untracked ROM alias without spaces
avoided this machine's make/native-Python argument splitting; removed afterwards.
Not yet in this copy: take over tools/smkx/dsp_asm.py and the sources.txt LF write in tools/smkx/__main__.py if the same Windows build path is used. This is tooling only; game behavior is unaffected.

## From F030SCIV, 2026-10-01: LC/LCW compositor and Windows validation

SCIV adopted F030CT's working-tree compositor (HEAD 72f2c2e), including
LCW windows, corrected ring/mirror copies, blank-row masks and 93/89 slots.
Files: src/lc_plan.c/.h, src/smkx_lc.c, src/smkx_ppu.c/.h,
src/ppu_dsp.h, src/ppu_dsp_plan.c, src/smkx_video.c, replay hooks in
src/snes_ppu.c/.h, falcon/dsp/ppu.asm, falcon/smkx_lcput.s,
falcon/smkx_rt.s, falcon/smkx_video.s, falcon/smkx_m7dsp.s,
tools/dsp_snap.c, tools/lc/*, falcon/lcdtest.s, src/lcdtest.c, Makefile.
SCIV retains 1 MiB LoROM, nested NMI and NMI-keyed input: do not transplant
its bus map or game-specific runtime wholesale. The smkx_lc.c WRAM
snapshot guard must recognise that game's bus callback (SCIV: LoROM;
CT: HiROM), or the planners read different live HDMA tables.

Two shared compositor corrections were found here:
- An IRQ enabling colour math midway through a frame was accepted despite
  LC_REFUSE_MATH=1, because the imported guard suppressed colour writes
  before checking the initial state. A synthetic $2131 write reproduced it.
  src/lc_plan.c now captures colour registers with ppu_walk_all when exact
  colour is required, records visible math and mid-frame CGRAM writes
  (why $200/$400), and keys replay caching by the guard policy. Regression:
  tools/lc/sciv_lc_test.c. Default SCIVX_LC_MATH=1 uses exact fallback.
- set_depth(2) after a snapshot overrode SMKX_LC_SYNC=1, invalidating the
  checker's assumption that one job owns the private snapshot arrays.
  src/smkx_lc.c keeps depth 1 when compiled synchronous; the restoration
  contract is checked in tools/lc/lcput_test.c, both normal and sync builds.

Correction to the previous rebuild note: argument splitting was not make's
quoting. tools/smkx/__main__.py restarted Python with os.execv to set
PYTHONHASHSEED=0. Direct invocation reproduced Windows CRT execv losing
quotes for a spaced ROM path, and it does not replace the parent like POSIX.
It now uses subprocess.call and propagates child status on Windows,
retaining POSIX execv. Forced translation with the original ZIP filename
passes; the LF sources.txt fix remains needed.

Validation used freshly compiled pinned patched Hatari 3eea7b07 in the
required F030Arcade/third_party/hatari/build/src/hatari.exe path (Windows
suffix only), Release/Ninja, SHA256
600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf.
The Windows build has no Unix command FIFO: scripts/run_smkx.py omits
--cmd-fifo, watches its own event trace and stops/waits only its own PID.
Makefile selects the .exe suffix and stamps LCD frame-list selections.
No physical Falcon timing or throughput claim: actual LC/LCW kernels pass
28 synthetic plus 112 captured mask/shift cases with zero differing words.
160 host frames (106 windows), 20,000 walks, 20,000 copies and 1,000 jobs
pass; live compositor checking reports no mismatch. A 6,000-frame attract
run reached its limit without a trap; all 15 sampled snapshot planes match
the reference. Four early samples are covered; later samples fall back for
16x16 tiles, colour math and ring-layer per-row scroll. Window support does
not imply those effects are covered or a gameplay speed-up.
Details: games/sciv/analysis/list-compositor-sciv.md.
Colour guard: not yet in this copy: take over State/cap_fn/capture/why_bits/cache-policy in src/lc_plan.c and the regression cases.
Sync depth fix: not yet in this copy: take over smkx_lc_set_depth in src/smkx_lc.c and the tools/lc/lcput_test.c check.
Windows translator/runner fixes: not yet in this copy unless already adapted locally: take over the Windows restart in tools/smkx/__main__.py; adapt scripts/run_smkx.py and Makefile to the local port names.
For SMK: race Mode 7 remains on its existing path; the colour guard affects tile scenes and fades.

## 2026-10-01 — F030SCIV: DSP compositor scanline selection and Mode 7 passes

Found while investigating the slow initial castle-gate gameplay scene in
F030SCIV. Frame-start BGMODE=7 was misleading: replaying actual HDMA shows
Mode 1 ($09) at visible row 0 with TM=$14 (BG3/OBJ), TM=$17 from row 31
(adding BG1/BG2), and CGWSEL=$02 from row 191 with CGADSUB=$42 / TS=$35
(half-subscreen addition on BG2 near the ground). This captured frame has
no visible Mode 7 rows. Diagnose the visible replay, not just initial IO.

The compositor now uses fixed screen coordinates for mode/layer/map-base/
vertical-origin splits and Mode 7, composing the full frame conservatively.
True mixed tile/Mode 7 frames use disjoint LC/LC7 row masks into one page,
presenting only after both passes finish. LC7 masks at X:$3300 avoid plane,
stream, lookup and sprite memory; skipped rows still advance M7 entries.
LCW must save/restore its row mask across sub-screen composition, otherwise
the sub pass consumes it and the main pass emits nothing (DSP test timeout).
Half-subscreen addition preserves the reference eight-plane checkerboard,
not RGB565 blending; other math, Mode 7 windows/EXTBG and 16x16 tiles still
fall back. A one-job live checker must freeze planned VRAM: later runtime
HUD updates otherwise cause false mismatches against already uploaded tiles.

Changed files here: src/lc_plan.c/.h, src/smkx_lc.c, src/smkx_ppu.c/.h,
src/ppu_dsp_plan.c, src/ppu_dsp.h, src/smkx_video.c, falcon/dsp/ppu.asm,
falcon/smkx_m7dsp.s, falcon/smkx_rt.s, Makefile, tools/dsp_snap.c,
src/lcdtest.c, falcon/lcdtest.s, tools/lc/sciv_lc_test.c, lcd_model.c,
lcput_test.c. Details: games/sciv/analysis/list-compositor-sciv.md.
Validation: 185 host frames zero wrong pixels, 20,000 walks + 20,000 copies
and 1,000 complete jobs zero bad groups; 60 actual DSP cases including
mixed-mode, half-subscreen and captured stage 1-1 zero differing words.
Live stage-1 gameplay checking reports no LCCHK mismatch; 15 captured
frames match the reference. Patched Hatari 3eea7b07, build/src/hatari.exe,
SHA256 600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf.
These are emulator correctness checks, with no throughput/speedup or
physical Falcon claim. The ordinary executable retains LC=1, checks off.
Not yet in this copy: take over the files above, adapting the bus mapping and this port's build prefix.
Mode 7/tile HUD splits can use disjoint passes; per-frame program reload/upload cost needs measurement.


## 2026-10-01 — F030SCIV: retain fixed-screen compositor groups

The first scanline-selection extension composed all 3,584 groups and copied
that whole list each frame, defeating retention and adding overhead over a
whole-frame DSP buffer. Corrected src/lc_plan.c/.h and src/smkx_lc.c: exact
visible row configurations (HDMA included) invalidate only changed rows;
old/new OAM footprints and changed sprite tiles invalidate their groups;
32-byte VRAM dirty units are checked against each visible map word and
character row. Mode 7 plane changes conservatively redraw Mode 7 rows.
Copy masks track only updates the other page missed; the runtime must use
these dirty masks, not replace them with full row masks for fixed passes.
No graphics effect or selection was disabled. Fully hidden dirty-tile
filtering remains conservative; opacity, priority, windows and sub-screen
contributions need proof before omitting those invalidations.

Recognised with host/reference tests and the user's slower stage-1 report.
Same captured castle-gate sequence: warmed frames now compose 0–128 groups
instead of 3,584. Across 47 covered probe captures the mean drops from 2,592
to 945 groups; zero wrong pixels. These are planner counts, not throughput
or FPS. 233 synthetic frames (including dirty tiles and moving split
boundaries), copy tests and live gameplay checking pass. Patched Hatari
3eea7b07, build/src/hatari.exe, SHA256
600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf;
no physical Falcon or measured emulator speed-up claim.
Tests changed: tools/lc/sciv_lc_test.c. Details:
games/sciv/analysis/list-compositor-sciv.md.
Not yet in this copy: take over src/lc_plan.c/.h, src/smkx_lc.c and the regression alongside the scanline extension. For this game, retain its bus and validate its raster/sub-screen configurations before enabling coverage.


## 2026-10-01 — F030SCIV: Chorensha mask reference for occlusion planning

Inspected F030Arcade/third_party/chorensha/sources/atari/dsprite.asm
(draw_inverted_masks/draw_masks) and graphics.s (clear_sprites_dsp).
Its DSP constructs sprite footprint masks; the host consumes RLE runs to
restore the corresponding background. This supplies a compact mask/run
representation, not a SNES priority-aware visibility mask. SCIV's DSP
already tracks uncovered pixels with C_UNC in comp_group and stops when
coverage is complete. The next optimisation is retained per-priority-pass
visibility for dirty planning: skip a background dependency only if stable
opaque foreground covers its affected pixels. Invalidate old/new coverage
when foreground graphics, priority, position, windows or raster state
change; account for sub-screen blending separately. A single union of all
opaque pixels cannot decide which lower layer is hidden safely.
No code change or performance measurement; reusable design finding only.
Not yet in this copy: retained per-pass occlusion masks. Applies where this game's stable opaque foreground hides changing background tiles; blending can prevent culling.


## 2026-10-01 — F030SCIV: required graphics work sharing and copy costs

User clarified the architecture: raw VRAM -> SSI/DMA -> DSP -> SSI/DMA ->
RAM composition result -> CPU walks result -> screen buffer. DSP owns decoding,
visibility/occlusion, priority/windows, dirty planning, effects and composition.
CPU only performs unavoidable control/transfer setup and blindly executes the
returned draw/copy/clear operations in the screen buffer. Avoid CPU staging,
bulk WRAM/VRAM snapshots, framebuffer copies and page-mirror repair; final
screen writes requested by the DSP are permitted. Preserve all game effects.
Rule documented in games/sciv/AGENTS.md; migration gaps and census documented in
analysis/list-compositor-sciv.md. No new transport/offload kernel retained.

Recognised after fewer LC groups still produced slower stage 1: CPU dirty
planning, 128 KiB WRAM snapshots and list/page copies can outweigh the saved
DSP composition. Twelve separated stationary stage-1 captures had 768 warm
dirty groups, 629 unchanged reference groups; most invisible background
contributions were transparent, not occluded. Genuine opaque-hidden BG2 groups
averaged 433 per capture, but this does not establish dirty-work or time savings.
The oracle requires reference rendering and is research only. Blended ground
requires both screens; previous-frame visibility alone cannot prove safe culling.
Research files: tools/lc/occlusion_probe.c, occlusion_ref.c, occlusion_ref.h.
No throughput or physical-Falcon speed-up claim. Applies particularly to
CPU-bound scenes: compare full pipeline cost, not only group counts.
Not yet in this copy: consult games/sciv/AGENTS.md and analysis/list-compositor-sciv.md when taking over the DSP migration. No renderer code was transferred.

## 2026-10-01 — F030SCIV: raw page DMA, LC CPU regression and DSP memory guard

Stage-1 scripted idle scene, roughly 900 NMIs: staged whole-frame DSP submitted
37 pictures, CPU-planned LC presented 2, new raw-page whole DSP submitted 36.
Fewer composed LC groups did not establish a speed-up; CPU dirty planning,
128 KiB WRAM snapshots and page copies overwhelmed picture updates. SCIV now
defaults LC=0 pending DSP-owned dirty planning. These are completed-picture
counters, not moving-gameplay FPS. Raw DMA did not improve the old whole-frame
cadence. Patched Hatari 3eea7b07, build/src/hatari.exe, Oct 1 2026;
SHA256 600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf.
No physical-Falcon claim. Full details: games/sciv/analysis/list-compositor-sciv.md.

RAWPAGE/RAWPLANE headers (page, P address) go through the host port. Ready reply,
512 raw little-endian VRAM bytes directly by DMA from the existing render copy,
done reply. DSP swaps tile words/packs Mode 7. Removed SSI staging buffer and
CPU page packing; output still goes via SSI/DMA. Do not request another SSI
receive slot after the last one: an outstanding request can consume the next
window's first word. Playback helper extracted unchanged to falcon/ssi_play.s
for runtime and DSP tests. LC kernels and SSI-disabled builds keep host uploads.
Actual DSP: 36 frames, zero differences, including stage 1/windows/blend/Mode 7;
8,500-frame scripted run completes, final 28,672-word snapshot matches reference.
PPU replay/page discovery and stable render-VRAM synchronisation still use CPU.

Assembler success does not prove safe DSP memory allocation: external P aliases
Y/X data. Added ppu.asm image guard rejecting main code beyond P:$0C00 (sprite
rows) and window code beyond P:$5500 (VRAM cache). Reusable pitfall for all ports.
Not yet in this copy: take over src/smkx_ppu.c, src/ppu_dsp.h, falcon/dsp/ppu.asm, falcon/ssi_play.s, falcon/smkx_m7dsp.s, tools/smkx/dsp_asm.py and adapt Makefile; tests src/pputest.c and falcon/pputest.s. No code copied into this port.

## 2026-10-01 — F030SCIV: 25 Hz budget still blocked by whole-frame work

Captured stationary stage-1 PPUTEST frame 0: DSP drawing plus framebuffer DMA
alone takes 8,811/38,400 s = 229.45 ms in patched Hatari 3eea7b07 (Oct 1 2026,
build/src/hatari.exe; SHA256 600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf).
Thus this kernel's measured complete redraw cannot meet a 40 ms/25 Hz budget,
even if CPU preparation became free. This timing combines composition and
output; it is not an isolated SSI bandwidth ceiling or physical-Falcon result.

Raw-DMA runtime samples NMI 6089–6990: 36 submissions / 901 VBLs. Per submission
planning-phase elapsed time 392.67 ms, page-transfer phase 33.74 ms, command-send
phase 4.57 ms. These include game preemption/scheduling, are not CPU-exclusive
costs, and must not be added to the standalone DSP timing (work overlaps).
Whole-frame output is 57,344 bytes, or 1,433,600 bytes/s at 25 Hz; CPU replay/page
discovery and full DSP redraw/output both need removal for stationary scenes.
Retain DSP graphics/dependencies and output only affected groups; the timer,
flames and idle sprites still change while the background remains stationary.
No additional code fix in this diagnosis; already present default is raw DMA
whole-frame, not DSP-owned retained dirty planning. Applies to idle/static scenes
in this port: eliminating CPU copies alone cannot deliver 25 Hz.

## From F030SMW, 2026-10-01: M7DIRTY page index with BFEXTU

Changed falcon/smkx_rt.s: replaced LSR.L #8 followed by LSR.L #1 in
M7DIRTY with BFEXTU d4{0:23},d4, extracting the high 23 bits as the
unsigned byte-offset / 512 page index. The macro documents d4 as clobbered;
its callers use the result only as an index, not arithmetic flags.
This is an instruction simplification, not a measured performance gain.
Not yet in this copy: take over the M7DIRTY hunk in falcon/smkx_rt.s
if this copy has the same two long shifts. No game-specific implication.

## From F030SMW, 2026-10-01: bit-field extraction in I/O reads

Changed falcon/smkx_rt.s rt_io_read: VRAM low/high reads use
BFEXTU rt_vmadd{0:16},d4 and a long scaled index (a0,d4.l*2),
replacing zero-extension plus doubling. WRAM port reads use
BFEXTU rt_wmadd{15:17},d4 instead of MOVE.L plus ANDI.L #$1ffff.
Recognised while reviewing the read side after the M7DIRTY change;
unsigned extraction retains the upper-half VRAM addressing fix.
68030 assembly checked; no runtime or throughput measurement made.
Not yet in this copy: take over these rt_io_read hunks in
falcon/smkx_rt.s if equivalent paths exist. No game-specific implication.

## From F030SCIV, 2026-10-01: 68030 VRAM write bitmap

Changed falcon/smkx_rt.s: M7DIRTY uses BFEXTU d4{16:7},d4 to
extract the unsigned 512-byte page index, then BFSET (a0){d4:1}.
rt_m7_mark uses BFEXTU d6{17:7},d2 and BFSET for DMA ranges;
rt_vram_sync uses BFCLR (a2){d1:1} and its old-bit Z flag to test
and clear the page before copying. rt_vr_dirty is now 16 bytes
for 128 MSB-first bits, rather than 128 byte flags. All consumers
must change together. Full offset registers must be zero-extended:
BFSET uses their full 32-bit value, unlike word-indexed byte stores.
Recognised while implementing the requested bitmap: the DMA marker's
old MOVE.W/LSR.W sequence left stale upper bits, so it cannot supply
a dynamic bit-field offset unchanged. BFEXTU fixes that hazard.
68030 assembly checked with vasm; no runtime or throughput measurement.
Bit-field setting still reads/modifies/writes memory; smaller storage
is not evidence of a speed-up. CPU tracks writes only, not visibility;
the existing render-VRAM copy and CPU page discovery remain.
Not yet in this copy: take over all bitmap producer/consumer hunks in falcon/smkx_rt.s.
No game-specific implication; applies where these runtime paths exist.


## From F030SCIV, 2026-10-01: 68030 translator field extraction and BIT tests

Found while improving translator instruction selection. tools/smkx/emit.py
uses BFEXTU Dn{8:16},Dn instead of ROR.L #8 / ANDI.L #$ffff for a page index,
and BFEXTU d7{26:2},d6 (or d7) instead of copy/shift/mask for PLP M/X dispatch.
Bit-field offsets count from the MSB; extraction preserves X, but N is the
field's top bit. Use only where those intermediate flags are overwritten or
restored. Keep the single ANDI.L low-byte mask. Immediate single-bit BIT now
uses BTST on d0 with Z as its only clobber/produced flag, preserving N and X
and eliminating live-N CCR merges. SMKX_EMIT_OFF=bitfield restores old code.

A contiguous-mask BFTST candidate was smaller but slower and was removed.
Two identical 2,000,000-iteration microbench runs including SUBQ/BPL control:
page-index loop 716 -> 581 ticks; width selector 447 -> 202; BIT #$80
268 -> 226; rejected BIT #$3c BFTST 318 -> 552 (200 Hz ticks).
Required patched Hatari build/src/hatari.exe, HEAD 3eea7b07, Oct 1 2026,
SHA256 600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf;
Falcon 16 MHz, cpu-exact/compatible, TOS 4.04, 14 MiB ST-RAM. These are
cache/placement-dependent emulator loop measurements, not physical Falcon
results or gameplay-throughput gains. SCIV generated text shrank 18,902 bytes
(1.76%), 5,095 instruction lines, same 92,882 analysed nodes.

Changed files: tools/smkx/emit.py, tools/emit_tests.py, tools/emit_bench.py;
record: games/sciv/analysis/translator-68030.md. Model suite: 82,945 passing
cases, including every P/CCR combination, all BIT live-flag subsets, poisoned
upper address bytes, bank $FF, carry preservation and page-end slow paths.
6,000-frame intro/attract smoke run with rendering only at its final snapshot
reached its limit; all 28,672 snapshot words match the reference. Bench caches
identical assemblies and resolves the Windows vasm .exe suffix.
Not yet in this copy: take over the named emitter hunks and tests/bench
after checking against this port's runtime; other code was not edited here.
For this game: Applicable to pointer/table traffic and immediate BIT tests; game and DSP rendering budgets must still be measured separately.

## From F030CT, 2026-10-06: the render context runs at once when the DSP idles

Found in CT's VBL hook (`falcon/smkx_rt.s`, `rt_vbl`). The render context
(CT's render thread: planning, uploads and the send in the game's idle time)
got the CPU only at the game's idle marks or after `RWAIT` (3) VBLs, and a
context waiting for the DSP was left out of that count. When the VBL's poll
took the finished picture, the context's pending send, or the plan the poll
had asked for, waited for an idle mark while the DSP idled. Recognised in
the 200 Hz tick table ("SMKX dsp ticks"): "asked" and the render phases
"held" (the game has the CPU) while the DSP has nothing in flight; CT's
2026-09-27 table had 12% there. Fix: after its frame, the VBL switches to
the render context at once when work is pending (`rt_rstate`), the context
does not wait for the DSP (`smkx_render_waiting`) and `smkx_inflight` is
0; otherwise the `RWAIT` count as before. A VBL over the render context
still returns to the game, so the game keeps its frame. File:
`falcon/smkx_rt.s` (`rt_vbl`, label `.now`).

Result (attract demo, 12,000 frames, release build, `CTX_LC=0`, whole-frame
DSP renderer; Hatari v2.6.1-devel Oct 1 2026, `build/src/hatari.exe`,
SHA-256 `600ac868ea64c9deea6fa72731b9fded58b45fb930d607ecb43bc7d9109682cf`,
emulated): pictures 1,227 -> 1,390 (+13%); per scene 9.1 -> 11.1 (Mode 7
intro), 3.6 -> 6.3, 3.0 -> 4.7, 3.2 -> 3.9 pictures a second, each at 50
game fps; game NMIs 10,025 -> 9,971 (-0.5%). Details: F030CT
`analysis/ct.md`, "The render context at once when the DSP idles".
Not in this copy: no render context (the VBL plans synchronously); it applies when CT's render context is taken over.
For this game: Mode 7 scenes gained most in CT (9.1 -> 11.1 pictures a second); the gain is where the game runs without idle marks while the DSP is idle.

## From F030CT, 2026-10-06: building and running natively on Windows

Git for Windows' GNU make 4.4.1 (`/usr/bin/make` in this shell) hands its
recipes none of the shell's exported environment: TMP, TEMP, DOSBOX and
PYTHONHASHSEED all arrive empty. Recognised by the mingw64 cross gcc's
"Cannot create temporary file in C:\WINDOWS\: Permission denied" (also
with an MSYS-style TMPDIR) and dsp_asm's WinError 2 although DOSBOX was
exported. Fix: pass them as make variables, which make does export
(`scripts/make_win.sh` in CT wraps this: `CROSS_CC="m68k-atari-mintelf-gcc
-pipe"`, DOSBOX, TMP, TEMP, PYTHONHASHSEED=0, PYTHONUTF8=1); `-pipe`
keeps gcc off temporary files anyway. The ucrt64 vlink needs cygwin1.dll:
F030Arcade's `third_party/vlink/vlink.exe` and `vasm/vasmm68k_mot.exe`
(the Makefile defaults) work. `tools/smkx/__main__.py` re-executed itself
with `os.execv` for PYTHONHASHSEED=0, which on Windows joins the arguments
unquoted and split the ROM path at its space ("unrecognized arguments:
(USA).zip"): replaced by `subprocess.call`. `tools/smkx/dsp_asm.py`:
F030SCIV's DOSBOX/dosbox-staging discovery taken over, plus SDL dummy
video/audio drivers in the environment. `scripts/run_smkx.py` on Windows:
this Hatari takes no program path, so the program is staged as
`AUTO\CTX.PRG` in `build/ctx/<tag>-drive` and that directory passed,
`--debug-except none`, no command FIFO, and it stops its own Hatari PID
after "Program termination" in the trace. Files: `scripts/make_win.sh`
(new), `scripts/run_smkx.py`, `tools/smkx/__main__.py`,
`tools/smkx/dsp_asm.py`. Not yet in this copy: take over the
`__main__.py` execv fix and the run script's Windows launch; the
`dsp_asm.py` change is F030SCIV's. Tooling only; no game behaviour.

## From F030CT, 2026-10-06: profiling the DSP kernel by function inside a game run

Method (any port, any kernel). Hatari's debugger has a DSP profiler, `dp`
(`dp on`, `dp stats`, `dp symbols`, `dp cycles`, `dp save <file>`), which
counts every instruction's cycles per P address and reports hardware time
(cycles / 32 MHz) whatever speed the emulator runs at (F030Arcade's
DEBUGGING.md). CT's `scripts/dsp_profile.py` drives it without a FIFO: a
`--parse` script does `logfile`, `dspsymbols <file>` (labels and P addresses
scraped from the Motorola listing: a label on its own line takes the next
instruction's address; lines skipped by a false `if` are absent from the
listing) and two `b VBL = N :trace :once :file <script>` breakpoints that
turn it on and dump it; Hatari's VBL count is ~194 ahead of the runtime's
`vbl` status field (the TOS boot), so map the scene's frames first. The
driver stops its own Hatari PID once the saved profile exists, then sums the
per-address cycles by label range, folding local labels (`_x`) into the
global label before them (Hatari's own `dp symbols` counts only a symbol's
first address). Files: `scripts/dsp_profile.py` (new in CT). Not yet in this
copy: take it over when a per-function DSP profile is wanted; it needs only
the listing and the program.

What it found in CT's shared `ppu.asm` (whole-frame tile renderer, field
scene, BG mode 1, 384 VBLs, 43 pictures, emulated): composition
(`comp_group` and the sliver handlers, `merge`, `any_mask`, `slivers`) 28%,
SSI output service (`tx_isr` and its vectors, `fill`, `gate`, `tx_start`,
`room`) 22%, descriptors (`desc`, `layer_row`, `row`, `render`) 18%, sprites
11%, waiting for the host's next command 11%, the row loop 5%. The transmit
interrupt costs ~24 cycles a word (a long interrupt with the "sent" test);
a fast interrupt cannot replace it because the reader must test whether the
writer has reached the word (the DSP draws slower than the SSI drains): the
output service is a fixed cost per word sent, and fewer words is the lever.
In a BG mode 2 scene (CT's title) the offset-per-tile descriptors
(`desc_opt`) alone took 21%. For this game: profile its own scenes before
optimising; the shares shift with mode and sprite load, but the output
service's cost per word is the same in every port that sends over the SSI.

## From F030CT, 2026-10-07: HDDRIVER lists a big FAT16 image but TOS 4.04 will not mount it

Found putting CT on a BlueSCSI card (Falcon 030, TOS 4.04, HDDRIVER 11.01).
A raw AHDI image made the way F030Arcade's `scripts/make_gem_image.py` does
it (one BGM partition at sector 64, 240 MiB FAT16, 512-byte sectors, 4 KiB
clusters) was found by HDDRIVER and shown as `BGM, 251.7 MB`, but the drive
letter cannot be opened. Recognised in HDDRUTIL: Drive > Details of the drive
shows "-" for every BIOS (BPB) field, and Tools > Check Boot Sector lists the
volume against GEMDOS' limits, the violated ones greyed: sectors per cluster
8 (<=2), sectors 491520 (<=65535), clusters 61175 (<=32766). HDDRIVER itself
copes with big partitions; the limits are the GEMDOS above it. The README
claim that HDDRIVER "handles" >32 MB covers the BIOS, not GEMDOS.

The rules GEMDOS applies to a FAT volume: exactly 2 sectors per cluster, at
most 65535 logical sectors, at most 32766 clusters, at most 32768 bytes per
cluster, 1 or 2 FATs, at most 1008 root entries. Only the *logical* sector
size is free, so use big logical sectors: 4096 bytes x 61440 sectors, 2 per
cluster (8 KiB clusters, ~30700 clusters) for a 240 MiB partition. The AHDI
partition table does not change (491520 physical = 61440 x 4096 bytes).
Rule of thumb: logical sector S >= partition bytes / 65535, a power of two,
at most 16384; so a partition tops out at about 1 GiB (32 KiB clusters).
More space needs more partitions. Names stay upper-case 8.3.

How to make one on a Mac (mtools' `mformat -M` takes its geometry from the
file size and gives a wrong volume on an image file; use macOS' newfs_msdos):

    truncate -s $((491520*512)) part.img
    newfs_msdos -F 16 -S 4096 -c 2 -s 61440 -r 1 -e 512 -v LABEL ./part.img
    mcopy -i part.img -s -m -n files/* ::       # mtools copes with 4096-byte sectors
    # then dd it to sector 64 of the image behind the AHDI root sector

`fsck_msdos -n part.img` is clean on it. Files: `scripts/gemdos_hda.py` (new
in CT) converts an existing image in place: it copies every file out, makes
the volume again as above and copies everything back with names and times
(checked on CT's card image: all 257 names and every file's bytes identical).
Work on a copy of the image. Not yet in this copy: take over
`scripts/gemdos_hda.py` when you put an image on a card (needs mtools and
macOS' newfs_msdos; untested on Windows). F030Arcade's `make_gem_image.py`
makes the failing geometry too; it is not a port, so it was not touched here.

State at writing: the converted CT image is on the card but not yet tried on
the Falcon, so "TOS mounts it" is still to be confirmed there (look at Drive
Details: real BPB values instead of "-"). Check F030CT's git log for a follow-up.

A second thing from the same session: the BlueSCSI's own log (`log.txt` and
`lastlog.txt` on the card, `Debug = 1` in `bluescsi.ini`) says which images it
opened for which ID and logs every host selection of one of its IDs; HDDRIVER
at boot listed only SCSI ID 0, while HDDRUTIL > Tools > Device Check, run
later, found IDs 0, 1, 2 and 4, and a rescan then added the drive. Power-cycle
after changing the card's images; a file that must be off the bus is renamed
so it no longer starts with `HD<id>` (a spare 2 GB image was not the reason a
disk went missing). For this game: nothing in the code; it matters when its
`.TOS` is carried to a real Falcon.

## From F030CT, 2026-10-07: four bombs on a real Falcon: the unguarded NatFeats probe in `rt_nf_init`

Found when F030CT's `CTX.TOS` was first started on a real Falcon 030 (TOS 4.04): four bombs at once, before
anything appeared. Four bombs is TOS's count for an illegal instruction (2 bus error, 3 address error, 4 illegal
instruction). The cause is the first thing `start:` does after `Super`: `rt_nf_init` executes `dc.w $7300`
(NF_ID) with no check that an emulator is there. `$7300`/`$7301` are not valid `MOVEQ` encodings (bit 8 is set);
only an emulator with NatFeats turns them into a call, a real 68030 takes the illegal-instruction vector. The
exception catcher (`rt_catch_exceptions`) is installed after this point, so TOS drew the bombs. Every
run in this family used Hatari with `--natfeats true`, so no port had met it. Recognised by the count (4) and by the crash happening before any video change.

**Fix** (`falcon/smkx_rt.s`, `rt_nf_init` only): run the probe at `ori #$0700,sr` with the illegal-instruction
vector (`$10`) temporarily pointing at a handler that restores the stack pointer saved in `a1`, leaves
`rt_nf_stderr` at 0, and falls through to the common exit that restores the vector and SR. `rt_print` already
skips output when `rt_nf_stderr` is 0, so nothing else changes. Checked in the patched Hatari with
`--natfeats true` (log as before) and `--natfeats false` (no bombs, the game runs on to the intro picture).

Not yet in this copy: take over `rt_nf_init` from F030CT's `falcon/smkx_rt.s` (the routine and the comment above
it; the `nf_id`/`nf_call` subroutines are unchanged). The test programs (`falcon/pputest.s`, `lcdtest.s`,
`planbench.s` in the ports that have them) call `$7300` the same way; they are emulator-only and were left alone.

For this game: a real-Falcon run of its `.TOS` bombs at start until this is taken over. Other emulator-only
facilities the runtime calls without a probe would fail the same way; check for any further `dc.w $73xx`.
Nothing on the real machine reports progress (no NatFeats), so judge the first real runs by the screen.

## From F030CT, 2026-10-07: a crash report on a real Falcon (no NatFeats): CTXLOG.TXT, and the exception catcher from the start

Found while chasing F030CT's first real-Falcon crashes (after the NatFeats probe fix of the entry above, the next
failure was two bombs, a bus error). TOS shows only the bomb count, and every `rt_print` line was dropped without
NatFeats, so nothing said where the program died. Also, `rt_catch_exceptions` ran only after the video and DSP
set-up, so a fault in the start-up code (banks, pages, stacks, `smkx_video_init`, `smkx_apu_init`, `rt_video_on`)
was a TOS bomb even in Hatari.

**Change** (`falcon/smkx_rt.s` only):
- `start:` saves the Timer C, VBL and ACIA vectors and installs the exception catcher right after `rt_nf_init`
  (the later `rt_catch_exceptions` call and the three saves there are gone, the vector installs stay), so `rt_exit`
  is valid from any point below.
- Without NatFeats `rt_print` appends to a 192 KiB buffer (`rt_log_buf`, interrupts masked while it copies; the last
  8 KiB are reserved for the exception report: `rt_exc_stub` and `rt_fatal_arg` lift the limit).
- `rt_exit` calls `rt_log_flush` in user mode, just before Pterm: Fcreate/Fwrite/Fclose of `CTXLOG.TXT` in the
  current directory. Not written when NatFeats is present. A crash therefore ends as a clean quit to the desktop
  plus a file with the vector, the PC as a text offset (`scripts/smkx_pc.py` maps it through `CTX.map`), the
  registers and the stack. Checked in the patched Hatari with `--natfeats false` and a deliberate `dc.w $4afc`
  after `rt_video_on` (file written, desktop back), and with NatFeats on (no file, the normal log).
- A hang gives no file (the flush runs only at exit); a fault in `rt_m7_boot` or before `Super` is not caught.

Not yet in this copy: take over the `rt_print`/`rt_log_flush`/`rt_nf_init` hunks, the `start:` reordering, the two
`rt_log_lim` lines, `rt_exit`'s `bsr rt_log_flush`, the bss/data labels (`rt_log_*`, `log_name`, `LOG_BYTES`,
`LOG_RESERVE`) of F030CT's `falcon/smkx_rt.s`; rename the file to the port's own prefix if wanted.

For this game: the first run of a `.TOS` on a real Falcon should be done with this in place, otherwise a crash
says nothing. Read `CTXLOG.TXT` (or the port's name) from the card afterwards.

## From F030SMK, 2026-10-07: F030CT's NatFeats guard and crash log taken over

F030SMK took over the two F030CT entries above (the guarded NatFeats probe and the crash log with the early
exception catcher); the F030CT `rt_vbl` "render context at once" change was skipped, there is no render context or
RWAIT here.

**Changed:** `falcon/smkx_rt.s` only.
- `rt_nf_init` probes `$7300` at `ori #$0700,sr` under a temporary illegal-instruction vector (`$10`); without
  NatFeats `rt_nf_stderr` stays 0 and `rt_print` collects text into `rt_log_buf` (192 KiB, last 8 KiB reserved for
  the exception report).
- `start:` saves the Timer C, VBL and ACIA vectors and calls `rt_catch_exceptions` right after `rt_nf_init`; the
  later catcher call and saves are gone (the vector installs stay), so `rt_exit` is valid from any point after
  `Super`. `rt_exc_stub` and `rt_fatal_arg` lift the buffer limit; `rt_exit` calls `rt_log_flush` before Pterm.
- The crash log file is `SMKXLOG.TXT` (current directory), written only when NatFeats is absent.
- Method: `patch -p1 -F3` with F030FFVI's hunks (minus the `rt_vbl` one); the `start:` and `log_name` hunks needed
  by hand (this port's `start:` has `SMKX_PACE` lines in between, and the data block has a different layout).

**Result:** `make smkx` links (only the usual linker warnings 22/48/64); a 90 s `scripts/run_smkx.py --fast
--program build/smkx/SMKX.TOS` run in the patched Hatari (`--natfeats true`, as the script always uses) ran past
frame $866 (the intro into the title/menu states) with no exception report. Not run with `--natfeats false`, not
run on a real Falcon, so the file-writing path is not exercised here (a Hatari timeout kill skips `rt_exit`).

Remaining `dc.w $73xx` NatFeats calls are in the test programs only (`pputest.s`, `m7test.s`, `ssitest.s`,
`updtest.s`) and the guarded `nf_id`/`nf_call` in `smkx_rt.s`; left alone.

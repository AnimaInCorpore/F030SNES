# DSP-owned dirty groups: compose and send only what changed

2026-10-06. Design, before any code. Chosen over planner speed and kernel
micro-optimisations after the DSP profile of the same day (`ct.md`, "Where the
DSP's cycles go"): in a field scene composition takes 28% of the DSP's time and
the SSI output 22%, both proportional to the groups composed and sent;
descriptors 18%, sprites 11% and the row loop 5% are per row. The whole-frame
renderer composes all 3,584 groups of every picture; the list compositor
composed only the groups its planner named, but that planner cost 22-95 ms of
68030 time a picture (`list-compositor-ct.md`) and showed fewer pictures than
the whole-frame renderer. Here the DSP decides the groups itself, from what it
already has: the VRAM pages the host uploaded, the descriptors it builds per
sliver, the sprite masks per group. The host adds one word per row (what
changed in the registers, the HDMA scroll and the OAM, which it knows from its
two streams) and a buffer number, and afterwards walks the list into the
screen buffer. No planner change; `dsp_plan` and the uploads stay as they are.

## What exists and is reused

- The LC build of `falcon/dsp/ppu.asm` (`LCB=1`): `compose` composes only the
  groups of the row's mask (`RMASK`, shifted per group), `blank_row` sends
  eight zero words per set bit, `lc_range` limits the slivers a partial row
  builds, rows with an empty mask are skipped. Its masks come from the host
  (`DSP_CMD_LCMASK`, 224 words at X:$0500). The output is a list: the groups
  in row order, eight words each, no headers (the host knows the masks).
- `smkx_ppu_send_lc` (`src/smkx_ppu.c`): pages over the host port, LCMASK, the
  stream, DMA record armed on a list buffer of a known length.
- `dsp_rows` (`src/ppu_dsp_plan.c`): the host's own light replay comparing
  every row's registers, HDMA state, OAM segment and CGRAM with the last
  committed picture's, and which VRAM pages were written. `PartialState`
  (`src/ppu_rows.h`) keeps the two screen buffers' picture versions and per
  row the version that last changed it, so that a picture drawn into the
  older buffer also redraws what the newer one changed. The true-colour path
  (`src/smkx_tc.c`) runs on this at row granularity; the eight-plane path has
  no partial rendering at all (the field profile composed 196 of 224 rows a
  picture).
- `cmd_page`/`cmd_page24`: every upload goes through them with its P address.
- The screen buffer is group-major: a group's eight plane words are
  consecutive (16 bytes), a row is 256 bytes. Scattering a group is one
  16-byte copy to `row * 256 + g * 16`.

## Protocol (the DG build, `DGB=1`, built from `LCB=1`)

Per picture, after the page uploads:

1. `DSP_CMD_DGFLAGS` (replaces LCMASK): 224 words, one per row:
   - bit 0: everything of this row differs (registers other than scroll, the
     BG mode, TM, OBSEL, a palette-block change the pixels depend on, forced
     blank on or off, the first picture into this buffer, a DSP reboot): all
     16 groups.
   - bits 1-4: layer L's scroll or configuration differs on this row
     (HDMA per row included): all groups of that layer are dirty.
   - bit 5: the sprites differ (another OAM segment, OBSEL, or OAM bytes
     that differ for a sprite in range of this row): the groups sprites cover
     in this picture or covered in the previous one.
   - bit 6: the row is blank in both pictures: nothing to send.
   The host computes the word from `dsp_rows`' span states of the two
   pictures (it compares exactly these things already, per row, into one
   dirty bit; the change is to keep the cause per row).
2. `DSP_CMD_FRAME`, the stream as now, with one extra word after the count:
   the buffer number (0/1) the picture goes into.
3. Output: per row with any group to send, the row number | mask << 8 as one
   word, then the dirty groups' eight words each, in group order. The host
   arms DMA record for the worst case (224 x 129 words) and reads the list
   up to the terminator word $FFFFFF the DSP sends last.

## The DSP's side

- `DIRTYP[2]`: per buffer, a bitmap of the 128 P pages (six 24-bit words
  each); `cmd_page`/`cmd_page24` set the page's bit in both. `FRAME` selects
  the buffer's bitmap for the picture and clears it when the picture is
  done. So "written since this buffer's picture" needs no host bookkeeping,
  and a page uploaded for the other buffer's picture stays dirty for this one.
- `desc`, building sliver s of layer L (it reads the map entry at a P address
  and computes DA, the character row's P address): the sliver is dirty when
  DIRTYP has the map page's bit or the character page's bit (two bit tests,
  ~10 cycles; `L_CHR + chr * 16` never crosses a page, DA's page is the
  character's). A dirty sliver sets the groups it feeds in the layer's group
  mask word `LGM[L]`: groups s >> 1 and (s - 1) >> 1 (s = 2g, 2g + 1, 2g + 2
  feed group g). The word is kept with the block: the rows that step the
  descriptors (same keys) keep it, a rebuild starts it at 0. A row whose keys
  differ from the previous row's within the picture is rebuilt anyway.
- `obj_row` builds the row's sprites; its OM (the mask per priority and
  group) gives the coverage `COV` (OR over the four priorities, one bit per
  group). A sprite whose character page is dirty marks its groups too
  (`obj_sprite` knows the page: the same two tests). `PREVCOV[224]` keeps the
  previous picture's coverage per row; it is replaced by COV after the row.
- The row's mask: flag bit 0 -> $FFFF; else OR of (`LGM[L]` or $FFFF when bit
  L is set) over the layers TM shows, | (bit 5 ? COV | PREVCOV : the dirty
  sprites' groups). A blank row: bit 6 -> nothing; else $FFFF of zeros as
  `blank_row` does now.
- Then `compose` as in the LC build with that mask; the row header word goes
  into the ring before the groups; the terminator after the last row.
- Rows with an empty mask skip `compose` but not `layer_row` (the descriptor
  keys and steps are what makes the next row's flags valid; `slivers` and the
  any-masks are only needed when something is composed: skip them when the
  mask is 0). `obj_row` must run for COV.

Memory: `DGFLAGS` at X:$0500 (the LC build's MASKS), `PREVCOV` at X:$05E0
(224), `DIRTYP` 2 x 6, `LGM` 4 and a few variables after it: all inside
X:$0500-$06EF, the end of the stream's room (the stream may hold 1,024
words, `DSP_LC_STREAM_MAX`; frames the planner builds for the DG kernel are
limited like the LC build's). All 93 page slots stay.

## The host's side

- `smkx_ppu_plan` plans for `DSP_KERNEL_DG` when the frame needs no
  windows (the LCW variant comes later) and the stream fits; otherwise the
  old whole-frame programs as now (a switch of program reboots the DSP and
  empties its page cache and DIRTYP: the next picture is flagged all-dirty).
- Per screen buffer the host keeps which picture it holds (`PartialState`'s
  versions serve) and the `dsp_rows` span state of that picture; the row
  flags compare the new picture's spans with the target buffer's picture.
  Unknown (a buffer never drawn by this path, a reboot): all rows bit 0.
- `smkx_ppu_send_dg(flags, list)`: pages, DGFLAGS, FRAME with the buffer
  number, DMA record on the list.
- When the record is done (`smkx_ppu_done`, the terminator found), the walk:
  for each header, copy the groups to `row * 256 + g * 16` of the buffer
  (`movem.l` 16 bytes a group; a run of consecutive groups is one copy). Then
  the buffer holds the complete picture; the flip is as now. Cost: ~1 us a
  group, so under 4 ms even for a whole picture, well under the planner.
- Pictures with nothing to send (every row 0) are skipped as the TC path
  skips them (`smkx_ppu_skipped`).

## What it costs and what it gives

Per picture the DSP still runs `layer_row`, `desc` (per block, amortised)
and `obj_row` for every row: about 35-40% of today's field picture. Composition
and output scale with the dirty groups. A static field with idle sprites and
a few animated tiles has tens to a few hundred dirty groups: the DSP time per
picture falls to roughly 40% of today's, and the planner (60-100 ms a plan in
the field, `ct.md`) becomes the limit at 10-15 pictures a second. A scrolling
scene dirties every group of the scrolling layers (bit L): no gain, no loss
beyond the headers (224 words) and the walk. Menus and dialogue boxes gain
most. Measured on nothing yet.

## Correctness

The exact renderer stays (`keep-exact-renderer`): the DG kernel composes a
group exactly as the whole-frame kernel does; what it adds is the decision
not to compose. The decision is wrong only if a group's inputs changed and no
flag said so: the tests must attack that.

- `LCDTEST`-style: the DG kernel in Hatari on frame sequences from the demo
  (`SMKX_SNAP_FROM` windows, consecutive frames), the buffer accumulated on
  the host side of the test, compared with the host reference after every
  frame; both buffers alternately, so the two-generation path is covered.
- `tools/lc/lcd_fuzz.c`'s approach: one frame mutated per step (a VRAM word
  in a page, a scroll register, an OAM entry, a palette block) with the page
  flagged as the runtime would flag it: 0 wrong pixels over thousands of
  steps, and a mutation build that drops one flag source must fail.
- In the game: `scripts/lc_snaps.sh` / `lc_check.py` as for the compositor
  (synchronous snapshots compared with the host reference, every frame over a
  scene, then every 8th frame asynchronously).

## Milestones

1. Kernel: `DGB` build switch on the LC build: DIRTYP from uploads, sliver
   flags in `desc`, LGM per layer, COV/PREVCOV in `obj_row`, the row mask,
   headers and terminator. PPUTEST with all rows flagged all-dirty must stay
   exact (it is then the LC build with masks of $FFFF).
2. Host test tool `tools/dgtest` (the fuzz and the sequence test above)
   against the DSP in Hatari, like `LCDTEST`.
3. Runtime: `DSP_KERNEL_DG` in the plan, flags from `dsp_rows`' spans, send,
   walk, skip; snapshots in the game.
4. Measure: pictures a second per scene against `ct.md`'s table, the DSP
   profile by function again, the planner's share.
5. Windows (`DGW`), then colour math as the whole-frame kernel has it.

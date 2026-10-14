# List compositor: a retained frame buffer updated by a DSP-made list

2026-09-30. Design record and host-model results. Nothing here runs on the
Falcon yet. The exact renderer (`falcon/dsp/ppu.asm`, eight planes, CGRAM
indices 1:1, bit-exact against `src/snes_ppu.c`) stays as it is: it is the
reference, the fallback and the kernel for background regions. Everything
below is an additional, selectable path that must reproduce its output.

## Why

`dsp-renderer-budget.md`: the exact renderer needs 165-205 ms per frame on
the DSP against a 76 ms SSI transport floor for a 57,344-byte frame. Most of
CT's picture does not change from one frame to the next, so the cost of a
frame should follow what moved, not the resolution.

## The scheme

1. The CPU streams PPU state changes (OAM, registers with scanline stamps,
   VRAM/CGRAM deltas) to the DSP. The DSP keeps, per Videl page, what that page
   shows (the pieces it drew, the clean background it was drawn over).
2. For a new frame the DSP works out which 16-pixel plane groups of which
   rows are dirty: groups whose clean background changed, plus the footprint of
   every sprite row piece that appeared, vanished or changed (hash of where it
   draws and what). It emits a list.
3. The 68030 walks the list: restore dirty groups from the clean background
   (RLE runs, Chorensha style), then draw the pieces that touch them with
   blitter rows, clipped to the dirty groups. Nothing else is touched.
4. The pieces are disjoint per pixel (the DSP resolves sprite order and
   BG-over-sprite priority, which it computes in the exact renderer anyway), so
   the blit order does not matter.
5. Pages are a virtual screen (see below), so unclipped sprite overhang
   lands in margins.

The list is small (hundreds of bytes to a few KB), so it can travel over the
host port (proven in `chorensha`, 4000 RLE values per frame) and leaves the SSI
to audio (ROADMAP item 6).

## Prior art in this machine's other projects

- `../../../F030Arcade/third_party/chorensha/sources/atari/dsprite.asm`, `graphics.s`:
  the DSP keeps packed 1-bpp sprite masks, a 1-bpp dirty bitmap, extracts RLE
  runs and sends each run as a jump displacement into an unrolled
  `move.l (a2)+,(a3)+` restore block on the CPU. Old/new sprite-info lists swap
  per frame. Work is time-sliced against Timer B.
- `../../../f030fastbobs/src/main.s`: Falcon blitter bobs in eight planes. The 1-bpp
  mask rows become the blitter endmasks, the blitter "Y count" steps through the
  eight planes, Skew does the pixel shift. `blit_fastest` is the tuned variant.
- `../../../stefastbobs2/main.s`, `sprite_compiler.js`: the STE four-plane peak
  ("hardwired" code per shape and per x&15: endmask immediates, a constant
  address `add`, Y-count poke and start poke per row, register addresses and
  constants preloaded, rows in zig-zag order). STE/68000 cycle comments only.
- `../../../F030Arcade/games/nemesis/src/gfx.s`: the persistent hardware-scrolled
  ring (virtual screen, duplicate for the linear fetch, base/HDB scroll,
  dirty-cell grid with a repaint countdown per physical page, ring-seam
  redraw, margins absorb all overhang). Its 16-bit; the eight-plane planar
  scroll registers need their own check.

## Blitter-native data

Sprites and tiles are pre-converted by the DSP (or CPU) into what the blitter
reads directly:

- Rows are plane-major: `plane0: W words | plane1: W words | ... plane7`. One
  blitter start per sprite row covers all eight planes (X count W, +1 when
  x&15 != 0; Y count 8; destination Y increment steps through a group's eight
  interleaved plane words). Skew does the shift, so there are no pre-shifted
  variants.
- The 1-bpp row mask becomes endmask 1+2 (one long write) and endmask 3. A blit
  row can only express the first and last word's mask freely; middle words are
  full. The model's `count_blits` applies exactly that rule.
- Pixel values are the CGRAM indices the exact renderer writes (BG: pal*16+c,
  sprites: 128+pal*16+c), so the result compares bit for bit. Transparency is
  the raw SNES colour 0, not the final index.
- H-flip is baked (bit reverse); V-flip walks the source rows with a negative
  stride.
- The DSP can emit, per blitter row, the register images (endmask 1+2,
  endmask 3, destination address, counts, control word with skew): 20 bytes
  per row (source and destination address, X count, source and destination Y
  increment, endmask 1 and 3, skew and flags; the model builds exactly these
  records, see below). The CPU loop is then
  `movem.l` of the record plus a handful of pokes, which fits the 68030's
  256-byte instruction cache. Compiled per-shape code (the stefastbobs2
  method) is only an option: straight-line generated code never hits the
  instruction cache, evicts the hot loops, and the 68000 cycle counts do not
  carry over. `DEBUGGING.md` records the same for generated per-tile code. It
  needs the hardware measurements below before anyone writes it.
- Tiles are opaque: no shape to hardwire. Restores and background updates are
  linear copies (a row's eight plane words are contiguous): one blitter
  rectangle or unrolled long moves.

### The blitter rows, as the planar model validates them

A piece row is drawn from a source row that holds its visible span from the
leftmost visible pixel `p0`, unshifted: `W = ceil(width / 16)` words per plane,
plane after plane (what a cache entry holds for a sprite row). The blitter's
skew is `s = p0 & 15`; the destination groups start at `gd0 = p0 >> 4`. A run
of destination groups `a..b` (`n` of them, `jj_a = a - gd0`, `jj_b = b - gd0`)
is one blit with HOP 2, OP 3, Y count 8 (the planes):

- `s = 0`: no FXSR or NFSR; `nreads = n`, first source word `jj_a`.
- `s > 0`: destination word j takes the tail of source word j-1 and the head of
  word j. `FXSR = jj_a > 0` (read the word before the run), `NFSR = jj_b == W`
  (the last destination word holds only a tail; no source word exists),
  `nreads = n + FXSR - NFSR`, first source word `jj_a - FXSR`.
- X count `n`; source X increment 2, source Y increment `2 * (W - nreads + 1)`;
  destination X increment 16 (next group, same plane), destination Y increment
  `2 - 16 * (n - 1)` (next plane's word of the run's first group).
- Endmask 1 is the first group's mask (also for a one-word run), endmask 3 the
  last group's, endmask 2 is all ones: the groups between must be full
  (`decompose()` cuts runs so). Bits outside the masks are noise in the source
  and leftovers in the blitter's shifter, and never reach the page.
- A run that would be one word with NFSR (the blitter's single-word NFSR case,
  which the Hatari authors reverse engineered) is widened by one group to its
  left with a zero mask (read-modify-write, unchanged).

For the shifted 32-pixel sprite these are the values `f030fastbobs`'s
`blit_fast` sets (X count 3, NFSR, source Y increment 2, destination Y
increment -16+2-16 = -30), so the model and that code agree on the register
arithmetic.

### The source row cache

The source row above is what the DSP would have to send for every blitter row,
every update. It is content-addressed instead. A sprite row's key is the pixel
pattern of its visible span (invisible pixels as 0; a visible index is never 0),
so the same row found again at another x, in another frame or in another sprite
slot is the same entry. An overlay layer's entry is the layer's own whole window
row whatever is visible of it (272 bytes), so that it stays the same when the
layers beneath move. The shift (skew) and the masks live in the blit record, not
in the entry. The DSP keeps the table of keys
and addresses and sends an entry (16 bytes per source word: 16 x W) only on a
miss; the CPU stores it in the cache region and the blit records point at it.
The model (`LC_CACHE_KB=n`) uses a bump allocator that is emptied at the start
of an update when less than a quarter is left: no eviction policy, so the hit
rates are a floor. The entry content stays noise where a pixel is not visible;
on a hit the entry is checked to hold every visible pixel of the row.

### Layer rings

An overlay layer that scrolls cannot live in the source row cache (its rows
slide under the row's start every frame), so it gets a persistent ring of its
own, like the clean ring: world-aligned at the layer's own scroll and laid out
like a page (group-interleaved planes), with the first 24 groups duplicated
behind the ring so a source run that crosses its end reads on. Each frame the
groups of the layer's window that differ from the ring are rewritten, which is
all the DSP sends for the layer (16 bytes each); a fixed layer sends nothing
after its first frame. The layer's piece rows are then blitted from the ring
(`LC_OVERLAY=ring`), no source row is built:

- A layer pixel at layer-world x lands at x + delta on the page, delta = Wx -
  Wx_layer = 16 q + r. Destination word c takes the head of the ring's group
  c - q and the tail of group c - q - 1, skew r.
- The ring holds real content beyond the window, so the no-final-read case is
  never needed: the run reads one word before its first destination word (FXSR,
  when r > 0) and one per destination word. There is no single-word NFSR special
  case on this path.
- The source is group-interleaved like the page: source X increment 16, one
  line per plane, source Y increment `2 - 16 * (reads - 1)`; destination as for
  any run. Masks, runs and the dirty logic are the piece's as before; only the
  source differs.

### The list, packed

The blit records were 20 bytes a run (source, destination, X count, both Y
increments, both masks, skew and flags). Almost all of it follows from little:
the increments from the run's size, the flags from the skew, the addresses from
the row. A packet per piece row (the exact layout is in `lc_model.c`):

- **Header.** An entry row (source row in the cache: sprites, rows afresh): 7
  bytes, the row's first group, the source row's number, and its width, first
  group and skew. A ring row (overlay layer blitted from its ring): 6 bytes for
  the first row of a layer, **1 byte for each next row**: it stands for the
  layer's next row, where both the destination and the source group advance by
  one ring row and the skew stays (59 full headers in 12,449 ring rows).
- **Each run.** One byte: adjacent to the previous run, the type of each of its
  two masks (full, one run of ones, anything else), the length; a byte with the
  first group when it is not adjacent; then the masks that are not full: one byte
  for a mask that is one run of ones (leftmost pixel, length; 62 to 92 % of the
  partial masks are), two bytes for any other.
- **Not in the list.** The duplicate copies (the CPU's restore loop copies a
  group below the duplicate limit twice and syncs what a blit wrote there, by
  rule); em2, the Y counts, HOP/OP and the X increments (set once per pass).
  Restore runs stay at 3 bytes each (a gap and a length).
- The CPU's side is a routine per record kind, picked by the first byte, which
  derives the increments and flags with a few instructions (Chorensha's jump
  displacement trick applied to records); the loop stays small enough for the
  instruction cache.

The model writes every packet into the update's record stream, reads it back
from the bytes the way the CPU would, derives the registers again and compares
them with the ones that were executed (the derivation is one shared function, so
the check proves that the bytes carry everything). It runs on every blit of every
test, including `blitfuzz`.

### Sprite instance packets

The entry rows of a sprite cost about 10 bytes each (7 of header), and a sprite
has 16 or 32 of them. `LC_INSTANCE=1` groups the sprite rows of an update per
OAM entry (the blits may run in that order: their masks are disjoint) and sends
each instance, the consecutive rows of one entry drawn this update, as one
packet:

- **call**: `0xC0`, program number (2), first row's destination group (2), G0
  (1): 6 bytes for the whole sprite;
- **define**: `0xC1` (kept) or `0xC2` (run once), rows - 1, destination group,
  G0, then the rows: runs - 1 (1), source row number (2), W | first group - G0 |
  skew (2), the runs as before with their first groups counted from G0. 5 bytes
  of header and about 5 a row, against 7 a row.

G0 is the first destination group of the instance's leftmost row. A program is
the rows relative to it, so one program serves the sprite at every x with the same
shift (the skew is in it) and at every y (the rows follow each other by one ring
row); it is what a stefastbobs2 routine is, as a record the cache-resident CPU loop
interprets, so no code is generated. The programs are kept in a store that is
emptied with the source row cache (a program holds source row numbers) and when it
fills (no eviction policy: flush, so the hit rates are a floor). A program that
was defined while the cache flushed under it is run once and not kept (`0xC2`).
The CPU side is modelled as a store of its own, filled from the bytes of the define
packets; every packet is decoded from its bytes and every register compared with
the one that was executed, as for all the records. (In the model a program is
found by the rows the update just generated; the DSP would look it up by the
sprite's image, shift and mask structure without building the rows, which is where
the flush coupling matters.)

Two variants of the instance packets, both exact:

- **The program store in slots** (`LC_PROG_LRU=1`): four slot sizes (128, 256, 512,
  1,024 bytes) sharing the store 3:3:2:1, a program in the smallest slot that holds
  it, and a policy when none is free: the least recently used program of that size
  (`lru`), a random one (`random`), or none (`bypass`: the program runs once and
  nothing is evicted). The define names its slot (`0xC3`, 7 bytes of header); the
  CPU side keeps the bytes per slot. A slot wastes the room between the program and
  its size, which a store that is just emptied does not.
- **Whole instances** (`LC_INSTANCE=2`): an instance that touches a dirty group is
  drawn whole and unclipped. Drawing a piece's visible pixels over groups that are
  not dirty changes nothing there (they already hold exactly those values), so
  clipping to the dirty groups only saves blitter work, and costs programs: the
  clipped rows have other masks.

### Could the DSP write the blitter's code instead?

It can (the CPU copies the words from the host port into a buffer, clears the
instruction cache and calls it), and stefastbobs2 shows the fast end of it: code
per sprite shape and shift, the destination relative to a base, constants held in
registers, a few pokes per row. It depends on what changes:

- **Code written afresh each frame is larger than a record.** A run with
  immediates is about 50 bytes (a long written to a blitter register alone is 8),
  with registers holding the constants about 25, against 20 bytes as a record
  before and 1 to 5 as a packed run. It saves the decoding and costs more to
  transfer (host port reads are 16 cycles a long) and more to fetch (code runs
  once and misses the cache). For the dense overlays it does not help: their
  masks change every frame.
- **Precreated code that is reused fits where the rows are the same again.** That
  is sprites. A sprite instance is a program: its rows' records without the
  destination position. One found again costs a call (6 bytes); a new one costs
  the program once. This is built now as instance packets (above), as records and
  not as generated code; real code would be a few times larger and sit in memory
  (about 16 shift variants per image). The results below are what the records
  achieve; generated code would need the same lookups and a larger store.

## Virtual screen and hardware scrolling

A page larger than the window lets the Videl scroll over standing content
(base address, line offset, pixel shift) instead of the page being redrawn:
only newly exposed columns and rows are drawn. The model (`hsim`) builds it as
nemesis does (a ring with a duplicate for the Videl's linear fetch), with
these rules, all of them checked by simulating the Videl's own fetch from the
page memory:

- **Geometry.** A page is a ring of `RH` rows of `RWG = 32` groups (512 px),
  addressed as one linear sequence of 16-byte groups, rows back to back (a
  "helix": group u of row v is linear group `v * RWG + u`, u may run on into
  the next row). The window (256 x 224 at world position `(Wx, Wy)`, the
  scroll summed over the frames) starts at linear group
  `(Wy mod RH) * RWG + (Wx >> 4)` with pixel shift `Wx & 15`, one extra group
  fetched when that is not 0, and the line offset adds the ring's row length.
  A world point always sits at the same linear group, so standing content never
  moves. Helix positions alias only 512 px and one row apart, never inside one
  window (`RH > 224`).
- **Duplicate.** The Videl reads linearly, so a window at the ring's end reads
  past it: the first `223 * RWG + 17` groups of a page exist a second time
  behind the ring. Restores write both copies. A blit may cross the ring's end
  into the copy; the groups it wrote there are copied back to their primary
  place. Both are needed: dropping the duplicate gives millions of wrong words,
  dropping the copy back is caught in the one case that needs it (sprites on the
  window's top row while it sits at the ring's end and the window then starts
  reading the primary copy of those cells).
- **Clean ring.** One background-only ring is shared by both pages. Each frame
  the window's groups are compared with the exact background and rewritten
  where they differ, which is all a scroll step exposes: one column of groups
  per 16 px, one row per pixel row. The pixels beyond the window's edges come
  from the same background rendered with every layer shifted by 16 px.
- **Page state.** Per group: was it in this page's last window (`valid`), and
  which clean version did it take. A group entering the window, or whose clean
  content changed, is dirty. Groups outside the window are left alone, so
  leftovers are harmless until a group re-enters (then it is restored). A piece
  that changed or vanished dirties its groups inside the window.
- **Sprites.** They are screen-anchored, so a scrolling window moves every one
  of them in the ring and all are redrawn. A sprite that scrolls with the
  background (an NPC) keeps its place in the ring: its piece is unchanged and
  costs nothing.

### Which layer is on the hardware (per-layer classification)

The Videl scroll is global, so the page holds whatever scrolls rigidly
together: the **standing set S**. Everything else is blitter work, exactly as
for sprites: restore the cells its old and new footprints overlap from the clean
ring, draw what overlaps them. Model (`rframe_build`):

- **Classification.** The BG layers shown on the main screen are grouped by how
  far their effective scroll (the first visible row's registers, HDMA included)
  moved since the last frame. Every group is a candidate for S; the cost is the
  opaque pixels of the layers left over, and the cheapest group wins. The group
  of the last frame's anchor layer is kept unless it costs more than 512 pixels
  extra. The world position follows S's scroll. Nothing requires S to stay the
  same from frame to frame, or the standing layers to cover the screen: the
  clean ring is compared with the exact background of S and rewritten where it
  differs, and the pages take what changed, so a switch only costs redrawing.
- **Owners.** A pixel needs painting where the exact frame F differs from the
  background of S alone (B_S). Its owner is the sprite the rasterizer has there
  if the sprite's pixel equals F, otherwise the first overlay layer (BG1 to
  BG4) whose own picture equals F. A pixel that equals B_S needs nothing, which
  also covers colour collisions. An overlay layer is one piece per row with a
  mask of the pixels it owns, drawn like any sprite row; it has no overhang.
- **Overlays in front or behind.** An overlay in front of S is drawn where it
  is opaque. One **behind** S works as well: the comparison with F leaves only
  the pixels where S is transparent, and those are painted into the clean
  ring's holes (the model shows BG2 as an overlay behind a standing BG1 with
  exact output). What it costs is mask complexity (more blitter runs, more
  restored cells) and that the DSP must know where S is transparent, which the
  exact renderer already computes. So no layer order is blocked; the choice is
  purely one of cost. (An earlier version of these notes said a layer behind
  could not be an overlay; the model disproved it.)
- **What the choice trades.** Treating all layers as standing (the state before
  this step) keeps the blitter out of it but the BG renderer has to produce every
  group where the composite changed, about 1,200 to 1,800 groups a frame as soon
  as two dense layers move differently. With overlays the BG renderer produces
  10 to 40 groups a frame and the blitter draws the overlay layers instead. It
  is a trade, not a free win: a large overlay costs hundreds of blitter rows a
  frame and restores its footprint twice (old and new cells).

Findings from the snapshots:

- 93 of the 97 modellable snapshots drive the BG scroll registers by HDMA
  (mode 3 indirect tables to $210D/$210F/$2111, channels 1-3), yet the value is
  the same on every line of the frame. So hardware scrolling applies, but the
  scroll has to be read as the effective register values of the frame's first
  visible row, not the latched ones.
- In these scenes BG2 is the big dense layer (54,016 opaque pixels, 94 % of the
  screen's cells) and BG1 or BG3 the sparse one (352 to 3,164 cells), so the
  cheapest standing set is BG2 (plus whatever moves with it). This matches what
  the layers are in these scenes: a backdrop and a foreground.
- BG2 and BG3 of the three-layer scene (`F2D00`) read the same HDMA data blocks,
  so they scroll together by construction; a synthetic difference between them
  cannot be made and the game does not make one.
- Per-line scroll variation (waves, split screens) does not occur in these
  snapshots; such frames are skipped by the ring model and stay on the exact
  path.
- 8-plane planar scroll on the Falcon (HSCROLL, line offset, the extra group
  fetched when shifted) is assumed to behave as on the STE and must be
  verified on hardware.

## Plane partition and plane sharing (lossy, optional, later)

Not needed by the compositor and never a replacement of the exact path: a
second step that removes the read-modify-write and gives free layer blending
in the palette.

- **Partition (dual playfield).** Layers own disjoint plane sets, e.g. 5+3 planes.
  The palette is a two-dimensional table: `pal[(b<<5)|a] = a ? colA[a] : colB[b]`.
  The Videl does priority and transparency; colour math between two layers is a
  table entry. Layers redraw independently (a scrolling back layer never touches
  the front layer's planes). Cost: 31 and 7 colours, a palette reduction per
  scene (the OKLab palette machinery of the Mode 7 path is reusable).
- **Sharing a plane between layers** (the user's idea) gains colours. Give
  every layer `k` key planes of its own (opaque where the key is non-zero) and
  let `a` attribute planes be shared by all layers. A layer then has
  `(2^k - 1) * 2^a` colours with `sum(k) + a = 8`. Examples for two layers:
  (3,3,2): 28 and 28; (4,3,1): 30 and 14; (4,2,2): 60 and 12; against 31 and 7
  for the disjoint 5+3 split. Decoding: the front layer whose key is non-zero
  wins; the shared attribute bits belong to whichever layer drew last at that
  pixel.
  Costs: a shared plane couples the layers. Redrawing the back layer over the
  front layer's pixels would overwrite the front layer's attribute bits, so the
  cells must be restored and redrawn in order (nemesis' "anything standing on a
  dirty cell is requeued" pass) or the back layer's masks must exclude the front
  layer's silhouette (arbitrary masks are not expressible across blitter middle
  words). The free colour math between layers is gone for the shared bits.
- Colours are the decisive number: extend the census to count distinct RGB
  colours per layer and per scene before choosing a split.

## Host model (`tools/lc_model.c`, `make build/lc_model`, `make check-lc-model`)

For each snapshot it renders F (exact, identity map) and B (OAM hidden: every
entry moved to x = -256, since CT's HDMA and IRQ handlers write TM per line),
rasterizes the sprites itself (same sliver order and 32/34 limits as
`obj_line`) to learn each pixel's owning OAM entry, builds the per-row pieces,
and runs the page update above on two pages. Commands:

```
build/lc_model census  ct.sfc F*.BIN     cold cost, all sprites, per snapshot
build/lc_model seq     ct.sfc F*.BIN     snapshots as consecutive frames (scene changes)
build/lc_model sim     ct.sfc SNAP frames movers dx dy sx sy   synthetic motion
build/lc_model hsim    ct.sfc SNAP frames movers dx dy sx sy   the same on the virtual screen
build/lc_model dump    ct.sfc SNAP prefix                      F, B, CGRAM as raw files
build/lc_model blitfuzz 50000 1                                random pieces through the blit-row generator
```

Modelled, abstract: two pages, dirty groups from background change plus piece
footprints, RLE restore runs, blitter-row decomposition (first/last masks free,
interior groups full), list bytes at 20 B per blitter row and 4 B per restore
run, a margin group each side. The page after every update is compared with F.

Modelled, concrete: the same update executed on planar pages. Restores are
linear copies of the clean background's plane words; sprite rows are the
blitter runs above, executed by `blit_run()`, a port of the word loop in
Hatari's `blitter.c` (HOP 2, OP 3: source shifter, skew, FXSR, NFSR, three
endmasks, read-modify-write under a partial mask, the word left on the bus).
The blitter's shifter and bus word start as noise and persist from blit to
blit, unused source bits are noise, so a list that leaned on leftovers would
show as wrong pixels. After every update the planar page must equal the packed
pixel page in all groups (margins included) and `ppu_planar8()` of the exact
renderer's F in the window. `blitfuzz` draws random pieces (random position,
width, holes, pixels) under random dirty masks onto a page of noise and checks
the whole page.

Modelled, virtual screen (`hsim`): the ring pages, the clean ring, exposure and
version tracking, the duplicate, and the Videl's fetch simulated from the page
memory (start group, pixel shift, extra group, line offset) and compared with
`ppu_planar8()` of the exact frame. The scroll it is fed also patches the HDMA
tables that carry it in CT's frames (direct or indirect mode 3 tables to
$210D/$210F/$2111, each data block once), and the extension pixels beyond the
window are rendered with the same patch.

The scroll it is fed can differ per layer (`LC_LAYER_SX` / `LC_LAYER_SY`, four
values for BG1-4), which drives the classification above; `LC_STANDING=all` or a
bit mask forces the standing set, for comparisons.

Not modelled yet: the DSP's own visibility computation (it is taken from the
renderer: a sprite or overlay pixel shows where F differs from the standing
background), windows, colour math, modes 2 and 7, and any timing. The
classification's cost is a pixel count, not a time. The source row cache has no
eviction policy and keys on the visible pattern (a sprite row whose occlusion
changes misses although its underlying row is the same), so its hit rates are a
floor. The layer rings (above) are built; the choice between the row cache and a
ring per layer is not automatic, `LC_OVERLAY=ring` picks the rings. `blit_run()` follows
Hatari's source; it has not been compared with Hatari's output or a Falcon.

### Results (Hatari-independent, pure list counts)

The 199 snapshots in `build/ctx/F*.BIN` (the attract run, 64 frames apart):
97 are modelled. Skipped: 42 forced blank, 31 mode 2/7, 20 colour math, 5 TM
varies per line, 4 windows. So 20 of the 122 mode-1 snapshots need colour math
before the compositor could serve them.

- The model's rasterizer and ppu_render agree on every pixel of the 97 frames
  (0 disagreements), which validates the piece extraction.
- Cold page (every sprite drawn): mean 13.7 sprites, 146 blitter rows,
  about 3.8 KB of list; the busiest frame 38 sprites, 448 rows, 9.9 KB.
- Scene changes (all snapshots as consecutive frames): dirty 1674 of 3584
  window groups on average, blitter rows 121, list 3.5 KB, source rows built
  1.9 KB. The dirty set is within 0.5 % of the pixel-difference lower bound
  (1667 groups).
- Synthetic motion from real frames (`sim`), groups dirty of 3584, blitter rows,
  list bytes (20 B per row, 4 B per restore run), source bytes built:

| case | dirty groups | blitter rows | list bytes | source bytes |
| --- | ---: | ---: | ---: | ---: |
| nothing moves | 0 | 0 | 0 | 0 |
| one character (4 sprites), 1 px per frame | 69 | 48 | 1.1 KB | 0.8 KB |
| 12 sprites, 2 px + 1 px | 173 | 113 | 2.6 KB | 1.8 KB |
| 38-sprite frame, 10 moving | 123 | 178 | 3.7 KB | 2.8 KB |
| 38-sprite frame, all moving | 368 | 439 | 9.2 KB | 7.0 KB |

- Every case ends with 0 wrong pixels and 0 wrong planar bytes: the 95
  scene-change updates, the synthetic motion, four different noise seeds, and
  60,000 random pieces in `blitfuzz`.
- The checks fail when they should. Leaving out the old footprints gives about
  5000 wrong pixels. Five deliberately wrong blit-row generators (skew off by
  one, FXSR dropped, NFSR dropped, endmask 3 all ones, no widening of the
  single-word NFSR run) give 2,800 to 18,600 wrong pages of 20,000 in
  `blitfuzz`. The real frames alone would not have caught a missing FXSR: a
  run that starts after its piece's first group is rare in them, which is why
  the fuzz test exists.

Caveats. The snapshots are an attract run, not gameplay; the motion cases move
OAM entries by a constant step. The source-byte figures here are without the
cache (every update builds its rows afresh), so they are an upper bound on what
has to be sent; the cache results are below. No figure is a time, and none is a
Falcon measurement.

### Results: the virtual screen

The same synthetic frames (12 frames, 10 warm, means per update) on the fixed
page (`sim`) and on the ring (`hsim`). "BG groups" is what the background
renderer has to produce: the groups whose background changed on the fixed page,
the clean ring's rewritten groups on the ring (a one-pixel scroll exposes a new
column of 224 groups only every 16 frames, so short runs show 0 or a few). "Dup
groups" are the ring's extra copies. "World-anchored" sprites move on screen
against the scroll so that they stay put in the ring.

| scene and case | fixed: dirty groups | fixed: BG groups | fixed: rows | fixed: list bytes | ring: dirty groups | ring: BG groups | ring: dup groups | ring: rows | ring: list bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1B04 38 sprites, nothing moves | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 1B04 4 sprites move 1 px, no scroll | 35 | 0 | 32 | 692 | 34 | 0 | 34 | 32 | 744 |
| 1B04 scroll 1 px/frame, screen-anchored sprites | 2096 | 2096 | 359 | 9178 | 366 | 0 | 366 | 450 | 9708 |
| 1B04 scroll 1 px/frame, world-anchored sprites | 2174 | 2096 | 462 | 11157 | 16 | 0 | 26 | 27 | 642 |
| 1B04 scroll 4 px/frame, screen-anchored | 2277 | 2277 | 381 | 9536 | 488 | 42 | 486 | 451 | 10424 |
| 1B04 scroll 1 px/frame down, screen-anchored | 2010 | 2010 | 379 | 9273 | 423 | 17 | 384 | 436 | 9495 |
| 1B04 scroll (2,1), world-anchored | 2243 | 2179 | 465 | 11136 | 140 | 38 | 109 | 30 | 1133 |
| 1D00 scroll 1 px/frame, screen-anchored | 1968 | 1968 | 64 | 3646 | 186 | 0 | 186 | 113 | 3151 |
| 1D00 scroll 1 px/frame, world-anchored | 2018 | 1968 | 113 | 4507 | 22 | 0 | 22 | 0 | 179 |
| 2D00 scroll 3 px/frame, screen-anchored | 3001 | 3001 | 285 | 7292 | 403 | 41 | 402 | 288 | 7555 |

- Scrolling costs the fixed page about 2,000 to 3,000 restored groups a frame
  (the whole background changes); the ring needs none of them for the
  background but the exposed column or row (14 groups per frame on average at
  1 px per frame horizontally, 17 vertically, in theory). Over longer runs (200 to
  700 frames, crossing the ring's rows and its 512 px width in both
  directions) the clean ring was rewritten 13 to 96 groups per frame depending
  on the scroll speed.
- With the background free, the sprites are what is left: screen-anchored
  sprites are all redrawn on every scrolling frame (the ring's list is then as
  long as the fixed page's, 9.7 KB against 9.2 KB for 38 sprites), sprites that
  scroll with the background cost almost nothing (642 bytes for 38 of them).
- Memory at `RH = 448`: 335 KB per page, two pages and the clean ring 224 KB,
  about 1.2 MB with the scratch; at `RH = 256` the duplicate covers most of the
  ring.
- Every run ends with 0 wrong Videl words: the cases above, long runs that
  cross the ring's vertical and horizontal seams in both directions, and starts
  at other places in the ring (`LC_WORLD`). Dropping the duplicate gives about
  3.6 million wrong words, never marking entering groups dirty 370,000, and not
  copying back groups a blit wrote past the ring's end is caught in the one
  scenario that needs it (`LC_SEED=2 LC_SHIFT=0,-63 LC_WORLD=512,443
  build/lc_model hsim ct.sfc F0001B04.BIN 30 38 -1 0 1 0`: sprites on the
  window's top row while it sits at the ring's end).

### Results: per-layer classification

60 frames, means per update, one scene per block, scroll per frame as given
(`LC_LAYER_SX`), no sprites moving. "BG groups produced" is what the exact
background renderer has to deliver (the clean ring's rewritten groups);
"before" treats every layer as standing, as the ring model did until this step.

| scene and scroll per frame | standing set | wrong Videl | dirty groups | BG groups produced | blitter rows | of them overlay | list bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1B04 BG1 2 px, BG2 1 px | automatic: standing BG2 overlay 1 | 0 | 1885 | 11 | 856 | 404 | 20316 |
| 1B04 BG1 2 px, BG2 1 px | all layers standing (before) | 0 | 1360 | 1207 | 451 | 0 | 11418 |
| 1B04 BG1 2 px, BG2 1 px | BG1 standing, BG2 behind it as overlay | 0 | 2281 | 25 | 792 | 341 | 17758 |
| 1B04 BG1 1 px, BG2 fixed | automatic: standing BG2 overlay 1 | 0 | 1507 | 0 | 502 | 456 | 13096 |
| 1B04 BG1 1 px, BG2 fixed | all layers standing (before) | 0 | 1354 | 1217 | 451 | 0 | 11142 |
| 1D00 BG1 fixed, BG2 2 px | automatic: standing BG2 overlay 1 | 0 | 654 | 25 | 274 | 161 | 6997 |
| 1D00 BG1 fixed, BG2 2 px | all layers standing (before) | 0 | 1520 | 1454 | 49 | 0 | 5560 |
| 2D00 BG1 fixed, BG2 and BG3 2 px | automatic: standing BG23 overlay 1 | 0 | 2956 | 21 | 1121 | 878 | 25455 |
| 2D00 BG1 fixed, BG2 and BG3 2 px | all layers standing (before) | 0 | 1451 | 1380 | 83 | 0 | 6088 |
| 2D00 BG1 2 px, BG2 and BG3 fixed | automatic: standing BG23 overlay 1 | 0 | 2407 | 0 | 966 | 707 | 22029 |
| 2D00 BG1 2 px, BG2 and BG3 fixed | all layers standing (before) | 0 | 1870 | 1521 | 286 | 0 | 10972 |

- Every run ends with 0 wrong Videl words, in all 97 modellable snapshots with
  three different scroll speeds on both axes and six moving sprites, and in
  long runs (400 to 600 frames, vertical and horizontal seams, pieces written
  across the ring's end) with parallax.
- Dropping the overlay pieces gives 468,000 wrong words in a parallax scene and
  none in a scene where everything scrolls together, so the check sees the
  overlays and only them.
- The BG renderer's work falls from about 1,200 to 1,500 groups a frame to 0 to
  25. What it costs is overlay rows and cells: 160 to 880 blitter rows a frame
  and up to about 3,000 restored groups, more than the whole-composite
  treatment for the big overlays (BG1 of `F2D00` covers 3,164 of 3,584 cells).
  Where the overlay is sparse (`F1D00`, BG1 352 cells) the restores also drop
  (654 against 1,520 groups). The break-even is a ratio: the overlay path pays
  off when producing one BG group on the DSP costs more than
  (extra rows x one row's cost + extra restored groups x a group copy) / (BG
  groups saved). Two examples with assumed costs of 13 microseconds for a
  blitter row and 4 for copying a group (guesses, not measurements): `F1D00`
  (BG1 fixed) needs 225 more rows but 866 fewer restored groups for 1,429 fewer
  BG groups, so it pays off at any BG group cost; `F2D00` with its large BG1
  fixed needs 1,038 more rows and 1,505 more restored groups for 1,359 fewer BG
  groups, which pays off above about 14 microseconds per BG group. The exact
  renderer's BG work is about 26 microseconds per group in Hatari's DSP timing
  (`dsp-renderer-budget.md`: slivers 54 ms and composition 41 ms for 3,584
  groups), which is not a hardware figure either. The row and copy costs have
  to be measured on a Falcon.

### Results: the source row cache

60 frames, means per update, 64 and 256 KB caches against none (the rows are then
built afresh every update). "New source bytes" is what the DSP sends for rows
not in the cache. A sprite row is keyed by its visible pattern; an overlay
layer's row by the layer's own whole window row, so that it does not change
when what lies under the layer does. "Layer ring estimate" is what the DSP would
send if each overlay layer lived in a world-aligned ring of its own, as the
standing set does (the groups of its window that changed, 16 bytes each;
estimate only, the blits from such a ring are not built).

| scene and case | cache KB | source rows / update | hits | new source bytes / update | layer ring estimate B / update | list bytes / update | peak KB | flushes | wrong |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1B04 4 sprites move 1 px, no scroll | off | - | - | 562 | - | 813 | - | - | 0 |
| 1B04 4 sprites move 1 px, no scroll | 64 | 35 | 97.0% | 17 | - | 813 | 7 | 0 | 0 |
| 1B04 4 sprites move 1 px, no scroll | 256 | 35 | 97.0% | 17 | - | 813 | 7 | 0 | 0 |
| 1B04 scroll 1 px, 38 screen-anchored sprites | off | - | - | 7217 | - | 9913 | - | - | 0 |
| 1B04 scroll 1 px, 38 screen-anchored sprites | 64 | 451 | 99.5% | 39 | - | 9913 | 8 | 0 | 0 |
| 1B04 scroll 1 px, 38 screen-anchored sprites | 256 | 451 | 99.5% | 39 | - | 9913 | 8 | 0 | 0 |
| 1B04 scroll (2,1), 38 world-anchored sprites | off | - | - | 711 | - | 1570 | - | - | 0 |
| 1B04 scroll (2,1), 38 world-anchored sprites | 64 | 44 | 88.0% | 85 | - | 1570 | 11 | 0 | 0 |
| 1B04 scroll (2,1), 38 world-anchored sprites | 256 | 44 | 88.0% | 85 | - | 1570 | 11 | 0 | 0 |
| 1D00 BG1 fixed overlay, BG2 scrolls 2 px | off | - | - | 7440 | - | 6997 | - | - | 0 |
| 1D00 BG1 fixed overlay, BG2 scrolls 2 px | 64 | 241 | 100.0% | 0 | - | 6997 | 29 | 0 | 0 |
| 1D00 BG1 fixed overlay, BG2 scrolls 2 px | 256 | 241 | 100.0% | 0 | - | 6997 | 29 | 0 | 0 |
| 1B04 BG1 overlay scrolls 2 px, BG2 1 px | off | - | - | 46667 | 407 | 20316 | - | - | 0 |
| 1B04 BG1 overlay scrolls 2 px, BG2 1 px | 64 | 662 | 8.3% | 59129 | 407 | 20316 | 59 | 58 | 0 |
| 1B04 BG1 overlay scrolls 2 px, BG2 1 px | 256 | 662 | 53.8% | 54305 | 407 | 20316 | 217 | 14 | 0 |
| 2D00 BG1 fixed, BG2+BG3 scroll 2 px | off | - | - | 53455 | - | 25455 | - | - | 0 |
| 2D00 BG1 fixed, BG2+BG3 scroll 2 px | 64 | 454 | 29.0% | 55805 | - | 25454 | 55 | 58 | 0 |
| 2D00 BG1 fixed, BG2+BG3 scroll 2 px | 256 | 454 | 95.9% | 298 | - | 25454 | 72 | 0 | 0 |
| 2D00 BG1 scrolls 2 px, BG2+BG3 fixed | off | - | - | 45396 | 47 | 22029 | - | - | 0 |
| 2D00 BG1 scrolls 2 px, BG2+BG3 fixed | 64 | 470 | 20.6% | 56614 | 47 | 22030 | 55 | 58 | 0 |
| 2D00 BG1 scrolls 2 px, BG2+BG3 fixed | 256 | 470 | 46.8% | 54643 | 47 | 22030 | 213 | 14 | 0 |
| 1B04 BG1 1 px, BG2 3 px | off | - | - | 51730 | 175 | 21601 | - | - | 0 |
| 1B04 BG1 1 px, BG2 3 px | 64 | 662 | 8.1% | 59413 | 175 | 21602 | 59 | 58 | 0 |
| 1B04 BG1 1 px, BG2 3 px | 256 | 662 | 53.6% | 54565 | 175 | 21602 | 217 | 14 | 0 |
| 1B04 fixed page: 10 of 38 sprites move | off | - | - | 2380 | - | 3113 | - | - | 0 |
| 1B04 fixed page: 10 of 38 sprites move | 64 | 149 | 90.0% | 237 | - | 3113 | 20 | 0 | 0 |
| 1B04 fixed page: 10 of 38 sprites move | 256 | 149 | 90.0% | 237 | - | 3113 | 20 | 0 | 0 |
| 1D00 fixed page: 12 sprites move 2 px | off | - | - | 1546 | - | 2276 | - | - | 0 |
| 1D00 fixed page: 12 sprites move 2 px | 64 | 97 | 100.0% | 0 | - | 2276 | 1 | 0 | 0 |
| 1D00 fixed page: 12 sprites move 2 px | 256 | 97 | 100.0% | 0 | - | 2276 | 1 | 0 | 0 |

- **Sprites are nearly free.** The hit rate is 88 to 100 %, and what the DSP
  sends falls from 560 to 7,200 bytes per update (1,500 to 2,400 on the fixed
  page) to 0 to 237. A cache of 7 to 20 KB holds every sprite row of these
  scenes; 64 KB never flushed. Across the 95 scene changes of the snapshot set
  (new pictures every time) the hit rate is 75 % and the DSP sends 495 bytes per
  update against 1,944, peak 45 KB.
- **A fixed overlay layer is nearly free if its rows stay resident.** Keyed by
  the whole row it hits 100 % in the sparse case (`F1D00`, 0 bytes) and 96 % in
  the dense one (`F2D00`'s BG1: 298 bytes per update). But a dense layer's rows
  are 272 bytes each and take about 72 KB resident; at 64 KB it thrashes (29 %
  hits, 58 flushes, 56 KB sent per update). (Keyed by the visible pattern, as a
  sprite row is, it hit only 29 to 46 % at either size, because which pixels
  show changes as the layers beneath scroll.)
- **A scrolling overlay layer cannot be served this way.** Its rows differ every
  frame (the picture slides under the row's start): 8 to 54 % hits, and the DSP
  would send 54 to 59 KB per update, as much as a whole frame (57 KB), with
  flushes at every size. This is the limit of the overlay scheme as built so
  far. Layer rings (above) are the answer: 47 to 673 bytes per update, see the
  next results.
- The DSP's table for a 64 KB cache is a few hundred entries (a 24-bit hash and
  a 16-bit address each), a few KB of DSP memory, against the 77-page VRAM cache
  it shares memory with.
- Every run ends exact with the cache on: the 95 scene changes, the 97 snapshots
  under parallax, the long runs, and `blitfuzz` with half of its pieces
  repeating an earlier one at another place (hits) and 18 to 295 flushes; a cache
  key that ignores colours is caught (the entry check aborts).

### Results: layer rings

The same scenes (60 frames, means per update) with overlay layers served three
ways: source rows built afresh every update, the row cache (256 KB), and layer
rings with a 64 KB cache for the sprites. "Source row bytes" and "layer ring
bytes" are what the DSP sends for the overlay (and sprite) pictures; the clean
ring's bytes are the standing set's and do not depend on the treatment. The list
is the same in all three: the overlay pieces are the same pieces.

| scene and scroll | overlay treatment | source row bytes | layer ring bytes | clean ring bytes | list bytes | overlay rows | wrong Videl |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1D00 BG1 fixed, BG2 2 px | rows afresh | 7440 | 0 | 400 | 6997 | 161 | 0 |
| 1D00 BG1 fixed, BG2 2 px | row cache 256 KB | 0 | 0 | 400 | 6997 | 161 | 0 |
| 1D00 BG1 fixed, BG2 2 px | layer rings + 64 KB cache | 0 | 0 | 400 | 6997 | 161 | 0 |
| 2D00 BG1 fixed, BG2+BG3 2 px | rows afresh | 53455 | 0 | 336 | 25455 | 878 | 0 |
| 2D00 BG1 fixed, BG2+BG3 2 px | row cache 256 KB | 298 | 0 | 336 | 25454 | 878 | 0 |
| 2D00 BG1 fixed, BG2+BG3 2 px | layer rings + 64 KB cache | 298 | 0 | 336 | 25459 | 878 | 0 |
| 1B04 BG1 2 px, BG2 1 px | rows afresh | 46667 | 0 | 176 | 20316 | 404 | 0 |
| 1B04 BG1 2 px, BG2 1 px | row cache 256 KB | 54305 | 0 | 176 | 20316 | 404 | 0 |
| 1B04 BG1 2 px, BG2 1 px | layer rings + 64 KB cache | 20 | 407 | 176 | 20316 | 404 | 0 |
| 1B04 BG1 1 px, BG2 3 px | rows afresh | 51730 | 0 | 640 | 21601 | 452 | 0 |
| 1B04 BG1 1 px, BG2 3 px | row cache 256 KB | 54565 | 0 | 640 | 21602 | 452 | 0 |
| 1B04 BG1 1 px, BG2 3 px | layer rings + 64 KB cache | 39 | 175 | 640 | 21602 | 452 | 0 |
| 2D00 BG1 2 px, BG2+BG3 fixed | rows afresh | 45396 | 0 | 0 | 22029 | 707 | 0 |
| 2D00 BG1 2 px, BG2+BG3 fixed | row cache 256 KB | 54643 | 0 | 0 | 22030 | 707 | 0 |
| 2D00 BG1 2 px, BG2+BG3 fixed | layer rings + 64 KB cache | 0 | 47 | 0 | 22030 | 707 | 0 |
| 2D00 BG1 3 px down, BG2+BG3 2 px right | rows afresh | 52786 | 0 | 336 | 24922 | 854 | 0 |
| 2D00 BG1 3 px down, BG2+BG3 2 px right | row cache 256 KB | 497 | 0 | 336 | 24922 | 854 | 0 |
| 2D00 BG1 3 px down, BG2+BG3 2 px right | layer rings + 64 KB cache | 298 | 673 | 336 | 24925 | 854 | 0 |

- **Layer rings are exact**, on the first run and everywhere after: 0 wrong
  Videl words in these scenes, in all 97 snapshots under parallax with six
  moving sprites, in 400 to 600-frame runs with both rings crossing their seams
  (the layers' world origin moved near the ring's end with `LC_LWORLD`, the
  standing set's with `LC_WORLD`) combined with the sprite cache.
- **What the DSP sends for overlays and sprites** falls from 45 to 54 KB per
  update (rows afresh or cached, scrolling layer) to 50 to 700 bytes: 47 to 673
  bytes of layer ring groups plus 0 to 300 of sprite rows. A fixed layer costs
  nothing once it is in its ring.
- **The row cache still does well for a layer that scrolls vertically** (`F2D00`
  BG1, 3 px down: 497 bytes per update, against 52 KB afresh), because the same
  rows show up again at other places. It fails for horizontal scrolling.
- **The list was what the DSP sends most**: 20 bytes per blitter row, about
  1,100 to 1,400 rows a frame with a large overlay (22 to 32 KB per update),
  against 0.2 to 1.3 KB for every picture the DSP sends. The rows are many because a dense
  overlay is cut into runs wherever its mask has a hole (an inner group has to
  be full). The packed records (below) cut it to a third.
- **Mutations are caught**: skew off by one (176,000 wrong words), FXSR dropped
  (262,000), the source ring's duplicate not written (caught at the 20 of 64
  positions of the layer ring where a run crosses the ring's end; the real code
  is exact at all 64).
- **Memory**: a layer ring is 229 KB (the same as the clean ring), so two pages,
  the clean ring, up to four layer rings and a 64 KB cache come to about 1.9 MB at
  `RH = 448`, a good part of a 4 MB Falcon; a smaller `RH` or fewer simultaneous
  overlay layers reduces it, and only layers that are overlays use a ring.

### Results: the list, packed

200 frames, means per update, overlay layers from their rings and a 64 KB
cache for the sprites. "List before" is the 20-byte record per run (and 4 bytes a
restore or duplicate run); "packed" is the records above and 3 bytes per restore
run.

| scene and scroll per frame | rows | list before (20 B per row) | packed | of it entry rows | ring rows | restore runs | sprite programs: calls + new | wrong |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1D00 BG1 fixed, BG2 2 px | 274 | 6975 | 2458 | 1185 | 712 | 561 | 76 | 0 |
| 2D00 BG1 fixed, BG2+BG3 2 px | 1236 | 28538 | 7811 | 2057 | 4316 | 1437 | 1098 | 0 |
| 1B04 BG1 2 px, BG2 1 px | 867 | 19515 | 7697 | 5044 | 1833 | 820 | 643 | 0 |
| 1B04 BG1 1 px, BG2 3 px | 837 | 19320 | 7604 | 4905 | 1726 | 973 | 652 | 0 |
| 2D00 BG1 2 px, BG2+BG3 fixed | 593 | 14115 | 3839 | 1140 | 1849 | 849 | 90 | 0 |

- **The list is 2.5 to 3.7 times smaller**: 2.5 to 7.8 KB a frame against 7 to
  28.5 KB. The dense overlay of `F2D00` (1,236 blitter rows a frame) went from
  28.5 KB to 7.8 KB, its ring rows taking 4.3 KB of that.
- **Where the rest is**: ring rows (the overlay layers) 0.7 to 4.3 KB, entry
  rows (the sprites) 1.1 to 5 KB, restore runs 0.6 to 1.4 KB. With the
  continuation header a ring row costs about 5 bytes, nearly all of it runs and
  masks; the sprite rows cost about 10 each, 7 of it header.
- **Sprite programs** (estimate, bytes of record): after 200 frames 50 to 95 %
  of the sprite instances find their program, and what the sprites cost falls
  from 1.1 to 5 KB to 0.08 to 1.1 KB a frame (calls plus the programs
  still new). The scenes where the sprites animate or overlap, so that rows are
  drawn clipped to the dirty groups, find fewer. Moving sprites need a program per
  shift (16 pixel positions); the cost is in memory and in the first frames.
- Every run ends exact with the records checked; two deliberately wrong
  encoders (the last mask never sent, a range one pixel short) abort at once.

### Results: sprite instance packets

200 frames, means per update, overlay layers from their rings, a 256 KB cache for
the source rows. "Sprite bytes" is the list for the sprite rows; "calls" is the
share of instances that found their program; the defines are per update; "store
held" is what the program store holds at the end.

| scene and case | sprite rows | sprite bytes | calls of a kept program | kept defines / run once per update | store held | packed list | wrong |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1B04 38 sprites, scroll 1 px (screen-anchored) | per-row packets | 4905 | - | - | - | 5251 | 0 |
| 1B04 38 sprites, scroll 1 px (screen-anchored) | instances, store 64 KB | 4218 | 0% | 46.6 / 0.0 | 33 KB | 4564 | 0 |
| 1B04 38 sprites, scroll 1 px (screen-anchored) | instances, store 256 KB | 674 | 90% | 4.8 / 0.0 | 84 KB | 1021 | 0 |
| 1B04 38 sprites, scroll 3 px | per-row packets | 4897 | - | - | - | 5415 | 0 |
| 1B04 38 sprites, scroll 3 px | instances, store 64 KB | 4207 | 0% | 45.8 / 0.0 | 30 KB | 4725 | 0 |
| 1B04 38 sprites, scroll 3 px | instances, store 256 KB | 752 | 87% | 5.9 / 0.0 | 100 KB | 1270 | 0 |
| 1B04 4 sprites move 1 px, no scroll | per-row packets | 218 | - | - | - | 245 | 0 |
| 1B04 4 sprites move 1 px, no scroll | instances, store 64 KB | 56 | 78% | 0.8 / 0.0 | 10 KB | 83 | 0 |
| 1B04 4 sprites move 1 px, no scroll | instances, store 256 KB | 56 | 78% | 0.8 / 0.0 | 10 KB | 83 | 0 |
| 1B04 10 sprites move (1,1) | per-row packets | 809 | - | - | - | 878 | 0 |
| 1B04 10 sprites move (1,1) | instances, store 64 KB | 442 | 42% | 5.6 / 0.0 | 32 KB | 511 | 0 |
| 1B04 10 sprites move (1,1) | instances, store 256 KB | 371 | 51% | 4.7 / 0.0 | 66 KB | 440 | 0 |
| 1D00 12 sprites move, scroll 1 px | per-row packets | 795 | - | - | - | 1074 | 0 |
| 1D00 12 sprites move, scroll 1 px | instances, store 64 KB | 84 | 92% | 0.5 / 0.0 | 11 KB | 363 | 0 |
| 1D00 12 sprites move, scroll 1 px | instances, store 256 KB | 84 | 92% | 0.5 / 0.0 | 11 KB | 363 | 0 |
| 2D00 scroll 3 px | per-row packets | 2768 | - | - | - | 3458 | 0 |
| 2D00 scroll 3 px | instances, store 64 KB | 1689 | 29% | 19.9 / 0.0 | 9 KB | 2379 | 0 |
| 2D00 scroll 3 px | instances, store 256 KB | 1358 | 44% | 15.5 / 0.0 | 45 KB | 2048 | 0 |
| 2D00 12 sprites move (1,1), BG1 fixed overlay | per-row packets | 1875 | - | - | - | 7616 | 0 |
| 2D00 12 sprites move (1,1), BG1 fixed overlay | instances, store 64 KB | 1181 | 30% | 15.7 / 0.0 | 15 KB | 6922 | 0 |
| 2D00 12 sprites move (1,1), BG1 fixed overlay | instances, store 256 KB | 1043 | 40% | 13.4 / 0.0 | 182 KB | 6784 | 0 |

- **The sprite rows cost up to 7 times less**: 4,905 bytes a frame for 38 scrolling
  sprites become 674 (calls 253, new programs 414), 795 become 84 for `F1D00`'s 12,
  218 become 56. The whole packed list of the 38-sprite scene falls from 5.3 KB to
  1.0 KB.
- **The store has to hold the working set.** 38 sprites at 16 shifts need
  84 to 100 KB of programs; a 64 KB store is flushed every 12 frames, before a
  shift cycle of 16 frames has come round, and finds nothing (0 % calls, only the
  2 bytes a row saved by the smaller header). 256 KB finds 87 to 90 %.
- **Scenes with sprites that overlap, animate or are clipped find fewer**
  (30 to 50 %, `F2D00` and diagonal movers): the visible masks then differ, and a
  program is its masks. Those cost about half of what per-row packets did, at a
  store of 45 to 182 KB held. A smarter store (evicting the least recently used
  instead of everything) and keeping the unclipped image's program with the
  occlusion as a mask layered on top are the ways to do better.
- Every run ends exact with the packets checked, including the 97 snapshots under
  parallax, runs whose cache and store flush every few frames (16 KB cache, 24 KB
  store, 38 moving sprites), and ones that cross both rings' seams; a wrong G0 in a
  call aborts at once. The estimate made before (programs as a set of record
  hashes) said 1.1 to 5 KB down to 0.08 to 1.1 KB; the packets agree.

### Results: store policy, whole instances, occlusion

**Store policy** (200 to 400 frames; the first case is a constant 1 px scroll of 38
sprites, so every sprite steps through its 16 shifts in order, working set 84 KB;
the second lets the 38 sprites move as well, which is irregular). Share of the
instances that found their program, and the bytes the sprites cost per update:

| case | store | flush all when full | slots, least recently used | slots, random victim | slots, never evict |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1B04 38 sprites, scroll 1 px (a cycle of 16 shifts) | 64 KB | 0 % / 4328 B | 0 % / 4422 B | 6 % / 4169 B | 29 % / 3099 B |
| 1B04 38 sprites, scroll 1 px (a cycle of 16 shifts) | 128 KB | 92 % / 585 B | 0 % / 4421 B | 38 % / 2771 B | 58 % / 1901 B |
| 1B04 38 sprites, scroll 1 px (a cycle of 16 shifts) | 192 KB | 94 % / 516 B | 59 % / 2122 B | 79 % / 1032 B | 84 % / 926 B |
| 1B04 38 sprites, scroll 1 px (a cycle of 16 shifts) | 256 KB | 94 % / 516 B | 94 % / 522 B | 93 % / 559 B | 90 % / 676 B |
| 1B04 38 sprites moving (1,1), scroll 1 px | 64 KB | 48 % / 781 B | 18 % / 1223 B | 28 % / 1041 B | 34 % / 941 B |
| 1B04 38 sprites moving (1,1), scroll 1 px | 128 KB | 64 % / 564 B | 61 % / 586 B | 49 % / 733 B | 54 % / 696 B |
| 1B04 38 sprites moving (1,1), scroll 1 px | 192 KB | 67 % / 514 B | 65 % / 543 B | 58 % / 612 B | 59 % / 648 B |
| 1B04 38 sprites moving (1,1), scroll 1 px | 256 KB | 69 % / 487 B | 67 % / 525 B | 62 % / 572 B | 66 % / 524 B |

- **The policy matters less than whether the working set fits.** A store that is
  just emptied when full is all or nothing (0 % at 64 KB, 92 % at 128 KB, for
  the cycle), the slotted stores lose a third of their room to the size classes
  (128 KB of slots does not hold what 84 KB of programs need).
- **LRU is the worst policy on a cycle**: it evicts each program just before the
  cycle comes round to it, so it finds nothing until the whole set fits (0 % at
  128 KB, 59 % at 192 KB, 94 % at 256 KB). Random replacement and never evicting
  degrade gracefully (random 38 % and never evicting 58 % at 128 KB). On the
  irregular movement LRU does as well as any (61 % at 128 KB).
- Scrolling and walking cycles are what a game mostly has, so a slotted store wants
  random replacement or no eviction and 192 to 256 KB for 38 sprites at 16
  shifts each, or a store that is emptied when
  full at 128 KB if the set is known to fit. LRU is not the choice.

**Whole instances** (`LC_INSTANCE=2`, against `1`): the share of calls rises a
little (50.7 to 59.3 % with ten moving sprites, 40.5 to 44.8 % with twelve, no
change or 0.6 points elsewhere) for slightly more blitter rows (76 to 89, 1,209 to
1,239). Clipping is not what costs the programs.

**Occlusion is**, and it cannot be got out of the programs cheaply. Per instance
drawn, whether the sprite's own opaque pixels are all shown (the silhouette):

| scene | instances nothing occludes | found | occluded or clipped | found |
| --- | ---: | ---: | ---: | ---: |
| `F2D00`, scroll 3 px | 11.2 | 81 % | 16.7 | 29 % |
| `F2D00`, 12 sprites moving, BG1 fixed | 7.6 | 81 % | 14.6 | 26 % |
| `F1B04`, 38 sprites moving, scroll 1 px | 18.0 | 76 % | 12.9 | 58 % |
| `F1B04`, 10 sprites moving | 5.3 | 75 % | 4.0 | 39 % |

The occluded instances are more than half of all in the overlapping scenes and
find a quarter of their programs. The remedy one would build is a program for the
silhouette (the unoccluded rows) with the rows that differ sent explicitly. I
measured what that could save at best, with every silhouette program found: the
occluded instances would cost 797 bytes a frame instead of 1,001 (`F2D00`), 728
instead of 939, and in the other two scenes more, 253 instead of 228 and 737
instead of 449. Whole sprites are occluded in most of their rows, so the
explicit rows cost about what the rows did. Not built; the lever is at most 20 %
and sometimes negative.

### Results: cost-weighted choice of the standing set

The default rule picks the group of layers that leaves the fewest opaque BG pixels to
the overlays. `LC_CHOOSER=cost|exact` weighs costs instead: per candidate (each
group of layers with one scroll as the standing set with the others as overlays, or
"all": every layer stands, the DSP produces the cells that changed) the estimate is
`(g + 16h) * produced groups + c * copied groups + b * blitter rows + (b + c) * sprite
rows`, with g = one BG group from the exact renderer, b = one blitter row, c = one group
copied, h = one byte sent. **The unit costs are assumptions, not measurements**: g = 26 µs
is `dsp-renderer-budget.md`'s Hatari DSP timing, b = 13 and c = 4 are guesses, h = 0.25
is a 16-cycle host-port read at 16 MHz. `LC_COST=g,b,c,h` changes them and the model
prints the resulting cost per update. A switch rewrites the clean ring, so the new
choice must pay back the window's groups within `LC_HORIZON` frames (default 32).

- `cost` (1) takes cells and blitter runs from each layer's own picture.
- `exact` (2) takes them from the pieces the candidate would really make (F against the
  standing background, sprites left out; for "all", the cells that changed against the
  last frame moved by the anchor's scroll). It renders once per candidate, so it is the
  ceiling for a chooser, not something the DSP would do per frame.

Candidates are every set of layers that share one scroll (standing, the others overlays)
and "all standing" anchored to each layer. **The chooser commits to one and only
switches when the smoothed saving of another pays the rewrite of the clean ring**: an
exponential average of (committed cost - candidate cost) over the frames, times
`LC_HORIZON`, against the window's groups from the DSP. Idle frames (no layer scrolled,
the BG picture unchanged) say nothing and never switch. A set whose layers have different scrolls on a frame cannot stand as it is; it stays a
candidate at the cost of "all standing", so that a set which only works on the frames where
the layers move together shows its losses too (without that, a real window flipped between
"all three layers" and "BG2 alone" every few dozen frames). Without the commitment the
chooser was fooled by real frames: see "Real consecutive frames" below. (A first version
compared single frames and scored better on the synthetic scenes below, worst regret 14 %,
mean 1.9 %, but flipped on real frames exactly as the rule does.)

Hindsight comparison: 150 frames from one snapshot, scenes with synthetic scroll, each
policy against every forced standing set, regret = model cost over the best policy's
(`build/host/chooser_eval3.md`; scene and size of the sets are small, so this is a check
of the method and not a survey). Six unit-cost settings, 36 cases, every run exact
(0 wrong Videl words):

| policy | worst regret | cases above 5 % | mean |
| --- | ---: | ---: | ---: |
| opaque-pixel rule (default) | 50 % | 16 of 36 | 12.8 % |
| `cost` (each layer's own picture) | 65 % | 19 of 36 | 15.8 % |
| `exact` (pieces, committed) | 15 % | 6 of 36 | 2.0 % |

- **The pixel count misleads in both directions.** In the 2D00 scene with BG1 fixed and BG2/3
  scrolling, the rule keeps BG2/3 standing because BG1 is bigger, but BG1 as standing set
  costs 24 to 32 % less than the rule's choice: a dense fixed layer's pixels mostly equal what
  the layers behind show, so its overlay pieces are few. A layer's own picture (`cost`)
  cannot see that, only the comparison with the standing background can.
- **`exact` is within 9 % in 35 of 36 cases**; its misses are the 1B04 scene at g = 52 (15 %,
  a commitment made before the saving showed) and small losses where "all standing" wins at
  g = 5. An earlier version of the estimate counted the blitter runs of the whole mask where
  F differs, and was 4 to 5 times short for a dense overlay (211 runs against the model's 875
  for BG1, one 51 % case): pieces are cut by owner, the first overlay layer whose own picture
  is the pixel, so each layer's mask is ragged where a layer above shows through. The estimate
  now counts runs per owner and is within about 20 % of the model's overlay rows (756 against
  752, 879 against 1,063, 1,367 against 1,594 for three standing sets of the 2D00 scene; the
  rest is the ring seams and dirty-group cuts).
- **Switching is the expensive part**: a switch is 3,600 groups from the DSP, about as
  much as 5 to 50 frames of the steady-state difference between two sets.

### Real consecutive frames

157 consecutive frames of the attract run (game frames 6,904 to 7,060, a scene with two
scrolling layers and about 37 sprites), from the runtime built with
`SMKX_SNAP_EVERY=1` and the new `SMKX_SNAP_FROM=6900` (writes snapshots only from that frame
on: a window, not 2 GB). `lc_model hreal rom.sfc SNAP.BIN...` runs the ring model on them
as they are; nothing synthetic. The snapshots are in `build/ctxwin/` (untracked), taken in
Hatari v2.6.1-devel (Sep 9 2026, compiled 17:04:10), F030Arcade's patched build,
`third_party/hatari/build-ucrt64/src/hatari.exe`, Falcon, TOS 4.04, `--dsp emu`,
`--cpu-exact`, `--fast-forward`. Emulator frame numbers, not a Falcon's.

- **Exact**: 0 wrong pixels and 0 wrong planar bytes with the abstract and planar models (`seq`),
  0 wrong Videl words in the ring model with the row cache and instance packets on.
- **Many frames change nothing**: 56 of 157 have no dirty group at all (the game steps its
  picture on the others), so "idle" is common and a choice made on one says nothing.
- **Standing set.** The scroll of the two layers is sometimes equal and sometimes not, frame to
  frame. The rule follows it and flips between "BG2 standing, BG1 overlay" and "both
  standing", each flip a clean-ring rewrite of about 1,300 groups: 29,196 µs per update at the
  assumed unit costs, where a fixed "BG2 standing" costs 9,707 (BG1 standing 11,186, all
  standing 13,375). The committed chooser costs 10,199 (`cost` and `exact` agree), 5 % above
  the best fixed set, and 9,944 at g = 13 against 9,667. This is the one scene, and the
  fixed best set is found in hindsight.
- **Packed list**: 1,171 bytes per update with instance packets (6,724 unpacked), of which
  sprites 85 B calls + 317 B defines, 79.9 % of 17.8 instances per update found their
  program (128 KB store, 50 KB held, no flush); the source row cache hits 98.4 %; overlay
  rings need 182 B per update. The DSP would send about 1.2 KB a moving frame.
- Model cost with everything on: 9,820 µs per update at the assumed unit costs, against
  16.7 ms of a 60 Hz frame. Not a measurement of the Falcon.

### More real windows

`scripts/capture_window.sh NAME FROM TO` makes a window (reassemble, relink, run Hatari, stop
its own Hatari) into `build/ctxw_NAME`. **Frame numbers are not game states**: a build that
snapshots every frame lags, so the scenes of the 64-frame series in `build/ctx` sit about 700
frames earlier than the same scenes in a run that does not snapshot before its window (the
25-sprite three-layer scene is near frame 11,520 in the series and near 12,200 without
snapshots). Probe runs of a couple of snapshots at several frames found the scenes first.
Three more windows, all exact (0 wrong Videl words; `seq` 0 wrong pixels and planar bytes),
model cost at the assumed unit costs with the committed `exact` chooser, instance packets
(128 KB store) and a 64 KB row cache:

| window (game frames) | scene | idle frames | rule | `cost` | `exact` | best fixed set | list per update (unpacked) | instance calls |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 6,904 to 7,060 | two layers scrolling, 37 sprites | 56 of 157 | 29,196 | 10,199 | 10,199 | 9,707 | 1,171 B (6,724) | 80 % |
| 12,150 to 12,300 | three layers, 25 sprites, nearly still | 125 of 150 | 137 | 137 | 137 | (all equal) | 36 B (160) | 61 % |
| 12,750 to 12,900 | three layers scrolling, 12 sprites | 41 of 150 | 31,173 | 9,923 | 9,982 | 9,138 | 1,018 B (4,600) | 84 % |
| 10,300 to 10,450 | 55 frames with windows, then one layer and 13 sprites | 32 of 79 modelled | 4,102 | 4,102 | 4,102 | 4,102 (all) | 562 B (3,810) | 84 % |

- **The committed chooser is within 5 to 10 % of the best fixed set in both scrolling windows,
  the rule is 3 times worse** (it flips the standing set as the layers' scrolls agree or not).
- **Idle frames are the norm** in the still scene (125 of 150) and common in the others.
- **Frames the model does not cover** are real: in the last window 55 of 134 frames use
  windows and go to the exact renderer (the whole set: 102 of 199 snapshots of the series are
  skipped, 42 of them forced blank). What the model's rows say holds for the frames it covers.
- Sprite programs are found in 61 to 84 % of calls; the small numbers are scenes with
  nearly nothing to draw.

### The outdoor scene

The forest path with the party walking and scrolling (scrolling, about
a dozen sprites), the scene of the attract run that is closest to play. Window 10,360 to
10,820 (433 snapshots in `build/ctxw_outdoor`; it follows 55 frames of a window-wipe
transition that the model does not cover, see above). All 433 frames are modelled, all
exact: 0 wrong pixels and planar bytes (`seq`), 0 wrong Videl words (`hreal`), with the
ring, row cache and instance packets on.

- **One group of layers, nothing to choose.** BG1 and BG2 scroll together the whole window
  (standing set `S=12`; BG3 is the overlay layer and needs no painting), so every policy, the forced
  sets aside, gives the same cost (3,263 µs per update at the default unit costs, 2,600 to
  4,985 over the six settings); BG1 alone standing costs three times that.
- **Half of the frames are idle** (217 of 433: the game changes its picture on about
  half of the frames); a frame that moves costs about twice the mean, near 6 ms at the assumed unit costs
  (mean 2,984 µs with instance packets and the exact chooser).
- **The hardware does the scroll**: the DSP produces 17 exposed background groups a frame
  (8 groups a frame written to the clean ring on average), where the page model without
  hardware scroll (`seq`) has to produce 1,660 a frame. At the assumed 26 µs per group that
  is 0.4 ms against 43 ms, which is why the exact renderer cannot do this scene at frame
  rate; a figure on the Falcon needs the measured cost of a group.
- **The work that is left is sprites and their cleanup**: 133 blitter rows and 171 groups
  restored a frame, 11.7 sprite instances of which 82.5 % are calls of a kept program (58 B
  of calls and 203 B of defines per update); 8.2 instances a frame are not occluded (86 %
  found), 3.5 are occluded or clipped (74 % found), better than the attract scenes. Packed
  list 513 B per update (3,089 B unpacked), the source row cache hits 98 %, program store
  83 KB of 128 KB held, no flush.

### The outdoor scene played in Hatari (`falcon/lcplay.s`)

The model's output for the outdoor window, executed on the 68030 and the blitter in Hatari.
`LC_REC=file.lcp lc_model hreal ...` records what the update did to the model's memory as a
stream: DATA (bytes the DSP would send: source rows, clean and layer ring groups), COPY (the
030's restores and ring-seam copies), BLIT (one blitter run's registers), PAL, and an END per
frame (page, Videl start address, HSCROLL, groups fetched, three checksums). `lc_model play`
executes a stream on a fresh memory and checks each frame against the page window's
checksum, the displayed pixels of the exact renderer (HSCROLL applied) and the whole of
memory: the outdoor stream, 3.1 MB, 433 frames, 1,834 DATA, 91,347 COPY and 57,645 BLIT ops
(133 blits and 211 copy runs a frame), plays with 0 frames different.

`falcon/lcplay.s` (vasm and vlink, no C) is the player: it lays out the model's memory
(2.9 MB, two 344 KB pages with their ring duplicate, the clean ring, the layer rings, the
row cache), executes the ops (the blitter with HOP 2, OP 3, three endmasks, skew, FXSR and
NFSR, hog mode, waiting for it), and ends each frame by writing the Videl's start address
(the page window's first group), HSCROLL (`$ffff8265`), the line offset that keeps the
ring's 512-byte row stride (128 words, 120 with HSCROLL because one more group is
fetched), then waiting for the vertical blank that latches them. The next frame is drawn
to the other page. `scripts/run_lcplay.sh check` sums each page window in the player
(rotate and add, as the recorder does) and compares: **433 frames, 0 mismatches** in Hatari
(video on, real time, and video off, fast forward). A stream with one blit's skew flipped
in frame 100 gives 3 bad frames, first bad 100, so the check bites. `record` runs one pass
with Hatari's AVI recording (`--avirecord true --avi-vcodec png`): the scene scrolls and
the sprites are drawn, at one game frame per vertical blank (460 frames, 9.2 s at 50 Hz;
`build/lcplay/run_record/outdoor_hatari.mp4`). `view` opens a window and loops until a key.

- **What this shows**: the list, the row cache's entries, the restores, the ring with its
  duplicate, the skew/mask arithmetic and the page flip are executed by the 030 and Hatari's
  blitter and give the exact frames. **It says nothing about time**: Hatari's blitter and
  030 caches are not a Falcon's, and the recorded stream is what the DSP would send, not
  something a DSP produced.
- **HSCROLL**: Hatari reads the eight-plane window with HSCROLL as STE does (one more group
  per line, the line stride growing by the plane count). The player relies on that; the real
  Falcon's behaviour in eight planes is not verified.
- **Data cache**: the player turns the 030's data cache off (the blitter writes behind it).
  A port with the cache on needs flushes.
- Two traps that cost a run each: `clr.l $ffff8264.w` also clears `$ffff8266`, the plane
  count (the picture then shows as four planes, in bands), and `--avirecord` takes a
  boolean (`--avirecord true`; otherwise Hatari opens a usage window and blocks).
- Not streamed or played: instance packets (the model records the blits they expand to,
  as all blits), colour-math or window frames, the exact renderer's fallback for them.

### How the real frames were made on this machine

`scripts/capture_window.sh` does the following. No cross-GCC here, so the C objects of the existing `build/ctx` are relinked and only the
runtime is reassembled (`falcon/smkx_rt.s` with `SMKX_SNAP_EVERY=1 SMKX_SNAP_FROM=6900
SMKX_FRAMES=7060`, the other switches as in `build/ctx/flags.stamp`, vasm and vlink from
`../../../F030Arcade/third_party`), into `build/ctxwin/CTXWIN.TOS`. Hatari needs the program
staged as `AUTO\GAME.PRG` with the directory as the trailing argument (the program-file
argument fails in `GetTempFileName` in this sandbox), `--debug-except none`,
`C:\msys64\ucrt64\bin` on PATH; it does not exit by itself after the last frame and
has to be stopped by its own PID. The snapshots land in the directory.

## Open questions and next steps

1. One window of consecutive real frames is in (above). More are needed (walking in a
   field, battle, menu): `SMKX_SNAP_FROM` picks the window, `hreal` runs it.
2. Done in the model: the blitter rows (planar), the virtual screen with
   hardware scroll, per-layer classification with overlay layers as pieces.
   The source row cache, the layer rings, the packed list, the sprite instance
   packets and their store policies are in (above); the occlusion lever was
   measured and is small. The standing-set choice weighs costs now (above, with
   assumed unit costs; `exact` needs a cheap proxy for the DSP). Next: measured costs
   (DSP group, blitter row, group copy) for those units; more real windows, which
   decide how cyclic and how occluded sprites really are. Compare `blit_run()` with Hatari's own
   blitter on the fastbobs code, then with a Falcon.
3. DSP side: visibility and BG-priority occlusion for pieces, the per-page
   state (memory next to the 77-page VRAM cache), list format, host-port
   transfer of the list.
4. Falcon hardware, not Hatari (it models neither the blitter nor the 030
   instruction cache well, see `../../../F030Arcade/docs/falcon-timing-probe.md`):
   cost of one blitter register write from the 030; cost of fetching a word of
   generated code from ST-RAM with the cache enabled and frozen; sprite time of
   the cache-resident record loop against straight-line generated code; a hot
   loop's time before and after the generated code ran; eight-plane
   hardware-scroll behaviour.
5. Colour math (20 of 122 mode-1 snapshots) and windows: the compositor path
   falls back to the exact renderer for frames that use them, until the
   reference supports them (`ROADMAP.md` item 4).
6. The plane partition and plane sharing need the per-layer colour census.

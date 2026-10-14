# Four-plane DSP renderer: a path towards 40 ms

Update 2026-09-26: [Mode 7 audit and current 25 fps budget](mode7-25fps.md)
has fresh frozen-source measurements. The per-row sample payload and some
CPU/scheduling recommendations below have since been superseded.

2026-09-25. Advice based on the current source and an isolated PPUTEST
snapshot benchmark. No renderer or runtime implementation was changed.

Keep the 256x224, four-plane output. It halves SSI/DMA pixel traffic
relative to eight planes and reduces c2p work. This makes a sub-40 ms
transport possible on the stock Falcon. SMK already gets that benefit;
getting the complete renderer below 40 ms still requires a substantial
rewrite of its repeated work. The measurements do not yet establish that
full-resolution, reference-equivalent rendering will meet that target.

## The output budget

Atari's hardware reference gives a master-clock / 4 maximum serial bit
rate and a 1 MB/s maximum for SSI and each sound DMA channel. These are
hardware-derived ideal bounds, not physical Falcon measurements.
Source: [Atari Falcon030 Developer Support Package, Hardware p. 21,
PDF page 49](https://www.atarimania.com/documents/Atari-Falcon030-Developer-Support-Package.pdf#page=49).

| Payload for 224 rows | Bytes | Ideal at current 25.175 MHz | Ideal at 32 MHz |
| --- | ---: | ---: | ---: |
| Four planes, pixels only | 28,672 | 36.45 ms | 28.67 ms |
| Current SMK: four planes plus samples | 32,256 | 41.00 ms | 32.26 ms |
| Eight planes, pixels only | 57,344 | 72.89 ms | 57.34 ms |

Calculation: bytes / (master clock / 4 / 8). Real output also has startup,
handshake and producer gaps. Four planes need 716,800 useful bytes/s just
to finish in 40 ms. At the current clock, pixels alone leave only 3.55 ms
for gaps; at 32 MHz they leave 11.33 ms.

The extra SMK traffic comes from `cp_smp` in `falcon/dsp/ppu.asm`: every
row sends 64 plane words followed by eight words containing 16 CGRAM
index samples. `src/smkx_m7.c` records all 72 words; Videl's line offset
hides the samples but does not avoid transmitting them. Thus **the current
payload and clock cannot deliver a complete frame in under 40 ms**, even
with instantaneous rendering.

Four planes reduce output and conversion work, but do not halve the
number of Mode 7 samples, sprites, priority decisions or HDMA writes.
The present c2p is already four-plane code (`cp_loop`, two plane pairs
in A and B). There is no additional automatic 8-to-4-plane saving to take.

## Fresh baseline and its limits

All measured numbers in this section are emulated: patched Hatari
`v2.6.1-497-g639fadf6`, build banner `2026-09-22 21:37:14`, exact binary
`../../../F030Arcade/third_party/hatari/build/src/hatari`. Binary SHA-256:
`b3279c6de6893a666e8e3c87b65eb4cf69f10f296a098b39272f7757731f1f7d`.

Freshly assembled kernel and freshly compiled PPUTEST are under
`build/dsp-advice-20260925/`; log:
`build/smkx/dsp-advice-20260925-stderr.log`.

| Snapshot test | Time |
| --- | ---: |
| Full frame, including SSI record | 212.76 ms |
| Full frame, output disabled | 187.50 ms |
| Output disabled, sprites removed | 145.16 ms |
| Output disabled, BG1 removed | 118.13 ms |
| Output disabled, both removed | 79.45 ms |
| CPU builds the frame stream, separately | 95.10 ms |
| CPU builds it with HDMA disabled | 24.24 ms |

The stream has 1,459 words and three OAM lists of 17/19/17 sprites.
The no-output timing includes host command feeding and the final PING;
it is not an isolated DSP cycle count. Upload and CPU stream construction
are outside those render timers. Feature-removal differences are useful
priorities, not independent additive stage costs: removing BG1 affects
both tile and Mode 7 rows, and composition changes too.

The input is a frozen copy of the current `build/smkx/F0001194.BIN`, with
SHA-256 `03f02f6e019cd17cc15b37cd570ddf93362e3d160b26bbd8b69f9a0580fc7d0f`.
Earlier notes benchmark a different capture under the same filename,
with more sprites and 238 ms including SSI. This is not evidence of a
new renderer speed-up. Hash snapshots when comparing changes.

This run passed the 32,768-word VRAM round-trip check. PT_SNAP performs
timings, not a pixel comparison; random-frame equality tests were not
rerun for this advice. Historical correctness results remain in
`analysis/translator.md`.

## Recommended implementation order

1. **Prove the transport first.** Add a resident, deterministic output
   benchmark that sends exactly one frame of known words into sound DMA.
   Measure both clock routes and compare every captured word. Test the
   32 MHz DSP-to-record route in `falcon/smkx_m7dsp.s` and the PPUTEST
   glue; this changes the crossbar clock, not the DSP's compute clock.
   Retain a fallback until it is checked on a physical Falcon. The
   converter's audio clock restrictions matter if audio later shares the
   route. Four play/record tracks are already configured here.

   Accumulate the same 16 samples per row into 256 DSP histogram bins
   and return the bins once per frame over the host port. This preserves
   the histogram and palette policy while removing 3,584 SSI bytes and
   the CPU's sample-gather loop. Reserve memory for the bins explicitly.
   A host-port return has CPU read cost; include it in the measurement.
   Change the record length, screen pitch, snapshot handling and format
   tests together. Start with 256 counters; compression is secondary.

2. **Overlap output with drawing across rows.** SMK's `ssi_send` polls
   TDE and writes each word directly; while it builds the next row, the
   transmitter has little queued work. Its asynchronous 68030 host-feed
   interrupt is a separate mechanism. Introduce a bounded SSI output
   ring, with a producer/consumer protocol that cannot transmit unwritten
   data. CT's marked-word ring and initial polled word are useful examples.

   Budget the interrupt's execution and register saves as DSP work.
   This is not a free second processor. Count ring-empty stops and
   producer waits; a larger ring only hides bursts. The approximate
   overlapped cost is startup + max(compute including output service,
   wire time) + drain. Do not add full compute and transfer times, and do
   not expect removing the observed 25.26 ms SSI increment alone to
   turn this 212.76 ms frame into a 40 ms one.

   There is a specific memory obstacle: the fresh kernel uses 508 of 512
   internal P words and places code in otherwise unused interrupt vectors.
   Reserve the SSI vectors and relocate that code before enabling its
   interrupts. The Mode 7 loop and c2p are already internal; merely
   suggesting they be moved there again yields no gain.

3. **Fuse the Mode 7 path and exploit exact sampling coherence.** Today
   `m7ins` fetches the map and texel, looks up a ranked CGRAM value and
   writes LB0. Composition rereads LB0, merges sprites into IB, then
   rereads IB and four colour-plane tables to pack the output. Prototype
   a 16-pixel path that samples, selects the winning sprite/background
   index and packs four planes without those full intermediate passes.
   Keep a general fallback while validating the fused path.

   Cache the map/tile base while successive samples remain in the same
   source cell. Reuse an exact texel lookup when adjacent coordinates
   resolve to the same texel; this helps magnified regions, not every row.
   Use sprite coverage to identify groups requiring priority comparisons.
   Preserve colour-zero transparency, OBJ0 below BG1, OBJ1-3 above it,
   flips, wrapping, TILE0/FILL cases and the winner's original CGRAM index
   for histogram sampling. Quantised slot zero is not a transparency test.

   Earlier isolated measurements put the sampling loop near 20 two-clock
   DSP instruction cycles per pixel. At 168x256 samples and 16 million
   such cycles/s, that alone is about 53.8 ms. Consequently, small c2p
   improvements cannot suffice. A fused kernel around 7-8 such cycles
   per Mode 7 pixel is an experimental budget, not a predicted result;
   it would leave about 18-21 ms of a 40 ms compute budget for all other
   work. Establish whether that is attainable before promising 25 fps.

4. **Prepare sprite membership per OAM segment and cache decoded rows.**
   `obj_draw` scans the uploaded OAM list on each row, builds slivers,
   fetches packed VRAM through `vwn`/`xl_read`, and expands 4-bpp pixels
   again. Build compact enter/leave events or row membership once per
   OAMSET on the DSP; reset/rebuild at each split-screen OAM change.
   Preserve OAM ordering and the reference's 32-sprite/34-sliver limits.

   Cache decoded character rows and their opaque masks by VRAM address
   and generation. Keep palette/priority separate to avoid invalidating
   decoded graphics when the adaptive slot map changes. Measure packed
   VRAM decode cost and cache hits independently. Reserve cache and ring
   storage by removing buffers the fused path no longer needs; the
   existing DSP memory map is already tight.

5. **Keep tile backgrounds in masks/plane groups where practical.** SMK
   already caches tile-row descriptors in `ren`; add reuse of decoded
   character rows and avoid repeated layer setup when its fields are
   unchanged. CT's plane-mask composition is worth adapting to four
   output planes. Its eight CGRAM-index planes cannot simply be truncated:
   SMK's adaptive CGRAM-to-slot map must be applied and keyed correctly.
   Profile sky rows separately from Mode 7 rows.

6. **Reduce CPU preparation and remove frame-boundary idle time.** The
   fresh stream build costs 95.10 ms even before the DSP starts, and
   disabling HDMA cuts it by 70.86 ms. Use compact channel-specific HDMA
   loops that emit resolved register events; preserve the generic replay
   as the checker. `Regs.dirty` already avoids comparing all fields on
   unchanged rows, so adding that flag again is not a solution.

   Cache CGRAM colour grouping and palette inputs so unchanged work can
   return before expensive clustering/mapping. Historical async profiles
   put palette/sample completion near 42 ms (same patched Hatari build),
   making it another CPU priority. Keep equivalent palette results first;
   reducing palette-update frequency is a separate visual-policy choice.

   Give completed-frame presentation a safe VBL path independent of
   `rt_main_busy`, while retaining main-loop-paced SNES IRQ/NMI snapshots
   that fixed the missing karts. `smkx_render` currently returns after
   finishing an asynchronous frame; the next submission happens on a
   later call. Avoid that submission gap with stable frame state and
   correctly owned screen/feed buffers. Include host-feed interrupt time
   in CPU cost. Do not reuse VRAM, palettes or a feed buffer still owned
   by an in-flight frame.

## Acceptance budget

A useful first gate is pixel-only output near 30 ms on the 32 MHz route,
with zero differing words. The next is a repeatable full race-frame DSP
time below 80 ms, then below 50 ms, before a sub-40 ms acceptance gate.
Those are milestones, not measured gains or forecasts.

For sustained 25 fps, aim for at most roughly 32-35 ms of DSP compute
including output service, enough overlap to keep SSI supplied, and a
small start/drain margin. The 68030 separately needs game execution plus
all renderer preparation/service below 40 ms per new game frame. Earlier
game-only figures already exceed that; a 39 ms DSP cannot by itself make
the game run at 25 fps. First-frame latency and steady-state displayed
frame intervals must be reported separately, including VBL alignment.

Use frozen one- and two-player race snapshots, crowded OAM, sky/HUD,
title and mode transitions. Report typical and worst frame times and
VRAM-upload spikes. Compare planes and histogram counts against
`src/snes_ppu.c`, preserve `src/renderer.c` as the byte-format reference,
and update planar/snapshot tests whenever the output format changes.
Recheck on a physical Falcon before treating emulator throughput as a
hardware result.

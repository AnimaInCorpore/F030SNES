# DSP renderer budget: 40 ms target and the SSI milestone

2026-09-25. Review of the current CT renderer, with a fresh six-scene
PPUTEST run. No renderer implementation changes were made.

The first useful milestone is to keep SSI supplied and approach its
transfer time. The current renderer already overlaps output with drawing;
adding an asynchronous transfer mechanism is not the missing optimisation.
A full 256x224 eight-plane frame cannot cross SSI in under 40 ms on the
stock Falcon. That final target also requires a different output strategy.

## What already overlaps

`falcon/dsp/ppu.asm` writes completed plane words to `RING`, a 512-word
buffer. `tx_isr` feeds SSI, which shifts bits independently while DSP code
runs `layer_row`, `obj_row` and `compose`. The previous row can therefore
leave the DSP while it draws sprites for the next row. `gate` starts or
restarts transmission, `room` prevents the producer overtaking the reader,
and the sent marker prevents reading unwritten words.

The interrupt handler itself still consumes DSP cycles. There are not two
independent DSP instruction streams. A useful approximate pipeline model is:

    elapsed = startup + max(drawing including output service, wire time) + drain

Do not add the complete drawing time to the complete transfer time. The
ring holds 1,024 output bytes, or four scanlines; at the measured blank-frame
rate that buffers only about 1.36 ms. Enlarging it can absorb short stalls,
but cannot fix drawing that is slower than output on average.

## Baseline and limits

Fresh emulated results: patched Hatari `v2.6.1-497-g639fadf6`, binary
`../../../F030Arcade/third_party/hatari/build/src/hatari`, build banner
2026-09-22 21:37:14. These are not physical Falcon measurements.

| Test | Render + record after the stream was sent |
| --- | ---: |
| Field 1 / 2 | 171.02 / 170.99 ms |
| Menu 1 / 2 | 164.27 / 164.24 ms |
| Title 1 / 2 | 197.32 / 205.05 ms |
| Forced blank, including its short command stream | 76 ms (integer report) |

All six real frames and the blank frame had zero differing words. Log:
`build/ctx/advice-baseline-stderr.log`. Fresh kernel and test executable:
`build/dsp-advice/`. The old `pputest-stderr.log` is a failed historical run;
it was not used as this baseline. This run did not repeat random-frame tests.

PPUTEST excludes CPU planning and VRAM upload from render+record. Its
separate "frame send" timer also includes capture-buffer clearing and
record setup, so it is not a pure host-port throughput measurement.
Earlier stage estimates in `analysis/ct.md` are slivers ~54 ms,
composition ~41 ms and sprites ~15 ms for a field frame. They are useful
priorities, not additive independent costs: feature-removal profiles alter
pipeline overlap and waiting.

Each complete frame is 57,344 bytes. At 40 ms that requires more than
1.4336 MB/s of useful output, before any margin. Atari's hardware reference
states that SSI and each sound DMA channel peak at 1 MB/s. The 32 MHz
crossbar clock gives an 8 Mbit/s serial stream: a theoretical minimum of
57.344 ms per full frame. The current 25.175 MHz / 4 bit clock gives a
72.89 ms ideal minimum, versus the measured 76 ms blank path.
Source: [Atari Falcon030 Developer Support Package, Hardware p. 21](https://www.atarimania.com/documents/Atari-Falcon030-Developer-Support-Package.pdf).

## Priorities for approaching the transfer limit

1. **Measure starvation before changing the buffer.** Add cumulative DSP
   execution-cycle profiling for drawing, SSI service and `room` waiting;
   count ring-empty stops (separate startup and final drain), minimum fill,
   and time spent empty. Use a repeat-frame command with resident VRAM to
   separate kernel throughput from host planning/upload. A nearly continuous
   SSI stream and time waiting in `room` indicate the desired output-bound
   state. Measure whole frames, avoiding a CPU timer call per scanline.

2. **Cache layer configuration and decoded tile rows.** `layer_row` derives
   map dimensions, character base and scroll factors on every row.
   Split configuration changes from row advancement. `render` also resets
   `CAKEY`, `OBJKEY` and descriptor keys each frame: preserve reusable work
   with explicit register, VRAM-generation and page-slot invalidation.
   Cache a tile row's plane bytes, opacity and horizontal reverse by VRAM
   address/bpp/generation, so repeated slivers do not keep splitting the
   same raw words. Keep palettes and priority separate where possible.
   Budget cache memory explicitly: CT currently reserves 77 VRAM pages,
   while these six scenes use 22-29; that is an opportunity, not evidence
   that every battle can use a smaller page cache safely.

3. **Reduce the inner composition loop's memory and address work.** In
   `comp_group`, every BG plane loads an offset, has two explicit `nop`
   slots around address generation, gathers three slivers, and merges
   through `ACC`. Prototype fixed 4-bpp/2-bpp handlers with stepped pointers,
   useful work scheduled in AGU delay slots, and an aligned-scroll path
   that needs only two source bytes. Preserve the existing coverage masks
   and early exit. Accumulate `ANYLO`/`ANYHI` during sliver generation rather
   than scanning each completed strip twice in `any_mask`.

4. **Spend internal program RAM on repeated work.** The fresh listing has
   `slivers` and `comp_group` below P:$0200 already; moving those there again
   is not a win. Startup/loader code also occupies internal P, while
   `compose`, layer setup and sprite routines run externally. Reclaim
   startup-only space after boot or use bounded internal-P overlays for
   the hottest remaining loops. Keep the SSI vector/handler resident.
   Add assembly size guards and account for external X/Y/P bus conflicts;
   do not assume external X and Y accesses are independent buses.

5. **Reduce sprite work after the BG costs.** `obj_row` scans the visible
   segment's sprites on every line. Build row-entry/exit buckets per OAM
   segment and retain an ordered active list, including Y wrap. Preserve
   the reference's OAM order, 32-sprite and 34-sliver rules. Reuse decoded
   tile rows for sprites too. The title is an important stress case.

6. **Tune output service only when measured.** `compose` calls `gate` for
   each of 16 groups, with more calls around layer and sprite work. Once
   restart behaviour is instrumented, reduce redundant checks or schedule
   restarts around larger batches. A larger ring can cover descriptor
   rebuild bursts. Neither change substitutes for reducing drawing work.

At 76 ms, the average budget is about 339 us per scanline, roughly 10,857
DSP clocks at 32 MHz, including interrupt service. Current scene elapsed
times need approximately 2.2-2.7x improvement to approach that milestone.
Target slightly above the blank-frame result first; startup and drain make
exact equality unrealistic.

## The additional change needed for under 40 ms

For unchanged 256-colour, full-frame output, first benchmark host-port
readback with a small 68030 assembly loop and the Videl active. Include
packing, ST-RAM stores and the CPU time taken away from game execution.
Require comfortably more than 1.4336 MB/s; about 1.8-2 MB/s useful throughput
would leave margin. This is an experiment, not a measured capability here.
Atari documents the Falcon host port as CPU-driven programmed I/O, not
autonomous DMA (same reference, Host Port section).

Dirty tile/rectangle output can preserve 256 colours and make static fields
and menus much cheaper, but cannot guarantee 40 ms for arbitrary scrolling
or full-screen animation. It also needs a coherent previous frame in each
back buffer, correct invalidation, and efficient placement of sparse data.
Compression needs CPU reconstruction and a worst-case escape path.

Four-plane output halves the payload: the ideal SSI minimum at 32 MHz is
28.672 ms. That is an explicit reduction to 16 simultaneous indexed colours
unless a separately validated palette scheme is added, so it is not the
default recommendation for CT. A faster SSI clock alone cannot meet the
eight-plane target.

Finally, kernel time is not displayed frame cadence. `smkx_render` finishes
an in-flight frame and returns for the flip; it starts the next render on
a later call. Remove this avoidable scheduling gap when changing buffer
ownership, keeping each frame's palette attached to it. Measure CPU
planning, dirty uploads, game/NMI execution and presentation separately.
The older title game/NMI measurement in `analysis/ct.md` was about 80 ms;
renderer success alone would not establish 25 game frames per second.

Validate each implementation step with `make check-dsp-model`,
`make test-ppu` (real plus random cases), and runtime snapshot comparisons.
Keep `src/snes_ppu.c` as the reference. Test warm and cold VRAM caches,
scrolling, tile animation, IRQ/OAM changes and sprite-heavy scenes, then
confirm the budget on a physical Falcon.

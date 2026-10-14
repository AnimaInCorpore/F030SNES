/* The DSP renderer's driver on the Falcon (falcon/dsp/ppu.asm).
 *
 * The DSP keeps the whole VRAM and draws every line: BG layers, sprites,
 * Mode 7, composing, the c2p. Per frame the CPU sends the 256-word VRAM
 * pages written since the last frame, the colour tables when the palette's
 * slot map changes, and the frame stream (src/ppu_dsp.c: the register
 * fields that change, line by line, and OAM segments). The planes go over
 * the SSI into DMA record, one window per frame, 72 words a line: after
 * the 64 plane words come 16 samples (the CGRAM index of every 16th
 * pixel), which the Videl's line offset hides and the palette counts.
 *
 * The commands go by DMA playback into the DSP's SSI receive when the DSP
 * took SSION at init (smkx_m7_ssi): one buffer a frame (the pages, the
 * colour tables, the stream), built in memory and played once the record
 * window is armed; the 68030 writes no host-port word. Without the route
 * the stream goes over the host port from an interrupt as before.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "smkx_m7.h"
#include "ppu_dsp.h"
#include "smkx_prof.h"

extern uint8_t rt_vram[];
extern uint8_t rt_vram_dirty[128];         /* per 256-word VRAM page */
extern void rt_m7_record(void *start, uint32_t bytes);
extern int rt_m7_record_done(void);         /* also clears the data cache */
extern const uint8_t ppu_ext[], ppu_ext_end[];
extern uint8_t rt_pitch72;                  /* the snapshot copy skips the samples */
extern void rt_feed_install(void);
extern void rt_feed_start(const uint32_t *words, uint32_t n);
extern void rt_feed_more(const uint32_t *end);
extern int rt_feed_done(void);
extern void rt_ssi_play(const uint16_t *start, uint32_t bytes);
extern int rt_ssi_play_done(void);

uint8_t smkx_m7_on;
uint32_t smkx_m7_rows, smkx_m7_segs, smkx_m7_pages, smkx_m7_timeouts;
uint32_t smkx_m7_spins;                     /* host-port polls that found it full */
uint32_t smkx_m7_wait;                      /* last frame: waiting for the window, 1/38400 s */
uint32_t smkx_m7_t_up, smkx_m7_t_stream;    /* last frame: uploads, the stream */
uint32_t smkx_t_up, smkx_t_stream;          /* the same, cumulative (timing line) */
uint32_t smkx_m7_npages;                    /* last frame: VRAM pages sent */
uint8_t smkx_m7_ssi;                        /* the commands go by DMA playback */
uint8_t smkx_m7_stale;                      /* the list compositor's program runs in the DSP (SMKX_LC) */
uint32_t smkx_m7_slots;                     /* last frame: 16-bit words played */

/* The frame's commands as the DSP reads them from the SSI (ppu.asm's
 * get_x): a protocol word is two slots (bits 23-8, then 7-0), a VRAMSET or
 * COLOUR data word one. Enough for every VRAM page, the tables and a
 * 16384-word stream. */
#define SLOTS (128 * (6 + 256) + 2 + 1024 + 2 * 16384 + 8)
static uint16_t sbuf[SLOTS];
static uint32_t nslot;
static uint8_t slot_over;                   /* the frame did not fit */
static uint8_t skipped;                     /* so only its uploads went */
uint32_t smkx_m7_skips;
static void s24(uint32_t w)
{
    if (nslot + 2 > SLOTS) { slot_over = 1; return; }
    sbuf[nslot++] = (uint16_t)(w >> 8);
    sbuf[nslot++] = (uint16_t)(w & 0xFF);
}
static void s16(uint32_t w)
{
    if (nslot + 1 > SLOTS) { slot_over = 1; return; }
    sbuf[nslot++] = (uint16_t)w;
}

#define HSR (*(volatile uint8_t *)0xFFFFA202)
#define HTX (*(volatile uint32_t *)0xFFFFA204)  /* $A204 is unused: a long
                                                   write sends TXH, TXM, TXL */
#define HRX (*(volatile uint32_t *)0xFFFFA204)

static void put(uint32_t w)
{
    while (!(HSR & 2)) smkx_m7_spins++;
    HTX = w;
}

static uint32_t w24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

static uint8_t sent_map[256];
static int map_valid;

/* The program beyond the boot image goes in (LOAD), the DSP is asked for its answer, and it is told to take
 * its commands from the SSI. 0: it does not answer. */
static int m7_load(void)
{
    for (const uint8_t *e = ppu_ext; e < ppu_ext_end;) {   /* LOAD the rest */
        uint32_t n = w24(e + 6);
        put(6);
        put(w24(e));
        put(w24(e + 3));
        put(n);
        for (uint32_t i = 0; i < n; i++) put(w24(e + 9 + i * 3));
        e += 9 + n * 3;
    }
    uint32_t spin;
    put(4);                                                  /* PING */
    for (spin = 0; spin < 4000000 && !(HSR & 1); spin++) {}
    if (!(HSR & 1)) return 0;
    if ((HRX & 0xFFFFFF) != 0x505055) return 0;
    put(3);                                                  /* SSION */
    for (spin = 0; spin < 4000000 && !(HSR & 1); spin++) {}
    smkx_m7_ssi = (HSR & 1) && (HRX & 0xFFFFFF) == 1;
    for (int i = 0; i < 128; i++) rt_vram_dirty[i] = 1;
    map_valid = 0;
    return 1;
}

int smkx_m7_init(void)
{
    if (!m7_load()) return 0;
    rt_feed_install();
    smkx_m7_on = 1;
    rt_pitch72 = 0;                                          /* 64-word lines */
    *(volatile uint16_t *)0xFFFF820E = SMKX_LINE_WORDS - 64;   /* line offset */
    return 1;
}

#ifdef SMKX_LC
/* The list compositor ran its own program in the DSP (src/tile/smkx_tile.c booted it): SMK's is booted again
 * (the reset line, the 512-word bootstrap, then the same loader as at the start) and every VRAM page is sent
 * anew. Supervisor mode. A DSP that does not answer leaves the CPU to draw. */
extern void rt_dsp_boot(const uint8_t *image, uint32_t bytes);
extern const uint8_t m7_kernel_full[];
extern const uint32_t m7_kernel_full_bytes;

extern uint32_t smkx_tile_stat[];

int smkx_m7_revive(void)
{
    uint32_t t0 = prof_now();
    rt_dsp_boot(m7_kernel_full, m7_kernel_full_bytes);
    int ok = m7_load();
    smkx_tile_stat[7] += prof_now() - t0;
    if (!ok) {
        smkx_m7_on = 0;
        rt_pitch72 = 0;
        *(volatile uint16_t *)0xFFFF820E = 0;
        return 0;
    }
    smkx_m7_stale = 0;
    return 1;
}
#endif

static void upload(const uint8_t *map)
{
    for (int p = 0; p < 128; p++) {
        if (!rt_vram_dirty[p]) continue;
        rt_vram_dirty[p] = 0;
        smkx_m7_pages++;
        const uint8_t *v = rt_vram + p * 512;
        if (smkx_m7_ssi) {
            /* VRAMSET p * 256, 256 words: SLOTS has room for every page, so
             * no check per word; the VRAM shadow's bytes come swapped */
            uint16_t *o = sbuf + nslot;
            o[0] = 0; o[1] = 5;
            o[2] = (uint16_t)p; o[3] = 0;
            o[4] = 1; o[5] = 0;
            const uint16_t *w = (const uint16_t *)v;
            uint16_t *d = o + 6;
            for (int i = 0; i < 256; i++) {
                uint16_t x = w[i];
                d[i] = (uint16_t)(x << 8 | x >> 8);
            }
            nslot += 6 + 256;
            continue;
        }
        put(5);                                              /* VRAMSET */
        put((uint32_t)p * 256);
        put(256);
        for (int i = 0; i < 256; i++, v += 2) put((uint32_t)v[0] | (uint32_t)v[1] << 8);
    }
    int same = map_valid;
    for (int i = 0; same && i < 256; i++) same = sent_map[i] == map[i];
    if (!same) {
        if (smkx_m7_ssi) {
            uint16_t *o = sbuf + nslot;                      /* COLOUR */
            o[0] = 0; o[1] = 2;
            o += 2;
            for (int b = 0; b < 4; b++)
                for (int i = 0; i < 256; i++) *o++ = (uint16_t)(((map[i] >> b) & 1) << 8);
            nslot += 2 + 1024;
        } else {
            put(2);                                          /* COLOUR */
            for (int b = 0; b < 4; b++)
                for (int i = 0; i < 256; i++) put(((uint32_t)(map[i] >> b) & 1) << 8);
        }
        for (int i = 0; i < 256; i++) sent_map[i] = map[i];
        map_valid = 1;
    }
}

/* The frame stream goes out from an interrupt (falcon/smkx_m7dsp.s) while
 * it is built: the DSP starts drawing the first lines while the 68030 is
 * still walking the later ones, and the 68030 returns to the game while
 * the DSP draws. Words are published every FEED_STEP; a full buffer is
 * drained by the interrupt and then refilled from the start. */
#define FEED_MAX 8192
#define FEED_STEP 32
static uint32_t feed[FEED_MAX];
static uint32_t nfeed, nsent;
static uint32_t polls;
uint32_t smkx_m7_refills;                   /* times the buffer ran full */

static void to_feed(uint32_t w)
{
    if (nfeed == FEED_MAX) {
        rt_feed_more(feed + nfeed);
        while (!rt_feed_done()) {}
        smkx_m7_refills++;
        nfeed = nsent = 0;
        rt_feed_start(feed, 0);
    }
    feed[nfeed++] = w;
    if (nfeed - nsent >= FEED_STEP) {
        nsent = nfeed;
        rt_feed_more(feed + nfeed);
    }
}

void smkx_m7_start(const PpuInput *in, uint16_t *screen)
{
    uint32_t t = prof_now(), pages0 = smkx_m7_pages;
    if (smkx_m7_ssi) {
        /* the last frame's padding may still be going out */
        while (!rt_ssi_play_done()) {}
        nslot = 0;
        slot_over = 0;
        upload(in->map);                                     /* (always fits) */
        uint32_t nup = nslot;
        smkx_m7_npages = smkx_m7_pages - pages0;
        uint32_t u = prof_now();
        smkx_m7_t_up = u - t;
        uint16_t *e = ppu_dsp_frame_slots(in, sbuf + nslot, sbuf + SLOTS);
        if (e) nslot = (uint32_t)(e - sbuf);
        else slot_over = 1;
        skipped = slot_over;
        if (skipped) {                                       /* uploads only */
            nslot = nup;
            smkx_m7_skips++;
        }
        while (nslot & 7) s24(0);                            /* no-ops: whole frames */
        smkx_m7_slots = nslot;
        if (!skipped) rt_m7_record(screen, ((uint32_t)PPU_H * SMKX_LINE_WORDS + SMKX_HIST_WORDS) * 2);
        rt_ssi_play(sbuf, nslot * 2);
        smkx_m7_t_stream = prof_now() - u;
        smkx_t_up += smkx_m7_t_up;
        smkx_t_stream += smkx_m7_t_stream;
        smkx_m7_rows += PPU_H;
        polls = 0;
        return;
    }
    upload(in->map);
    smkx_m7_npages = smkx_m7_pages - pages0;
    uint32_t u = prof_now();
    smkx_m7_t_up = u - t;
    rt_m7_record(screen, ((uint32_t)PPU_H * SMKX_LINE_WORDS + SMKX_HIST_WORDS) * 2);
    nfeed = nsent = 0;
    rt_feed_start(feed, 0);
    ppu_dsp_frame(in, to_feed);
    rt_feed_more(feed + nfeed);
    smkx_m7_t_stream = prof_now() - u;
    smkx_m7_rows += PPU_H;
    polls = 0;
}

int smkx_m7_poll(void)
{
    if (smkx_m7_ssi ? rt_ssi_play_done() && (skipped || rt_m7_record_done())
                    : rt_feed_done() && rt_m7_record_done()) return 1;
    if (++polls > 2000000) {            /* stalled: the CPU renders from now on */
        smkx_m7_timeouts++;
        smkx_m7_on = 0;
        rt_pitch72 = 0;
        *(volatile uint16_t *)0xFFFF820E = 0;
        return -1;
    }
    return 0;
}

void smkx_m7_samples(const uint16_t *screen, uint32_t *hist)
{
    /* the DSP's histogram after the last line: each group's first pixel,
     * which counts 16 */
    const uint16_t *h = screen + PPU_H * SMKX_LINE_WORDS;
    for (int i = 0; i < 256; i++) hist[i] += (uint32_t)h[i] * 16u;
}

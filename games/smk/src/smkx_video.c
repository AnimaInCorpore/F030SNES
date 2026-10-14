/* Falcon side of the video adapter: builds the renderer's input from the
 * runtime's state (falcon/smkx_rt.s), keeps the adaptive palette, composes a
 * frame into the screen and loads the Videl palette.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "snes_ppu.h"
#include "ppu_fast.h"

/* SMKX_REF_RENDER: compose with the reference renderer instead (for
 * before/after timing; the output is the same). */
#ifdef SMKX_REF_RENDER
#define compose_frame ppu_render
#define planes_frame(c, s) ppu_planar(c, s)
#else
#define compose_frame ppu_fast_render
#define planes_frame(c, s) ppu_c2p4(c, s, PPU_W * PPU_H / 16)
#endif
#include "palette.h"
#include "smkx_m7.h"
#include "smkx_prof.h"

extern uint8_t rt_vram[], rt_cgram_start[], rt_oam[], rt_io_start[], rt_sram[];
extern uint32_t rt_noamseg;
extern PpuOamSeg rt_oamseg[];
extern uint8_t smkx_wram[], smkx_rom[];
extern uint16_t rt_bgofs_start[], rt_m7_start[], rt_m7ofs_start[];
extern PpuWrite rt_irqlog[];
extern uint16_t rt_nirqlog;
extern uint8_t rt_vram_dirty[128];         /* written 256-word VRAM pages */
extern const int16_t smkx_oklab[];

static uint8_t chunky[PPU_W * PPU_H];
static PpuBus bus;
static Palette pal;
static uint32_t hist[256];
static int have_hist;

/* Snapshot support: the palette the last frame was mapped with, and what
 * palette_update() made of that frame's counts. */
uint8_t smkx_pal_used[PAL_SNAP_BYTES];
uint8_t smkx_pal_next[PAL_SNAP_BYTES];

#ifdef SMKX_PROFILE
/* Per render, in 1/38400 s (smkx_prof.h), read by the runtime's report:
 *  0 palette_update       1 the DSP frame: uploads, stream, waiting
 *  2 VRAM pages and colour tables up  3 waiting for the record window
 *  4 the frame stream (HDMA, writes)  7 VRAM pages sent (a count)
 *  5 ppu_render, CPU only (fallback)
 *  6 ppu_planar, CPU only             8 host-port stalls this frame */
#define SMKX_PROF_FIELDS 23
uint32_t smkx_prof[SMKX_PROF_FIELDS];
#ifdef PPU_PROFILE
static uint32_t clock_fn(void) { return prof_now(); }
#endif
#endif

void smkx_video_init(void)
{
    bus.wram = smkx_wram;
    bus.sram = rt_sram;
    bus.rom = smkx_rom;
}

/* The Videl colours for the frame about to be shown. The runtime writes
 * them at the VBL where the new screen base takes effect (rt_clut_due),
 * so image and colours change together whenever the frame was finished. */
uint32_t smkx_clut[PAL_COLOURS];

static void load_clut(const Palette *p, uint8_t inidisp)
{
    uint32_t *clut = smkx_clut;
    uint32_t rgb[PAL_COLOURS];
    palette_scaled(p, inidisp & 15, rgb);
    for (int s = 0; s < PAL_COLOURS; s++) {
        uint32_t c = (inidisp & 0x80) ? 0 : rgb[s];
        uint32_t r = (c >> 16) & 0xFC, g = (c >> 8) & 0xFC, b = c & 0xFC;
        clut[s] = (r << 24) | (g << 16) | b;
    }
}

/* The frame the DSP is drawing: its palette and INIDISP (the CLUT goes up
 * when it is shown). */
static int busy;
static Palette fpal;
/* Cumulative 68030 time (1/38400 s) for the timing line: the palette
 * update and the samples it counts. */
uint32_t smkx_t_pal, smkx_t_smp;
/* The next frame's palette is palette_update(finished frame's palette, its
 * counts, CGRAM at the next start). A sync finish computes it at once (the
 * snapshot keeps it: pal_ready 1, valid while CGRAM is as it was then); an
 * asynchronous one leaves it to the start (pal_ready 2), which would
 * otherwise redo it whenever CGRAM changed in between (in SMK's race, most
 * frames). */
static int pal_ready;
static uint8_t pal_cgram[512];
static uint8_t finidisp;
static uint16_t *fscreen;

/* The DSP's frame is in: samples to the histogram, the palettes it was
 * mapped with to the snapshot state, its CLUT. */
static void finish(int sync)
{
#ifdef SMKX_PROFILE
    uint32_t t0 = prof_now();
#endif
    uint32_t ts = prof_now();
    smkx_m7_samples(fscreen, hist);
    smkx_t_smp += prof_now() - ts;
    load_clut(&fpal, finidisp);
    palette_save(&fpal, smkx_pal_used);
    if (sync) {
        Palette next = fpal;
        palette_update(&next, hist, rt_cgram_start, smkx_oklab);
        palette_save(&next, smkx_pal_next);
        pal = next;
        for (int i = 0; i < 512; i++) pal_cgram[i] = rt_cgram_start[i];
        pal_ready = 1;
    } else {
        pal_ready = 2;
    }
    busy = 0;
#ifdef SMKX_PROFILE
    smkx_prof[9] = prof_now() - t0;
#endif
}

/* The frame the DSP was drawing, if it is in: 1 (finished: show it),
 * 2 (still drawing), 0 (none, or it stalled). Starts nothing: the VBL
 * calls it while SMK's main loop is busy (SMKX_PACE), when the SNES state
 * is not a frame's. */
#ifdef SMKX_LC
/* The list compositor (src/tile): 4 when it showed a picture, -1 when it declines the frame */
extern int smkx_tile_render(int sync);
extern int smkx_tile_poll(void);
extern int smkx_tile_active(void);
extern int smkx_m7_revive(void);
extern uint8_t smkx_m7_stale;
#endif

int smkx_render_poll(void)
{
#ifdef SMKX_LC
    if (smkx_tile_active()) return smkx_tile_poll() ? 4 : 0;
#endif
    if (!busy) return 0;
    int r = smkx_m7_poll();
    if (r == 0) return 2;
    busy = 0;
    if (r < 0) return 0;
    finish(0);
    return 1;
}

/* Called at a frame (the VBL, or SMK's wait for the NMI). Returns 0:
 * nothing composed (a black frame: the CLUT went black); 1: a frame is in
 * screen, show it; 2: the DSP is drawing one (show nothing yet); 3: the
 * frame the DSP was drawing is in screen: show it, then call again at once
 * to start the next (no frame-long gap with the DSP idle); with SMKX_LC
 * also 4: the list compositor's ring window is on the Videl already
 * (count a render, snapshot it), 5: the compositor has nothing new.
 * sync: finish before returning (snapshots, the last frame). */
/* ---- the render throttle (F030SMW's, src/smkx_video.c) -----------------------------------------------
 * smkx_render is called every frame; it starts a composition only when the renderer's share of the time allows it. A
 * composition's own cost (the 68030 work of starting a DSP frame, or planning, or a blocking CPU frame) sets when the
 * next may start: `spent * 100 / share`. The share adapts: every 25 frames, if the game lost more frames than the game
 * loses on its own (a frame handler still running when the next VBL came: rt_stuck; the base is measured in the
 * windows without a composition) it shrinks by a quarter, otherwise it grows a little. A frame the DSP is still
 * drawing is not throttled (the poll below finishes it); a snapshot's frame (sync) is not either. */
extern volatile uint32_t rt_stuck, rt_vblcount, rt_frames;
static uint32_t next_ok;            /* prof_now() ticks: no composition starts before */
static int share = 35;              /* percent of the time the renderer may take */
static uint32_t chk_vbl, chk_stuck, chk_frames;
uint32_t smkx_thr_stat;             /* share << 16 | base_lost << 8 | lost, of the last window */
static int base_lost = 3;           /* VBLs in 25 the game loses with no composition going on */
static int bad;                     /* windows in a row the renderer cost frames */
static int heavy;                   /* compositions of the current window that took 5 ms or more */
static int win_blocked;             /* ... in the current window */
static int blocked;                 /* the call waited for a CPU frame: it costs the game its frames whatever the share */

static void adapt(void)
{
    uint32_t v = rt_vblcount;
    if (!chk_vbl) {                                         /* the first call: the window starts here */
        chk_vbl = v | 1;
        chk_frames = rt_frames;
        chk_stuck = rt_stuck;
        return;
    }
    if (v - chk_vbl < 25) return;
    /* The game's loss in this window: VBLs that passed without a SNES frame (SMK's main loop paces the frames: its
     * time taken by the renderer shows as frames not run; F030SMW counts rt_stuck, frame handlers still running at the
     * next VBL, which is all that shows there) */
    int lost = (int)(v - chk_vbl) - (int)(rt_frames - chk_frames);
    if (lost < 0) lost = 0;
    if (lost > 25) lost = 25;
    if (win_blocked) {                                      /* a blocking frame stopped the game: says nothing about the share */
        win_blocked = 0;
        heavy = 0;
        chk_vbl = v;
        chk_frames = rt_frames;
        chk_stuck = rt_stuck;
        return;
    }
    if (!heavy) {
        base_lost = (base_lost * 3 + lost) / 4;             /* a quiet window: the game's own lag */
        bad = 0;
        if (share < 85) share += 6;                         /* and nothing to hold the renderer back */
    } else if (lost > base_lost + 10) {                     /* the renderer costs the game frames (not one stall: twice in a row) */
        if (++bad >= 2) share = share * 3 / 4 < 10 ? 10 : share * 3 / 4;
    } else {
        bad = 0;
        if (share < 85) share += 6;
    }
    smkx_thr_stat = (uint32_t)share << 16 | (uint32_t)base_lost << 8 | (uint32_t)lost;
    chk_vbl = v;
    chk_frames = rt_frames;
    chk_stuck = rt_stuck;
    heavy = 0;
}

static int render_work(uint16_t *screen, int sync);

int smkx_render(uint16_t *screen, int sync)
{
    if (busy) {
        int r;
        while ((r = smkx_m7_poll()) == 0 && sync) {}
        if (r == 0) return 2;
        if (r > 0) {
            finish(sync);
            if (!sync) return 3;        /* (a sync render draws its own) */
        }
        busy = 0;
    }
    if (sync) return render_work(screen, 1);                /* (a snapshot's frame is not throttled) */
    uint32_t t0 = prof_now();
    adapt();
    if ((int32_t)(t0 - next_ok) < 0) {
#ifdef SMKX_LC
        if (smkx_tile_active()) {                           /* the jobs in flight go on */
            smkx_tile_poll();
            return 5;
        }
#endif
        return 2;                                           /* throttled: the picture on the screen stays */
    }
    blocked = 0;
    int r = render_work(screen, 0);
    uint32_t spent = prof_now() - t0;
    if (blocked) {
        win_blocked = 1;
        next_ok = t0 + spent * 2;       /* the game gets as long as it was stopped before the next one */
        return r;
    }
    if (spent >= 192) heavy++;
    next_ok = t0 + spent * 100u / (uint32_t)share;
    return r;
}

static int render_work(uint16_t *screen, int sync)
{
#ifdef SMKX_LC
    /* The list compositor takes the tile-mode frames it covers (4: its ring window is on the Videl, 5: nothing
     * new); the adaptive palette starts from fresh counts when SMK's renderer comes back. */
    {
        int t = smkx_tile_render(sync);
        if (t >= 0) {
            have_hist = 0;
            pal_ready = 0;
            return t;
        }
    }
#endif
    if ((rt_io_start[0x100] & 0x80) && rt_nirqlog == 0) {
        load_clut(&pal, rt_io_start[0x100]);
        return 0;
    }
    if (!have_hist) {
        /* No counts yet: weigh every CGRAM entry once. */
        for (int i = 0; i < 256; i++) hist[i] = 1;
        have_hist = 1;
    }
#ifdef SMKX_PROFILE
    uint32_t t = prof_now(), u, spins0 = smkx_m7_spins;
    for (int i = 0; i < SMKX_PROF_FIELDS; i++) smkx_prof[i] = 0;
#ifdef PPU_PROFILE
    ppu_prof_clock = clock_fn;              /* the frame loop's stage clocks */
    for (int i = 0; i < PPU_PROF_STAGES; i++) ppu_prof[i] = 0;
#endif
#endif
    int same = pal_ready == 1;
    for (int i = 0; same && i < 512; i++) same = pal_cgram[i] == rt_cgram_start[i];
    if (!same) {
        /* (pal_ready: pal is finish()'s update; else from the counts) */
        uint32_t tp = prof_now();
        if (pal_ready) palette_update(&fpal, hist, rt_cgram_start, smkx_oklab), pal = fpal;
        else palette_update(&pal, hist, rt_cgram_start, smkx_oklab);
        smkx_t_pal += prof_now() - tp;
    }
    pal_ready = 0;
    for (int i = 0; i < 256; i++) hist[i] = 0;
#ifdef SMKX_PROFILE
    u = prof_now(); smkx_prof[0] = u - t; t = u;
#endif

    PpuInput in;
    in.vram = rt_vram;
    in.cgram = rt_cgram_start;
    in.oam = rt_oam;
    in.io = rt_io_start;
    in.bgofs = rt_bgofs_start;
    in.m7 = rt_m7_start;
    in.m7ofs = rt_m7ofs_start;
    in.abus = ppu_abus_hirom;
    in.aptr = ppu_aptr_hirom;
    in.ctx = &bus;
    in.log = rt_irqlog;
    in.nlog = rt_nirqlog;
    in.map = pal.map;
    in.hist = hist;
    in.oamseg = rt_oamseg;
    in.noamseg = (int)rt_noamseg;
    in.m7sink = 0;
#ifdef SMKX_LC
    if (smkx_m7_on && smkx_m7_stale) smkx_m7_revive();      /* (a failure leaves the CPU to draw) */
#endif
    if (smkx_m7_on) {
        /* The DSP draws the frame (VRAM pages go up there) while the 68030
         * goes on; the CPU composes only without it. */
        fpal = pal;
        finidisp = rt_io_start[0x100];
        fscreen = screen;
        smkx_m7_start(&in, screen);
        busy = 1;
#ifdef SMKX_PROFILE
        u = prof_now();
        smkx_prof[1] = u - t;
        smkx_prof[2] = smkx_m7_t_up;
        smkx_prof[4] = smkx_m7_t_stream;
        smkx_prof[7] = smkx_m7_npages;
#ifdef PPU_PROFILE
        for (int i = 0; i < PPU_PROF_STAGES; i++) smkx_prof[10 + i] = ppu_prof[i];
#endif
        smkx_prof[8] = smkx_m7_spins - spins0;
#endif
        if (!sync) return 2;
        int r;
        while ((r = smkx_m7_poll()) == 0) {}
        if (r > 0) {
            finish(1);
            return 1;
        }
        busy = 0;                       /* stalled: the CPU draws it below */
    }
    blocked = 1;                        /* the CPU draws the frame: the game waits for it */
    for (int p = 0; p < 128; p++)
        if (rt_vram_dirty[p]) {
            rt_vram_dirty[p] = 0;
            ppu_fast_invalidate_page(p);
        }
    for (int i = 0; i < 256; i++) hist[i] = 0;
#ifdef SMKX_PROFILE
    t = prof_now();
#endif
    compose_frame(&in, chunky);
#ifdef SMKX_PROFILE
    u = prof_now(); smkx_prof[5] = u - t; t = u;
#endif
    planes_frame(chunky, screen);
#ifdef SMKX_PROFILE
    u = prof_now(); smkx_prof[6] = u - t;
#endif
    load_clut(&pal, rt_io_start[0x100]);
    Palette next = pal;
    palette_save(&pal, smkx_pal_used);
    palette_update(&next, hist, rt_cgram_start, smkx_oklab);
    palette_save(&next, smkx_pal_next);
    return 1;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memset(void *d, int v, unsigned long n)
{
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)v;
    return d;
}

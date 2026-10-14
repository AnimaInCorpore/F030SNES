/* Falcon side of the video adapter: builds the renderer's input from the
 * runtime's state (falcon/smkx_rt.s) and composes a frame into the screen,
 * one of three ways:
 *   - the DSP renderer (falcon/dsp/ppu.asm, src/smkx_ppu.c): tile modes,
 *     eight planes, CGRAM as the Videl's 256 colours; it works on while
 *     the game runs, and smkx_render reports the frame when it is there;
 *   - the Mode 7 path (the DSP kernel for Mode 7 rows, the CPU for the
 *     rest): four planes with the adaptive 16-colour palette;
 *   - the CPU alone, eight planes, when the DSP renderer cannot take a
 *     frame (or there is no DSP route).
 * The DSP runs one program at a time; a scene that needs the other one
 * boots it (rt_dsp_boot, a few milliseconds).
 * With SMKX_LC (FF6X_LC=1, eight planes only) the list compositor (src/smkx_lc.c) comes first: for the tile frames
 * it covers (src/lc_plan.c) the DSP composes only the 16-pixel groups that changed into a scroll ring and the
 * Videl is pointed at a window of it; its pictures go up by themselves (the VBL's poll asks for a call every
 * frame while it is active), a frame it does not cover goes the ways above.
 * With SMKX_TC (FF6X_TC=1) the screen is true colour (RGB565) throughout:
 * the true-colour renderer (falcon/dsp/tc.asm, src/smkx_tc.c: every BG
 * mode, Mode 7 included) or, for a frame it cannot take, ppu_render_565 on
 * the CPU; every colour exact, no palette.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "snes_ppu.h"
#include "palette.h"
#include "smkx_m7.h"
#include "smkx_ppu.h"
#include "smkx_prof.h"

extern uint8_t rt_vram_r[], rt_cgram_start[], rt_oam[], rt_io_start[], rt_sram[];
extern uint32_t rt_noamseg;
extern PpuOamSeg rt_oamseg[];
extern uint8_t smkx_wram[], smkx_rom[];
extern uint16_t rt_bgofs_start[], rt_m7_start[], rt_m7ofs_start[];
extern PpuWrite rt_irqlog[];
extern uint16_t rt_nirqlog;
extern const int16_t smkx_oklab[];
extern uint8_t rt_m7_route;
extern void rt_vid_planes(int planes);
extern uint16_t *volatile rt_back;        /* the buffer the next frame goes into */
extern volatile uint8_t rt_snapdue;        /* a snapshot at the next frame */
extern int smkx_tc_runs;                    /* smkx_tc.c: the last plan's runs */

static PpuBus bus;
#if SMKX_LC
extern int smkx_lc_render(const PpuInput *in);
extern int smkx_lc_boot(void);
extern int smkx_lc_busy(void);
extern int smkx_lc_active(void);
extern void smkx_lc_pump(void);
extern void smkx_lc_set_depth(int d);
extern int smkx_lc_blank;
#ifndef LC_ON
#define LC_ON 1
#endif
uint32_t smkx_vid_stat[8];          /* render_start calls, forced-blank skips, lc handled, lc refused, dsp, cpu, boots, poll asks */
static int lc_on = LC_ON;           /* the list compositor draws the frames it covers */
static int lc_wait;                 /* frames to leave it alone after a boot it did not use */
static int lc_blank_skip;           /* the last lc_try skipped a forced-blank frame (no picture to snapshot) */
#endif
#if !SMKX_TC
static uint8_t chunky[PPU_W * PPU_H];
static Palette pal;
static uint32_t hist[256];
static int have_hist;
#endif

/* Snapshot support: the palette the last frame was mapped with, and what
 * palette_update() made of that frame's counts. */
uint8_t smkx_pal_used[PAL_SNAP_BYTES];
uint8_t smkx_pal_next[PAL_SNAP_BYTES];
uint32_t smkx_snap_planes = 4;          /* the frame on screen: 4 or 8 planes */

#ifdef SMKX_PROFILE
/* Per render, in 1/38400 s (smkx_prof.h), read by the runtime's report:
 *  0 palette_update       1 DSP uploads (VRAM pages, colour tables)
 *  2 ppu_render, DSP path (CPU rows, sprites, feeding the kernel)
 *  3 waiting for the record window    4 overlay and CPU rows' planes
 *  5 ppu_render, CPU only             6 ppu_planar, CPU only
 *  7 Mode 7 rows this frame           8 host-port stalls this frame
 *  9 plane words where the DSP and the CPU frame differ
 * 10-15 ppu_render stages, DSP path (hdma, bg, m7, obj, compose, m7 feed)
 * 16-21 ppu_render stages, CPU only     22 the kernel alone on the lines */
#define SMKX_PROF_FIELDS 23
uint32_t smkx_prof[SMKX_PROF_FIELDS];
static uint16_t cpu_planar[PPU_W * PPU_H / 4];
static uint32_t cpu_hist[256];
static uint32_t clock_fn(void) { return prof_now(); }
#endif

void smkx_video_init(void)
{
    bus.wram = smkx_wram;
    bus.sram = rt_sram;
    bus.rom = smkx_rom;
}

#if SMKX_TC
#define FRAME_PLANES 16                      /* RGB565 */
#else
#define FRAME_PLANES 8
#endif

#if !SMKX_TC
static uint8_t clut4_inidisp;                /* the four-plane frame's INIDISP */

static void load_clut(void)
{
    volatile uint32_t *clut = (volatile uint32_t *)0xFFFF9800;
    uint8_t inidisp = clut4_inidisp;
    uint32_t rgb[PAL_COLOURS];
    palette_scaled(&pal, inidisp & 15, rgb);
    for (int s = 0; s < PAL_COLOURS; s++) {
        uint32_t c = (inidisp & 0x80) ? 0 : rgb[s];
        uint32_t r = (c >> 16) & 0xFC, g = (c >> 8) & 0xFC, b = c & 0xFC;
        clut[s] = (r << 24) | (g << 16) | b;
    }
}

/* The Mode 7 path: four planes, the adaptive palette. */
static int render4(uint16_t *screen)
{
    if (!have_hist) {
        /* No counts yet: weigh every CGRAM entry once. */
        for (int i = 0; i < 256; i++) hist[i] = 1;
        have_hist = 1;
    }
#ifdef SMKX_PROFILE
    uint32_t t = prof_now(), u, rows0 = smkx_m7_rows, spins0 = smkx_m7_spins;
#endif
    palette_update(&pal, hist, rt_cgram_start, smkx_oklab);
    for (int i = 0; i < 256; i++) hist[i] = 0;
#ifdef SMKX_PROFILE
    u = prof_now(); smkx_prof[0] = u - t; t = u;
    for (int i = 1; i < SMKX_PROF_FIELDS; i++) smkx_prof[i] = 0;
    ppu_prof_clock = clock_fn;
    for (int i = 0; i < PPU_PROF_STAGES; i++) ppu_prof[i] = 0;
#endif

    PpuInput in;
    in.vram = rt_vram_r;
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
    in.m7sink = smkx_m7_begin(screen, pal.map);
#ifdef SMKX_PROFILE
    u = prof_now(); smkx_prof[1] = u - t; t = u;
#endif
    if (in.m7sink) {
        ppu_render(&in, chunky);
#ifdef SMKX_PROFILE
        u = prof_now(); smkx_prof[2] = u - t; t = u;
#endif
        smkx_m7_finish(chunky);
#ifdef SMKX_PROFILE
        u = prof_now();
        smkx_prof[3] = smkx_m7_wait;
        smkx_prof[4] = u - t - smkx_m7_wait;
        smkx_prof[7] = smkx_m7_rows - rows0;
        smkx_prof[8] = smkx_m7_spins - spins0;
#endif
    } else {
        ppu_render(&in, chunky);
        ppu_planar(chunky, screen);
    }
#ifdef SMKX_PROFILE
    /* The same frame through the CPU alone, into a scratch buffer, with
     * its counts discarded: timed, and compared with what the screen got. */
    for (int i = 0; i < PPU_PROF_STAGES; i++) {
        smkx_prof[10 + i] = ppu_prof[i];
        ppu_prof[i] = 0;
    }
    in.m7sink = 0;
    in.hist = cpu_hist;
    t = prof_now();
    ppu_render(&in, chunky);
    u = prof_now(); smkx_prof[5] = u - t; t = u;
    ppu_planar(chunky, cpu_planar);
    u = prof_now(); smkx_prof[6] = u - t;
    for (int i = 0; i < PPU_W * PPU_H / 4; i++) smkx_prof[9] += cpu_planar[i] != screen[i];
    for (int i = 0; i < PPU_PROF_STAGES; i++) smkx_prof[16 + i] = ppu_prof[i];
    smkx_prof[22] = smkx_m7_replay(cpu_planar);
#endif
    clut4_inidisp = rt_io_start[0x100];     /* loaded when the frame is shown */

    Palette next = pal;
    palette_save(&pal, smkx_pal_used);
    palette_update(&next, hist, rt_cgram_start, smkx_oklab);
    palette_save(&next, smkx_pal_next);
    return 1;
}
#endif

/* ---- eight planes -------------------------------------------------------- */

static uint8_t clut_cgram[512];              /* the frame's CGRAM and INIDISP, */
static uint8_t clut_inidisp;                 /* loaded when it is shown */

#if !SMKX_TC
static void load_clut8(void)
{
    volatile uint32_t *clut = (volatile uint32_t *)0xFFFF9800;
    int bright = clut_inidisp & 15;
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)(clut_cgram[i * 2] | clut_cgram[i * 2 + 1] << 8);
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        if (clut_inidisp & 0x80) r = g = b = 0;
        r = (r * (uint32_t)(bright + 1) / 16) << 3;
        g = (g * (uint32_t)(bright + 1) / 16) << 3;
        b = (b * (uint32_t)(bright + 1) / 16) << 3;
        clut[i] = (r << 24) | (g << 16) | b;
    }
}
#endif

#if SMKX_LC
/* The Videl palette of a list compositor picture (src/smkx_lc.c): loaded as the picture goes up. */
void smkx_set_palette(const uint8_t *cgram, uint8_t inidisp)
{
    for (int i = 0; i < 512; i++) clut_cgram[i] = cgram[i];
    clut_inidisp = inidisp;
    rt_vid_planes(8);
    smkx_snap_planes = 8;
    load_clut8();
}
#endif

static void input(PpuInput *in, const uint8_t *map, uint32_t *h)
{
    in->vram = rt_vram_r;
    in->cgram = rt_cgram_start;
    in->oam = rt_oam;
    in->io = rt_io_start;
    in->bgofs = rt_bgofs_start;
    in->m7 = rt_m7_start;
    in->m7ofs = rt_m7ofs_start;
    in->abus = ppu_abus_hirom;
    in->aptr = ppu_aptr_hirom;
    in->ctx = &bus;
    in->log = rt_irqlog;
    in->nlog = rt_nirqlog;
    in->map = map;
    in->hist = h;
    in->oamseg = rt_oamseg;
    in->noamseg = (int)rt_noamseg;
    in->m7sink = 0;
    in->subreloc = 0;                       /* the CPU path: a plain checkerboard */
}

enum { PROG_NONE, PROG_M7, PROG_PPU, PROG_LC };
static int prog, ppu_ok = 1;
/* a DSP frame not yet in the buffer; what the render context does (1
 * planning, 2 uploading and sending, 3 booting a DSP program, 4 the Mode 7
 * or CPU paths, 5 the rest of compose_frame): the runtime's timer ticks
 * read both */
volatile int smkx_inflight;
volatile uint8_t smkx_render_phase;
static uint8_t ident[256];
static uint32_t hist8[256];

/* How the composed frame is shown (smkx_render_present, at the flip): the
 * plane count and its palette. The paths only record it: the work may span
 * several VBLs, and the Videl must not switch mode or palette under the
 * frame that is still on screen. */
static int pres_planes = FRAME_PLANES;

void smkx_render_present(void)
{
    rt_vid_planes(pres_planes);
    smkx_snap_planes = (uint32_t)pres_planes;
#if !SMKX_TC
    if (pres_planes == 8) load_clut8();
    else load_clut();
#endif
}

/* Timing, in VBLs (smkx_render_poll counts them), as running averages
 * x4: how long the DSP takes for a frame (send to done) and how long the
 * next plan takes from its request (inputs taken) to its end. The next
 * frame is asked for so that its plan ends about when the DSP is done:
 * the DSP gets no gap, and the frame is not older than it has to be. */
static int dsp_avg4 = 10 * 4, plan_avg4 = 3 * 4;
volatile uint8_t smkx_render_waiting;       /* the render context waits for the DSP */
static int vbls_flight, vbls_asked, asked;
/* Pictures that change no row are skipped (smkx_tc.c): after a run of them the next composition is asked
 * for less often (each costs the plan's change detection), the run's length in VBLs at most 4. */
static int skip_streak, skip_wait;
/* Whether the render side is the limit: the VBLs the DSP waits, between its frame done and the next one
 * sent, as a running average x4. Partial rendering costs CPU (its change detection) to save DSP time,
 * so it is off while the DSP mostly waits for the CPU. */
static int gap_vbls, gap_open, gap_avg4;
volatile int smkx_cpu_bound;
int smkx_plan_early;                    /* the last plan was ready while the DSP still drew */
uint32_t smkx_done_at;                  /* prof_now() when the DSP's frame was found done (smkx_tc.c: the gap to the next send) */

/* In the VBL, every rendered frame. idle: no render work pending. Returns
 * bit 0: the DSP renderer's frame is in the back buffer (present it);
 * bit 1: ask for the next composition (from this frame's inputs). */
int smkx_render_poll(int idle)
{
    int r = 0;
    if (asked) vbls_asked++;
    if (smkx_inflight) {
        vbls_flight++;
        if (smkx_ppu_done()) {
            smkx_inflight = 0;
            smkx_done_at = prof_now() | 1;
            gap_open = 1;
            gap_vbls = 0;
            smkx_render_waiting = 0;        /* the wait is over: schedulable again */
            dsp_avg4 += vbls_flight - dsp_avg4 / 4;
            pres_planes = FRAME_PLANES;
            r |= 1;
        }
    }
#if SMKX_LC
    /* The list compositor wants a call every frame while it shows or still builds a picture (its jobs go up by themselves);
     * a snapshot due makes it stop asking, so that the work ends and the snapshot's synchronous render drains the jobs. */
    if (lc_wait > 0) lc_wait--;
    if (lc_on && (smkx_lc_active() || smkx_lc_busy())) {
        smkx_vid_stat[7]++;
        if (idle && !rt_snapdue) r |= 2;
        return r;
    }
#endif
    if (skip_wait > 0) skip_wait--;
    if (gap_open && !smkx_inflight) gap_vbls++;
    if (idle && !skip_wait && (!smkx_inflight || vbls_flight + plan_avg4 / 4 + 1 >= dsp_avg4 / 4)) {
        asked = 1;
        vbls_asked = 0;
        r |= 2;
    }
    return r;
}

/* The render context waits for the DSP's last frame (the VBL's poll takes
 * it and flips). Meanwhile the VBL does not hand it the CPU when the game
 * is busy (rt_vbl's RWAIT fallback): it would only spin. */

#if SMKX_TC && SMKX_PARTIAL_CHECK == 2
/* SMKX_PARTIAL_CHECK=2: the DSP draws each picture whole too (smkx_tc.c), and the partial one is compared with it */
static uint16_t *chk_buf;
static uint16_t *flight_buf;
extern void rt_cprint(const char *text);
extern void rt_cprint_hex(uint32_t value);
static uint16_t cpu_ref[PPU_W * PPU_H];
static int cpu_ref_valid;

static int rows_differ(const uint16_t *a, const uint16_t *b)
{
    int n = 0;
    for (int r = 0; r < PPU_H; r++) {
        int d = 0;
        for (int x = 0; x < PPU_W; x++) d |= a[r * PPU_W + x] != b[r * PPU_W + x];
        n += d;
    }
    return n;
}

static void chk_poll(void)
{
    if (!chk_buf || (smkx_inflight && chk_buf == flight_buf)) return;
    smkx_ppu_check_compare(chk_buf);
    if (cpu_ref_valid) {                    /* a third opinion for a few pictures: the CPU's own render */
        rt_cprint("SMKX CHECK CPU: drawn vs cpu ");
        rt_cprint_hex((uint32_t)rows_differ(chk_buf, cpu_ref));
        rt_cprint(" whole-dsp vs cpu ");
        rt_cprint_hex((uint32_t)rows_differ(smkx_ppu_check_screen(), cpu_ref));
        rt_cprint("\n");
        cpu_ref_valid = 0;
    }
    chk_buf = 0;
}
#define CHK_POLL() chk_poll()
#define CHK_ARM(buf, in) ((void)0)
#elif SMKX_TC && SMKX_PARTIAL_CHECK
/* SMKX_PARTIAL_CHECK: every picture the DSP draws or leaves as it was is compared, when it is complete,
 * with the CPU reference (ppu_render_565) of the inputs it was planned from. A partial picture that
 * differs anywhere is counted (PPU_ST_CHK_BAD) and reported. Slow (the 68030 renders each picture). */
extern void rt_cprint(const char *text);
extern void rt_cprint_hex(uint32_t value);
static uint16_t chk_ref[2][PPU_W * PPU_H];
static uint16_t *chk_buf[2];
static uint16_t *flight_buf;

static void chk_poll(void)
{
    for (int i = 0; i < 2; i++) {
        if (!chk_buf[i]) continue;
        if (smkx_inflight && chk_buf[i] == flight_buf) continue;        /* still being drawn */
        const uint16_t *a = chk_buf[i], *b = chk_ref[i];
        int bad = 0, first = -1;
        for (int r = 0; r < PPU_H; r++) {
            int d = 0;
            for (int x = 0; x < PPU_W; x++) d |= a[r * PPU_W + x] != b[r * PPU_W + x];
            if (d) { bad++; if (first < 0) first = r; }
        }
        smkx_ppu_stat[PPU_ST_CHK]++;
        if (bad) {
            smkx_ppu_stat[PPU_ST_CHK_BAD]++;
            rt_cprint("SMKX PARTIAL CHECK: rows differing ");
            rt_cprint_hex((uint32_t)bad);
            rt_cprint(" first row ");
            rt_cprint_hex((uint32_t)first);
            rt_cprint("\n");
        }
        chk_buf[i] = 0;
    }
}

static void chk_arm(uint16_t *buf, const PpuInput *in)
{
    chk_poll();
    int i = !chk_buf[0] ? 0 : !chk_buf[1] ? 1 : -1;
    if (i < 0) return;                      /* two checks are waiting: this picture is not checked */
    ppu_render_565(in, chk_ref[i]);
    chk_buf[i] = buf;
}
#define CHK_POLL() chk_poll()
#define CHK_ARM(buf, in) chk_arm(buf, in)
#else
#define CHK_POLL() ((void)0)
#define CHK_ARM(buf, in) ((void)0)
#endif
extern void rt_flip_c(void);                /* smkx_rt.s: present rt_back (interrupts off) */

/* The DSP's frame is complete and taken here, not by the VBL's poll: the same
 * bookkeeping (smkx_render_poll) and the flip, with interrupts off so that the
 * VBL does not take it too. */
static void take_done(void)
{
    uint32_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tor.w #0x700,%%sr" : "=d"(sr) : : "memory", "cc");
    if (smkx_inflight) {
        smkx_inflight = 0;
        smkx_done_at = prof_now() | 1;
        gap_open = 1;
        gap_vbls = 0;
        smkx_render_waiting = 0;
        dsp_avg4 += vbls_flight - dsp_avg4 / 4;
        pres_planes = FRAME_PLANES;
        rt_flip_c();
    }
    __asm__ volatile("move.w %0,%%sr" : : "d"((uint16_t)sr) : "memory", "cc");
}

/* Waits for the DSP's last frame. The render context spins on the DSP's
 * record window itself and presents the frame the moment it is complete: the
 * DSP then waits for the next stream only as long as the upload and send take,
 * not until the next VBL notices. */
static void wait_dsp(void)
{
    if (!smkx_inflight) return;
    smkx_render_waiting = 1;
    while (smkx_inflight) {
        if (smkx_ppu_done()) take_done();
    }
    smkx_render_waiting = 0;
    CHK_POLL();
}

#if SMKX_LC
/* A cheap look at the registers before the compositor is booted (booting the DSP into its program drops the page cache,
 * so a scene it refuses must not boot it every frame): the frames lc_plan would refuse for their mode, tile size or
 * mosaic. The planner has the last word (per-line changes, OAM segments, ...). */
extern int lc_covers(const PpuInput *in);
static int lc_likely(void)
{
    const uint8_t *r = rt_io_start + 0x100;       /* $2100 + n */
    if (r[0x00] & 0x80) return 0;                 /* forced blank at the start */
    if ((r[0x05] & 7) > 1 || (r[0x05] & 0xF0)) return 0;                 /* modes 2-7, 16x16 tiles */
    if ((r[0x06] & 0xF0) && (r[0x06] & 0x0F & r[0x2C])) return 0;        /* mosaic on a visible layer */
    return 1;
}

/* The list compositor: -1 not a frame it draws (the caller goes the other ways), 0 nothing new or a job on its way,
 * 1 a picture went up. sync (a snapshot): one job at a time, and wait for it. */
static int lc_try(const PpuInput *in, int sync)
{
    int running = smkx_lc_active() || smkx_lc_busy();
    lc_blank_skip = 0;
    smkx_lc_blank = 0;
    if (!running) {
        if (lc_wait > 0) return -1;
        if (!lc_likely() || !lc_covers(in)) {
            lc_wait = 4;                    /* not a frame it draws: look again in a few frames */
            return -1;
        }
    }
    if (prog != PROG_LC || (smkx_ppu_loaded() != DSP_KERNEL_LC && smkx_ppu_loaded() != DSP_KERNEL_LCW)) {
        wait_dsp();                         /* the DSP's last frame first */
        smkx_render_phase = 3;
        int ok = smkx_lc_boot();
        smkx_render_phase = 5;
        smkx_vid_stat[6]++;
        if (!ok) {
            lc_on = 0;
            prog = PROG_NONE;
            return -1;
        }
        prog = PROG_LC;
    }
    smkx_render_phase = 1;
    smkx_vid_stat[0]++;
    if (sync) {
        smkx_lc_set_depth(1);
        while (smkx_lc_busy()) smkx_lc_render(in);          /* the jobs planned ahead come first */
    }
    int r = smkx_lc_render(in);
    if (sync) {
        while (r == 0 && smkx_lc_busy()) r = smkx_lc_render(in);
        smkx_lc_set_depth(2);
    }
    lc_blank_skip = smkx_lc_blank;                          /* a forced-blank frame: hidden by the palette, nothing to snapshot */
    if (lc_blank_skip) smkx_vid_stat[1]++;
    if (r < 0) {
        smkx_vid_stat[3]++;
        if (!running) lc_wait = 8;                          /* it was booted for nothing: not again at once */
        prog = PROG_NONE;                                   /* (the old ways boot their own program) */
    } else {
        smkx_vid_stat[2]++;
    }
    return r;
}
#endif

/* Composes the frame whose inputs the runtime holds (rt_*_start): 0 when
 * the DSP renderer took it (smkx_render_poll reports it), 1 when it is in
 * rt_back (present it). sync: at once, the DSP renderer waited for. */
static int compose(int sync)
{
    smkx_render_phase = 5;
    CHK_POLL();
    if (!ident[1])
        for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
    PpuInput in;
    input(&in, ident, hist8);
    /* Forced blank at the start counts as not Mode 7 (for the fallback). */
    int mode = (rt_io_start[0x100] & 0x80) ? 0 : rt_io_start[0x105] & 7;
#if SMKX_LC
    if (lc_on && rt_m7_route && ppu_ok) {
        int r = lc_try(&in, sync);
        if (r >= 0) {
            smkx_render_phase = 0;
            return sync && !lc_blank_skip ? 2 : 0;      /* its picture is on the screen (or goes up by itself); sync: nothing to flip */
        }
    }
#endif
    /* The DSP renderer: the plan names the program the frame needs, ppu.asm
     * (tile modes) or its Mode 7 build; a switch reloads the DSP. A frame
     * neither takes (Mode 7 and tile rows mixed, too many sprite pages, too
     * long a stream) goes to the Mode 7 kernel or the CPU below. */
    if (rt_m7_route && ppu_ok) {
        smkx_render_phase = 1;
#if SMKX_TC
        smkx_ppu_sync = sync;
#endif
        int k = smkx_ppu_plan(&in);
        smkx_render_phase = 5;
#if SMKX_TC
        /* The first frame the DSP cannot take goes into a snapshot (when
         * the build has them, FF6X_SNAP_EVERY): its inputs, for the host. */
        static int snapped, mixed;
        if (k < 0 && !snapped) {
            snapped = 1;
            rt_snapdue = 1;
        }
        if (k >= 0 && smkx_tc_runs && mixed < 2) {     /* and the first mixed ones */
            mixed++;
            rt_snapdue = 1;
        }
#endif
        if (k >= 0) {
            if (asked) {
                plan_avg4 += vbls_asked - plan_avg4 / 4;
                asked = 0;
            }
#if SMKX_TC
            if (!smkx_ppu_skipped()) skip_streak = 0;
            if (smkx_ppu_skipped()) {           /* no row to draw */
                smkx_done_at = 0;               /* (the DSP idles by design) */
                CHK_ARM(smkx_ppu_newest(), &in);
#if SMKX_TC && SMKX_PARTIAL_CHECK == 2
                wait_dsp();                     /* (the last picture complete) */
                if (smkx_ppu_check_full()) smkx_ppu_check_compare(smkx_ppu_newest());
#endif
                skip_streak++;
                skip_wait = skip_streak > 6 ? 3 : skip_streak > 2 ? 1 : 0;
                smkx_render_phase = 0;
                if (smkx_ppu_skipped() == 1) return 0;   /* the picture on screen is current */
                pres_planes = FRAME_PLANES;
                return 1;                       /* (sync) the back buffer is */
            }
#endif
            if (prog != PROG_PPU || smkx_ppu_loaded() != k) {
                wait_dsp();
                prog = PROG_PPU;
                smkx_render_phase = 3;
                if (!smkx_ppu_boot(k)) {
                    ppu_ok = 0;
                    prog = PROG_NONE;
                }
                smkx_render_phase = 5;
            }
            smkx_plan_early = smkx_inflight && !smkx_ppu_done();   /* (the plan waits for the DSP) */
            if (smkx_plan_early) smkx_ppu_stat[PPU_ST_EARLY]++;
            wait_dsp();                     /* the DSP draws the last frame meanwhile */
            /* The render copy of VRAM changes only between frames' work
             * (rt_vram_sync), so the plan does not go stale meanwhile. */
            smkx_render_phase = 1;
            if (ppu_ok && (!smkx_ppu_stale() || smkx_ppu_plan(&in) == k)) {
                for (int i = 0; i < 512; i++) clut_cgram[i] = rt_cgram_start[i];
                smkx_ppu_palette(clut_cgram);   /* colour math the palette shows */
                clut_inidisp = rt_io_start[0x100];
                smkx_render_phase = 2;
                smkx_ppu_send(rt_back);
#if SMKX_TC && SMKX_PARTIAL_CHECK == 2
                flight_buf = rt_back;
                chk_buf = rt_back;
                if (smkx_ppu_pic() >= 0x94 && smkx_ppu_pic() <= 0x9C) {
                    ppu_render_565(&in, cpu_ref);
                    cpu_ref_valid = 1;
                }
#elif SMKX_TC && SMKX_PARTIAL_CHECK
                flight_buf = rt_back;
                CHK_ARM(rt_back, &in);
#endif
                smkx_render_phase = 5;
                vbls_flight = 0;
                if (gap_open) {
                    gap_open = 0;
                    gap_avg4 += gap_vbls - gap_avg4 / 4;
                    if (gap_avg4 >= 12) smkx_cpu_bound = 1;         /* 3 VBLs idle a picture */
                    else if (gap_avg4 <= 6) smkx_cpu_bound = 0;
                }
                if (!sync) {
                    smkx_inflight = 1;
                    return 0;
                }
                while (!smkx_ppu_done()) {}
                pres_planes = FRAME_PLANES;
                return 1;
            }
        }
    }
    smkx_render_phase = 4;
    asked = 0;
    wait_dsp();                             /* these paths use the DSP and rt_back now */
#if SMKX_TC
    (void)mode;
    ppu_render_565(&in, rt_back);           /* the CPU, true colour */
    smkx_ppu_drawn_full(rt_back);
    pres_planes = FRAME_PLANES;
    return 1;
#else
    if (rt_m7_route && mode == 7) {
        /* Mode 7 at the frame's start: the kernel and four planes */
        if (prog != PROG_M7) {
            prog = PROG_M7;
            if (!smkx_m7_boot()) prog = PROG_NONE;
        }
        pres_planes = 4;
        return render4(rt_back);
    }
    /* the CPU, eight planes */
    ppu_render(&in, chunky);
    ppu_planar8(chunky, rt_back);
    for (int i = 0; i < 512; i++) clut_cgram[i] = rt_cgram_start[i];
    clut_inidisp = rt_io_start[0x100];
    pres_planes = FRAME_PLANES;
    return 1;
#endif
}

static int compose_frame(int sync)
{
    int r = compose(sync);
    smkx_render_phase = 0;
    return r;
}

/* In the render context (rt_render_thread): may take several VBLs. */
int smkx_render_start(uint16_t *screen)
{
    (void)screen;                           /* rt_back when it is drawn into */
    return compose_frame(0);
}

/* In the VBL, all at once (snapshots: sync, the inputs and the planes are
 * then the same frame's). */
int smkx_render(uint16_t *screen, int sync)
{
    if (smkx_inflight) {
        if (!smkx_ppu_done()) {
            if (!sync) return 0;
            while (!smkx_ppu_done()) {}
        }
        smkx_inflight = 0;
        if (!sync) {
            /* the frame started earlier is in the back buffer */
            pres_planes = FRAME_PLANES;
            return 1;
        }
        /* a snapshot wants this frame's own planes: compose it again */
    }
    (void)screen;                           /* rt_back */
    return compose_frame(sync);
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

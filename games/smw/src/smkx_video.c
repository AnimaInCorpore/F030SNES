/* Falcon side of the video adapter (256-colour path): builds the renderer's
 * input from the runtime's state (falcon/smkx_rt.s), composes a frame of
 * CGRAM indices (ppu_fast.c), packs it into the eight-plane screen and
 * prepares the Videl palette from CGRAM, which the runtime loads at the
 * next VBL together with the new screen base.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "snes_ppu.h"
#include "ppu_fast.h"
#include "smkx_ppu.h"
#include "smkx_prof.h"
#include "lc_plan.h"

extern uint8_t rt_vram[], rt_cgram_start[], rt_oam[], rt_io_start[], rt_sram[];
extern uint32_t rt_noamseg;
extern PpuOamSeg rt_oamseg[];
extern uint8_t smkx_wram[], smkx_rom[];
extern uint16_t rt_bgofs_start[], rt_m7_start[], rt_m7ofs_start[];
extern PpuWrite rt_irqlog[];
extern uint16_t rt_nirqlog;
extern uint32_t rt_clut[256];
extern volatile uint8_t rt_clut_ready;
extern uint8_t rt_vram_dirty[128];  /* VRAM pages written since the last render */

static uint8_t chunky[PPU_W * PPU_H];
uint32_t smkx_c2p_ticks;            /* 200 Hz ticks spent packing planes */
static PpuBus bus;
static uint8_t identity[256];
static uint32_t hist[256];          /* scratch: only Mode 7 rows count into it */
extern int smkx_lc_render(const PpuInput *in);
extern int smkx_lc_boot(void);
extern int smkx_lc_busy(void);
#ifndef SMKX_LC_SYNC
#define SMKX_LC_SYNC 0
#endif
static int lc_on = 1;               /* the list compositor draws the frames it covers */
static int dsp_ok = 1;              /* the DSP renderer answered its last boot */
static uint8_t level[16][32];       /* 5-bit channel x brightness -> 6-bit Videl */

void smkx_video_init(void)
{
    bus.wram = smkx_wram;
    bus.sram = rt_sram;
    bus.rom = smkx_rom;
    for (int i = 0; i < 256; i++) identity[i] = (uint8_t)i;
    for (int b = 0; b < 16; b++)
        for (int c = 0; c < 32; c++)
            level[b][c] = (uint8_t)((c * 63 * b) / (31 * 15));
}

/* CGRAM (BGR555) scaled by INIDISP brightness, black in forced blank, in
 * the Videl's format: R and G in the top bits of the high bytes, B in the
 * low byte, six bits each. */
/* The Videl palette of a list compositor picture (src/smkx_lc.c): its job's CGRAM and brightness */
void smkx_set_palette(const uint8_t *cgram, uint8_t inidisp)
{
    const uint8_t *lv = level[(inidisp & 0x80) ? 0 : (inidisp & 15)];
    for (int i = 0; i < 256; i++) {
        unsigned c = cgram[i * 2] | (cgram[i * 2 + 1] << 8);
        uint32_t r = lv[c & 31], g = lv[(c >> 5) & 31], b = lv[(c >> 10) & 31];
        rt_clut[i] = (r << 26) | (g << 18) | (b << 2);
    }
    rt_clut_ready = 1;
}

static void make_clut(void)
{
    uint8_t inidisp = rt_io_start[0x100];
    const uint8_t *lv = level[(inidisp & 0x80) ? 0 : (inidisp & 15)];
    for (int i = 0; i < 256; i++) {
        unsigned c = rt_cgram_start[i * 2] | (rt_cgram_start[i * 2 + 1] << 8);
        uint32_t r = lv[c & 31], g = lv[(c >> 5) & 31], b = lv[(c >> 10) & 31];
        rt_clut[i] = (r << 26) | (g << 18) | (b << 2);
    }
    rt_clut_ready = 1;
}

/* Returns 0 when the frame was not composed: it starts in forced blank and
 * no IRQ writes a register, so it is black (tools/ppu_snap.c skips it the
 * same way). */
/* ---- the render throttle ------------------------------------------------------------------------------
 * smkx_render is called every frame; it starts a composition only when the renderer's share of the time allows it. A
 * composition's own cost (planning, or the blocking wait for a DSP or CPU frame) sets when the next may start:
 * `spent * 100 / share`. The share adapts: every 25 frames, if the game lost more frames than the game loses on its own (a frame
 * handler still running when the next VBL came: rt_stuck; the base is measured in the windows without a composition)
 * it shrinks by a quarter, otherwise it grows a little. A snapshot
 * build (SMKX_LC_SYNC) does not throttle: its pictures must be one instant with the snapshot's inputs. */
extern volatile uint32_t rt_stuck, rt_vblcount;
extern void smkx_lc_pump(void);
extern int smkx_lc_active(void);
static uint32_t next_ok;            /* prof_now() ticks: no composition starts before */
static int share = 35;              /* percent of the time the renderer may take */
static uint32_t chk_vbl, chk_stuck;

extern uint32_t smkx_lc_stat[];
static int base_lost = 6;           /* VBLs in 25 the game loses with no composition going on (it can be slower than 50 Hz alone) */
static int bad;                     /* windows in a row the renderer cost frames */
static int heavy;                   /* compositions of the current window that took 5 ms or more */
static int win_blocked;             /* ... in the current window */
static int blocked;                 /* the call waited for a DSP or CPU frame: it costs the game its frames whatever the share */

static void adapt(void)
{
    uint32_t v = rt_vblcount;
    if (v - chk_vbl < 25) return;
    int lost = (int)(rt_stuck - chk_stuck);
    if (win_blocked) {                                      /* a blocking frame stopped the game: says nothing about the share */
        win_blocked = 0;
        heavy = 0;
        chk_vbl = v;
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
    smkx_lc_stat[10] = (uint32_t)share << 16 | (uint32_t)base_lost << 8 | (uint32_t)lost;
    chk_vbl = v;
    chk_stuck = rt_stuck;
    heavy = 0;
}

static int render_work(uint16_t *screen);

int smkx_render(uint16_t *screen)
{
    uint32_t t0 = prof_now();
    adapt();
#if !SMKX_LC_SYNC
    if ((int32_t)(t0 - next_ok) < 0) {
        if (lc_on && dsp_ok && smkx_lc_active()) smkx_lc_pump();      /* the jobs in flight go on */
        return 3;                                                       /* throttled: the picture on the screen stays */
    }
#endif
    blocked = 0;
    int r = render_work(screen);
    uint32_t spent = prof_now() - t0;
    if (blocked) {
        win_blocked = 1;
        next_ok = t0 + spent * 2;           /* the game gets as long as it was stopped before the next one; not the share's business */
        return r;
    }
    if (spent >= 192) heavy++;
    next_ok = t0 + spent * 100u / (uint32_t)share;
    return r;
}

static int render_work(uint16_t *screen)
{
    if (!(lc_on && smkx_lc_active())) make_clut();      /* a list compositor picture has its job's palette */
    if ((rt_io_start[0x100] & 0x80) && rt_nirqlog == 0) {
        if (lc_on && dsp_ok && smkx_lc_active()) smkx_lc_pump();
        return 0;
    }
    PpuInput in;
    in.vram = rt_vram;
    in.cgram = rt_cgram_start;
    in.oam = rt_oam;
    in.io = rt_io_start;
    in.bgofs = rt_bgofs_start;
    in.m7 = rt_m7_start;
    in.m7ofs = rt_m7ofs_start;
    in.abus = ppu_abus_lorom;
    in.ctx = &bus;
    in.log = rt_irqlog;
    in.nlog = rt_nirqlog;
    in.map = identity;
    in.hist = hist;
    in.oamseg = rt_oamseg;
    in.noamseg = (int)rt_noamseg;
    in.m7sink = 0;
    in.aptr = ppu_aptr_lorom;   /* HDMA tables by host pointer: a call per table entry, not per byte */
    in.subreloc = 0;
    for (int p = 0; p < 128; p++)
        if (rt_vram_dirty[p]) {
            rt_vram_dirty[p] = 0;
            ppu_fast_invalidate_page(p);
        }
    /* The list compositor (ring pages, the DSP composes only what changed, the CPU walks its list) takes the frames it
     * covers without waiting; 2: its picture is on the screen, 3: nothing new yet. The exact DSP path below reboots
     * the DSP into the other program, and the compositor is booted again when its frames come back. */
    if (lc_on && dsp_ok) {
        /* the DSP is booted into the compositor's program only for a frame it covers (a frame it does not cover would be booted
         * away again at once) */
        if (smkx_ppu_loaded() != DSP_KERNEL_LC && smkx_ppu_loaded() != DSP_KERNEL_LCW) {
            if (lc_covers(&in) && !smkx_lc_boot()) lc_on = 0;
            else if (smkx_ppu_loaded() != DSP_KERNEL_LC) goto exact;
        }
        if (lc_on) {
            int r = smkx_lc_render(&in);
#if SMKX_LC_SYNC
            /* verification: wait for the DSP here, so that the picture and the snapshot's inputs are one instant */
            while (r == 0 && smkx_lc_busy()) r = smkx_lc_render(&in);
#endif
            if (r >= 0) return r ? 2 : 3;
        }
    }
exact:
    /* The DSP composes what it can (tile modes, main-screen windows); Mode 7 rows and frames it cannot
     * take (too many pages, mixed rows) go the CPU way below. The frame handler waits for it. */
    if (dsp_ok) {
        int k = smkx_ppu_plan(&in);
        if (k >= 0 && k != smkx_ppu_loaded() && !smkx_ppu_boot(k)) {
            dsp_ok = 0;                 /* no answer from the DSP: the CPU from now on */
            k = -1;
        }
        if (k >= 0) {
            blocked = 1;
            smkx_ppu_send(screen);
            uint32_t w0 = *(volatile uint32_t *)0x4BA;
            while (!smkx_ppu_done()) {}
            smkx_ppu_stat[PPU_ST_T_WAIT] += (*(volatile uint32_t *)0x4BA - w0) * 192u;
            return 1;
        }
    }
    blocked = 1;
    ppu_fast_render(&in, chunky);
    uint32_t t0 = *(volatile uint32_t *)0x4BA;       /* _hz_200 */
    ppu_c2p8(chunky, screen);
    smkx_c2p_ticks += *(volatile uint32_t *)0x4BA - t0;
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

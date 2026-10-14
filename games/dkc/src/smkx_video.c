/* Falcon side of the video adapter (256-colour path): builds the renderer's
 * input from the runtime's state (falcon/smkx_rt.s) and composes a frame of
 * CGRAM indices in eight planes, one of two ways:
 *   - the DSP renderer (falcon/dsp/ppu.asm, src/smkx_ppu.c; from F030CT):
 *     it works on while the game runs, and smkx_render reports the frame
 *     when it is in the back buffer;
 *   - the CPU (ppu_fast.c, then ppu_c2p8) when the DSP cannot take a frame
 *     (Mode 7 rows, too many VRAM pages) or there is no DSP route.
 * Both run in the game's idle time (smkx_render_start). The Videl palette
 * is the frame's CGRAM, which the runtime loads at the VBL that shows the
 * new screen base.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "snes_ppu.h"
#include "ppu_fast.h"
#include "smkx_ppu.h"
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
extern volatile uint8_t rt_snapdue; /* a snapshot waits for the render work to end */
extern uint8_t rt_m7_route;         /* the DSP -> DMA record route is ours */

extern int smkx_lc_render(const PpuInput *in);
extern int smkx_lc_boot(void);
extern int smkx_lc_busy(void);
extern int smkx_lc_active(void);
extern void smkx_lc_pump(void);
extern void smkx_lc_set_depth(int d);
#ifndef LC_ON
#define LC_ON 1
#endif
uint32_t smkx_vid_stat[8];          /* render_start calls, forced-blank skips, lc handled, lc refused, dsp, cpu, cpu ticks, poll calls */
static int lc_on = LC_ON;           /* the list compositor draws the frames it covers (DKCX_LC=0: not) */

static uint8_t chunky[PPU_W * PPU_H];
uint32_t smkx_c2p_ticks;            /* 200 Hz ticks spent packing planes */
static PpuBus bus;
static uint8_t identity[256];
static uint32_t hist[256];          /* scratch: only Mode 7 rows count into it */
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
static void make_clut_from(const uint8_t *cgram, uint8_t inidisp)
{
    const uint8_t *lv = level[(inidisp & 0x80) ? 0 : (inidisp & 15)];
    for (int i = 0; i < 256; i++) {
        unsigned c = cgram[i * 2] | (cgram[i * 2 + 1] << 8);
        uint32_t r = lv[c & 31], g = lv[(c >> 5) & 31], b = lv[(c >> 10) & 31];
        rt_clut[i] = (r << 26) | (g << 18) | (b << 2);
    }
    rt_clut_ready = 1;
}

/* The Videl palette of a list compositor picture (src/smkx_lc.c), loaded at the next VBL. */
void smkx_set_palette(const uint8_t *cgram, uint8_t inidisp)
{
    make_clut_from(cgram, inidisp);
}

static void make_clut(void)
{
    make_clut_from(rt_cgram_start, rt_io_start[0x100]);
}

static void input(PpuInput *in)
{
    in->vram = rt_vram;
    in->cgram = rt_cgram_start;
    in->oam = rt_oam;
    in->io = rt_io_start;
    in->bgofs = rt_bgofs_start;
    in->m7 = rt_m7_start;
    in->m7ofs = rt_m7ofs_start;
    in->abus = ppu_abus_hirom;
    in->ctx = &bus;
    in->log = rt_irqlog;
    in->nlog = rt_nirqlog;
    in->map = identity;
    in->hist = hist;
    in->oamseg = rt_oamseg;
    in->noamseg = (int)rt_noamseg;
    in->m7sink = 0;
    in->aptr = ppu_aptr_hirom;
}

/* The frame in the back buffer (composed, or on its way from the DSP): its
 * CGRAM and INIDISP, for the palette that goes with it once it is shown. */
static uint8_t back_cgram[512], back_inidisp;
static int dsp_state;               /* 0 not booted, 1 ready, -1 unusable */
static int inflight;

static void keep_palette(void)
{
    for (int i = 0; i < 512; i++) back_cgram[i] = rt_cgram_start[i];
    back_inidisp = rt_io_start[0x100];
}

/* The CPU path: 0 when the frame was not composed, because it starts in
 * forced blank and no IRQ writes a register, so it is black
 * (tools/ppu_snap.c skips it the same way); its palette (black) then
 * applies at once. */
static int render_cpu(uint16_t *screen, const PpuInput *in)
{
    if ((rt_io_start[0x100] & 0x80) && rt_nirqlog == 0) {
        make_clut();
        return 0;
    }
    for (int p = 0; p < 128; p++)
        if (rt_vram_dirty[p]) {
            rt_vram_dirty[p] = 0;
            ppu_fast_invalidate_page(p);
        }
    ppu_fast_render(in, chunky);
    uint32_t t0 = *(volatile uint32_t *)0x4BA;       /* _hz_200 */
    ppu_c2p8(chunky, screen);
    smkx_c2p_ticks += *(volatile uint32_t *)0x4BA - t0;
    keep_palette();
    return 1;
}

/* Sends the frame to the DSP renderer; 0 when it cannot take it. The planner says which build of the program
 * the frame needs (tile frames, or windows and the sub screen); the DSP is booted into it when it runs another. */
static int start_dsp(uint16_t *screen, const PpuInput *in)
{
    if (!rt_m7_route || dsp_state < 0) return 0;
    int k = smkx_ppu_plan(in);
    if (k < 0) return 0;
    if (k != smkx_ppu_loaded()) {
        if (!smkx_ppu_boot(k)) {
            dsp_state = -1;                 /* no answer from the DSP: the CPU from now on */
            return 0;
        }
        dsp_state = 1;
    }
    smkx_ppu_send(screen);
    keep_palette();
    inflight = 1;
    return 1;
}

/* The list compositor (ring pages, the DSP composes only what changed, the CPU walks its list; src/smkx_lc.c) takes the
 * frames it covers: its pictures go up by themselves (the Videl points at the ring window, the palette is its job's).
 * -1: not a frame it draws (the DSP is booted into the other program by start_dsp); else what smkx_lc_render says.
 * sync: one job at a time and wait for it (snapshots: the inputs and the picture are one instant). */
static int lc_try(const PpuInput *in, int sync)
{
    if (!lc_on || !rt_m7_route || dsp_state < 0) return -1;
    if (smkx_ppu_loaded() != DSP_KERNEL_LC && smkx_ppu_loaded() != DSP_KERNEL_LCW) {
        if (!lc_covers(in)) return -1;              /* the DSP is booted into the compositor only for a frame it covers */
        if (!smkx_lc_boot()) {
            lc_on = 0;
            return -1;
        }
    }
    if (sync) {
        smkx_lc_set_depth(1);
        while (smkx_lc_busy()) smkx_lc_render(in);          /* the jobs planned ahead come first */
    }
    int r = smkx_lc_render(in);
    if (sync) {
        while (r == 0 && smkx_lc_busy()) r = smkx_lc_render(in);
        smkx_lc_set_depth(2);
    }
    return r;
}

/* ---- rendering in the game's idle time (falcon/smkx_rt.s, from F030CT) ----
 * The VBL polls: a DSP frame that has arrived is shown (rt_flip calls
 * smkx_render_present), then the next composition is asked for. The work
 * (smkx_render_start) runs in the runtime's render context while the game
 * waits at WAI, preempted by every VBL; the frame's inputs stay as they were
 * asked for until it is done. */

/* -1 nothing in flight, 0 the DSP frame is not there yet, 1 it is in the
 * back buffer. */
int smkx_render_poll(void)
{
    smkx_vid_stat[7]++;
    if (rt_snapdue) return 0;                                /* let the work end: the snapshot is next */
    if (smkx_lc_active() || smkx_lc_busy()) return -1;       /* the list compositor wants a call every frame */
    if (!inflight) return -1;
    if (!smkx_ppu_done()) return 0;
    inflight = 0;
    return 1;
}

/* 1 when screen holds a new frame to show now; 0 when the DSP composes it
 * (smkx_render_poll reports it) or nothing was composed. */
int smkx_render_start(uint16_t *screen)
{
    PpuInput in;
    input(&in);
    smkx_vid_stat[0]++;
    /* A frame that starts in forced blank and has no IRQ writes is the NMI's own blank, not a picture: the list
     * compositor keeps what it shows (the call still lets it finish its jobs). */
    if (lc_on && (smkx_lc_active() || smkx_lc_busy()) && (rt_io_start[0x100] & 0x80) && rt_nirqlog == 0) {
        smkx_vid_stat[1]++;
        smkx_lc_pump();
        return 0;
    }
    if (lc_try(&in, 0) >= 0) {
        smkx_vid_stat[2]++;
        return 0;
    }
    smkx_vid_stat[3]++;
    if (start_dsp(screen, &in)) {
        smkx_vid_stat[4]++;
        return 0;
    }
    smkx_vid_stat[5]++;
    uint32_t t0 = *(volatile uint32_t *)0x4BA;
    int r = render_cpu(screen, &in);
    smkx_vid_stat[6] += *(volatile uint32_t *)0x4BA - t0;
    return r;
}

/* The palette of the frame that is shown from the next VBL on. */
void smkx_render_present(void)
{
    make_clut_from(back_cgram, back_inidisp);
}

/* Composed now, in the VBL: snapshots, whose inputs and planes must be the
 * same frame's (rt_render_sync). 1 when screen holds it. */
int smkx_render(uint16_t *screen, int sync)
{
    (void)sync;
    if (inflight) {
        while (!smkx_ppu_done()) {}
        inflight = 0;                /* superseded: compose this frame again */
    }
    PpuInput in;
    input(&in);
    if (lc_try(&in, 1) >= 0) return smkx_lc_active() ? 2 : 0;
    if (start_dsp(screen, &in)) {
        while (!smkx_ppu_done()) {}
        inflight = 0;
        smkx_render_present();
        return 1;
    }
    int r = render_cpu(screen, &in);
    if (r) smkx_render_present();
    return r;
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

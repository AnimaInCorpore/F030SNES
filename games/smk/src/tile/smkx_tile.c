/* The list compositor's seam to SMK's runtime and its own video adapter (src/smkx_video.c). F030SMK only.
 *
 * SMK draws its Mode 7 race, and every screen this does not cover, with its own DSP renderer (four planes,
 * the adaptive 16-colour palette, falcon/dsp/ppu.asm). The tile-mode screens (title, mode, driver, cup and
 * course select) can instead go through the list compositor (smkx_lc.c): the DSP's other program
 * (falcon/dsp/ppu_tile.asm, LCB build) composes only the 16-pixel groups of a scroll ring that changed, in
 * eight planes whose pixels are CGRAM indices, and the 68030 walks its list into a ring page and points the
 * Videl at a window of it. The Videl then runs in eight planes with CGRAM as its palette; whenever a frame is
 * not covered the runtime goes back to four planes and SMK's own renderer draws it (its DSP program is booted
 * again: the DSP runs one program at a time).
 *
 * smkx_video.c calls smkx_tile_render() first in smkx_render(); everything else here is the compositor's
 * state machine: entering (a probe plan on a frame that passes a cheap gate, then boot), the eight-plane
 * palette, leaving. This file is linked into the tile world (the symbols of src/tile are renamed, see the
 * Makefile: only tools/tile/interface.txt is visible outside). Freestanding (m68k-atari-mintelf-gcc). */
#include "snes_ppu.h"
#include "lc_plan.h"
#include "smkx_ppu.h"
#include "smkx_prof.h"

extern uint8_t rt_vram[], rt_cgram_start[], rt_oam[], rt_io_start[], rt_sram[];
extern uint32_t rt_noamseg;
extern PpuOamSeg rt_oamseg[];
extern uint8_t smkx_wram[], smkx_rom[];
extern uint16_t rt_bgofs_start[], rt_m7_start[], rt_m7ofs_start[];
extern PpuWrite rt_irqlog[];
extern uint16_t rt_nirqlog;
extern uint8_t rt_plan_dirty[128];
extern volatile uint8_t rt_clut8_due;   /* smkx_clut8 goes to the Videl at the next VBL */
extern uint8_t smkx_m7_stale;           /* SMK's DSP program is not the one running (src/smkx_m7.c) */

extern int smkx_lc_boot(void);
extern int smkx_lc_render(const PpuInput *in, int sync);
extern int smkx_lc_poll(void);
extern int smkx_lc_busy(void);
extern int smkx_lc_active(void);
extern int smkx_lc_ready(void);

uint32_t smkx_clut8[256];               /* the Videl's colours: CGRAM at the frame, brightness applied */
uint32_t smkx_tile_stat[8];             /* entered, left, probes refused, boot failures, blank frames, probes, boot ticks,
                                           SMK's DSP program booted again: ticks (src/smkx_m7.c); 1/38400 s */

static PpuBus bus;
static LcPlan probe;
static int started, lc_ok = 1;          /* the bus is set up; the DSP answered its last boot */
static int active;                      /* the compositor owns the Videl and the DSP */
static int dwell;                       /* frames to leave alone after a refusal */

#ifndef SMKX_TILE_DWELL
#define SMKX_TILE_DWELL 12
#endif

/* CGRAM (BGR555) as SMK's adaptive palette would colour an entry (src/palette.c: 5 bits widened to 8,
 * brightness (b + 1) / 16), in the Videl's format, black in forced blank. smkx_set_palette: the colours of a
 * compositor picture (src/tile/smkx_lc.c: its job's CGRAM and brightness, set when the picture goes up). */
void smkx_set_palette(const uint8_t *cgram, uint8_t inidisp)
{
    uint32_t b1 = (uint32_t)(inidisp & 15) + 1;
    int blank = (inidisp & 0x80) != 0;
    for (int i = 0; i < 256; i++) {
        unsigned c = (unsigned)(cgram[i * 2] | (cgram[i * 2 + 1] << 8));
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        r = ((r << 3 | r >> 2) * b1 >> 4) & 0xFC;
        g = ((g << 3 | g >> 2) * b1 >> 4) & 0xFC;
        b = ((b << 3 | b >> 2) * b1 >> 4) & 0xFC;
        smkx_clut8[i] = blank ? 0 : (r << 24) | (g << 16) | b;
    }
    rt_clut8_due = 1;
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
    in->aptr = ppu_aptr_hirom;
    in->ctx = &bus;
    in->log = rt_irqlog;
    in->nlog = rt_nirqlog;
    in->map = 0;                        /* (the compositor draws CGRAM indices: no map) */
    in->hist = 0;
    in->oamseg = rt_oamseg;
    in->noamseg = (int)rt_noamseg;
    in->m7sink = 0;
    in->subreloc = 0;
}

/* 1 when the frame passes the cheap gate: no register written mid-frame by an IRQ (the race writes seven)
 * and BG mode 0 or 1 at the frame's start. The planner would say no to the rest as well, but only after a
 * replay of the frame. */
static int gate(void)
{
    return rt_nirqlog == 0 && (rt_io_start[0x105] & 7) <= 1;
}

/* The frame the compositor takes, or -1: the caller draws it with SMK's own renderer. Returns 4: the ring
 * window is on the Videl (a picture went up, or the shown one is the frame); 5: nothing new. sync: finish
 * this frame's picture before returning (snapshots, the last frame). */
int smkx_tile_render(int sync)
{
    if (!lc_ok) return -1;
    if (!started) {
        bus.wram = smkx_wram;
        bus.sram = rt_sram;
        bus.rom = smkx_rom;
        started = 1;
    }
    PpuInput in;
    input(&in);
    int blank = (rt_io_start[0x100] & 0x80) && rt_nirqlog == 0;
    if (!active) {
        if (dwell > 0) {
            dwell--;
            return -1;
        }
        if (blank || !gate()) return -1;
        smkx_tile_stat[5]++;
        int rc = lc_plan(&in, &probe, rt_plan_dirty);       /* (does not consume the dirty flags) */
        lc_plan_reset();
        if (rc != 0) {
            smkx_tile_stat[2]++;
            dwell = SMKX_TILE_DWELL;
            return -1;
        }
        smkx_m7_stale = 1;                                  /* the reset below ends SMK's DSP program */
        uint32_t tb = prof_now();
        int booted = smkx_lc_boot();
        smkx_tile_stat[6] += prof_now() - tb;
        if (!booted) {
            lc_ok = 0;
            smkx_tile_stat[3]++;
            return -1;
        }
        active = 1;
        smkx_tile_stat[0]++;
    }
    if (blank) {
        /* forced blank with nothing logged: the screen goes black, the ring stays */
        smkx_tile_stat[4]++;
        smkx_lc_poll();
        smkx_set_palette(rt_cgram_start, rt_io_start[0x100]);
        return 5;
    }
    int r = smkx_lc_render(&in, sync);
    if (r < 0) {
        active = 0;
        dwell = SMKX_TILE_DWELL;
        smkx_tile_stat[1]++;
        if (!smkx_lc_ready()) lc_ok = 0;                    /* the DSP stopped answering: SMK's renderer from now on */
        return -1;
    }
    if (sync) return smkx_lc_active() ? 4 : 5;
    return r ? 4 : 5;
}

/* The VBL's call while SMK's main loop is busy: finishes the DSP's jobs (lists walked, pictures shown).
 * 1: a picture went up. */
int smkx_tile_poll(void)
{
    return active ? smkx_lc_poll() : 0;
}

int smkx_tile_active(void)
{
    return active;
}

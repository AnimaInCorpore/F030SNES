/* Falcon side of the DSP tile renderer (from F030CT's smkx_ppu.c, without the SSI playback, true colour and
 * partial rendering). dsp_plan() turns the frame's registers into the DSP's stream (ppu_dsp.h) for the
 * program the frame needs: ppu.asm for the tile modes, its window build for frames with main-screen windows.
 * The pages of VRAM the frame reads go up first over the host port (rt_dsp_dirty says which the game wrote
 * since); then DMA record is armed on the screen buffer and the stream follows; the DSP composes the
 * frame and sends its rows over the SSI into the buffer.
 *   tile program: 93 slots of 256 words (DSP_TILE_SLOTS); a page stays in its slot until a frame needs the
 *     slot for another page, the least recently used one goes. Window build: the same with 89.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "smkx_ppu.h"
#include "smkx_prof.h"

extern uint8_t rt_vram[];
extern uint8_t rt_m7_dirty[DSP_VPAGES];     /* written since the DSP took the page */
extern uint8_t rt_plan_dirty[DSP_VPAGES];   /* written since the planner looked */
extern const uint8_t ppu_image[], ppuw_image[], ppulc_image[], ppulcw_image[];
extern const uint32_t ppu_image_bytes, ppuw_image_bytes, ppulc_image_bytes, ppulcw_image_bytes;
extern void rt_dsp_boot(const uint8_t *image, uint32_t bytes);
extern void rt_m7_record(void *start, uint32_t bytes);
extern int rt_m7_record_done(void);

uint32_t smkx_ppu_stat[PPU_ST_N];

#define HSR (*(volatile uint8_t *)0xFFFFA202)
#define HTX (*(volatile uint32_t *)0xFFFFA204)  /* a long write sends TXH, TXM, TXL */
#define HRX (*(volatile uint32_t *)0xFFFFA204)

static inline void put(uint32_t w)
{
    while (!(HSR & 2)) {}
    HTX = w;
}

#define SLOTS DSP_TILE_SLOTS
#define STREAM_MAX DSP_TILE_STREAM_MAX      /* ppu.asm: X:$0100-$06EF */

static int loaded = -1;                     /* the program in the DSP */
static int nslots;
static int8_t slot_of[DSP_VPAGES];          /* VRAM page -> slot, or -1 */
static int16_t page_in[SLOTS];              /* slot -> VRAM page, or -1 */
static uint32_t used_at[SLOTS];             /* frame the slot was last read */
static uint32_t stamp;
static DspPlan plan;                        /* the planned frame */
static DspPlan kept[4];                      /* renders planned ahead (smkx_ppu_keep) */

int smkx_ppu_boot(int kernel)
{
    if (kernel == DSP_KERNEL_WIN) rt_dsp_boot(ppuw_image, ppuw_image_bytes);
    else if (kernel == DSP_KERNEL_LC) rt_dsp_boot(ppulc_image, ppulc_image_bytes);
    else if (kernel == DSP_KERNEL_LCW) rt_dsp_boot(ppulcw_image, ppulcw_image_bytes);
    else rt_dsp_boot(ppu_image, ppu_image_bytes);
    loaded = -1;
    put(4);
    uint32_t spin;
    for (spin = 0; spin < 4000000 && !(HSR & 1); spin++) {}
    if (!(HSR & 1)) return 0;
    if ((HRX & 0xFFFFFF) != 0x505055) return 0;
    loaded = kernel;
    nslots = kernel == DSP_KERNEL_WIN ? DSP_WIN_SLOTS : kernel == DSP_KERNEL_LC ? DSP_LC_SLOTS : kernel == DSP_KERNEL_LCW ? DSP_LCW_SLOTS : SLOTS;
    dsp_plan_reset();
    for (int i = 0; i < DSP_VPAGES; i++) slot_of[i] = -1;
    for (int k = 0; k < SLOTS; k++) {
        page_in[k] = -1;
        used_at[k] = 0;
    }
    return 1;
}

int smkx_ppu_loaded(void)
{
    return loaded;
}

static uint32_t slot_p(int k)
{
    return loaded == DSP_KERNEL_LC || loaded == DSP_KERNEL_LCW ? dsp_lc_slot_p(k) : dsp_tile_slot_p(k);
}

static void upload_to(int pg, uint32_t addr)
{
    const uint8_t *v = rt_vram + pg * 512;
    put(0x20);                              /* PAGE */
    put((uint32_t)pg);
    put(addr);
    for (int i = 0; i < 256; i++, v += 2) put((uint32_t)v[0] | (uint32_t)v[1] << 8);
    smkx_ppu_stat[PPU_ST_UPLOADS]++;
}

/* The exact path's plan: the program the frame needs, TILE or WIN (the caller boots another one than the loaded). While the
 * list compositor's program is loaded the next boot is the caller's business, so its slot limits do not count here. */
int smkx_ppu_plan(const PpuInput *in)
{
    uint32_t t0 = prof_now();
    int lc = loaded == DSP_KERNEL_LC || loaded == DSP_KERNEL_LCW;
    plan.vdirty = rt_plan_dirty;
    plan.prefer = loaded >= 0 && !lc ? loaded : DSP_KERNEL_TILE;
    int bad = dsp_plan(in, &plan) ? -1 :
              (plan.kernel != DSP_KERNEL_TILE && plan.kernel != DSP_KERNEL_WIN) ? 1 :
              (plan.nneed > (plan.kernel == DSP_KERNEL_WIN ? DSP_WIN_SLOTS : SLOTS) ||
               plan.n - 1 > STREAM_MAX);
    smkx_ppu_stat[PPU_ST_T_PLAN] += prof_now() - t0;
    if (bad) {
        smkx_ppu_stat[PPU_ST_FALLBACKS]++;
        smkx_ppu_stat[PPU_ST_BAD_SIZE] = (uint32_t)plan.nneed << 16 | (uint32_t)plan.n;
        smkx_ppu_stat[PPU_ST_BAD_KIND] = (uint32_t)(bad < 0 ? 0xFF : plan.kernel) << 16 | (uint32_t)plan.nsprneed;
        return -1;
    }
    return plan.kernel;
}

/* The list compositor's plan (src/smkx_lc.c): the program the frame needs is its tile build (DSP_KERNEL_LC) or, for rows
 * with windows (or force_win: the other render of the frame has them), its window build (DSP_KERNEL_LCW); -1 when
 * the DSP cannot take the frame. Independent of the program loaded: the caller boots the one returned. */
int smkx_ppu_plan_lc(const PpuInput *in, int force_win)
{
    uint32_t t0 = prof_now();
    plan.vdirty = rt_plan_dirty;
    plan.prefer = DSP_KERNEL_TILE;
    plan.force_win = force_win;
    int bad = dsp_plan(in, &plan);
    plan.force_win = 0;
    int k = -1, lim = 0;
    if (!bad && plan.kernel == DSP_KERNEL_TILE) k = DSP_KERNEL_LC, lim = DSP_LC_SLOTS;
    else if (!bad && plan.kernel == DSP_KERNEL_WIN) k = DSP_KERNEL_LCW, lim = DSP_LCW_SLOTS;
    smkx_ppu_stat[PPU_ST_T_PLAN] += prof_now() - t0;
    if (k < 0 || plan.nneed > lim || plan.n - 1 > STREAM_MAX) {
        smkx_ppu_stat[PPU_ST_FALLBACKS]++;
        smkx_ppu_stat[PPU_ST_BAD_SIZE] = (uint32_t)plan.nneed << 16 | (uint32_t)plan.n;
        smkx_ppu_stat[PPU_ST_BAD_KIND] = (uint32_t)(bad ? 0xFF : plan.kernel) << 16 | (uint32_t)plan.nsprneed;
        return -1;
    }
    return k;
}

/* The list compositor plans two renders of one frame (the window's groups 0-15 and its 17th) before sending either:
 * each is kept aside and comes back with smkx_ppu_restore when the DSP is free. */
void smkx_ppu_keep(int k)
{
    kept[k] = plan;
}

void smkx_ppu_restore(int k)
{
    plan = kept[k];
}

/* The slot pages. */
static void slots(const uint8_t *want)
{
    /* Pages already in place and clean are kept; their slots are taken. */
    for (int pg = 0; pg < DSP_VPAGES; pg++) {
        if (!want[pg] || slot_of[pg] < 0) continue;
        used_at[slot_of[pg]] = stamp;
        if (rt_m7_dirty[pg]) {
            rt_m7_dirty[pg] = 0;            /* before: a write while it goes up marks it again */
            upload_to(pg, slot_p(slot_of[pg]));
        }
    }
    /* The others go into the least recently used slots. */
    for (int pg = 0; pg < DSP_VPAGES; pg++) {
        if (!want[pg] || slot_of[pg] >= 0) continue;
        int best = 0;
        for (int k = 1; k < nslots; k++)
            if (used_at[k] < used_at[best]) best = k;
        if (page_in[best] >= 0) slot_of[page_in[best]] = -1;
        page_in[best] = (int16_t)pg;
        slot_of[pg] = (int8_t)best;
        used_at[best] = stamp;
        rt_m7_dirty[pg] = 0;
        upload_to(pg, slot_p(best));
    }
}

void smkx_ppu_send(uint16_t *screen)
{
    uint32_t t1 = prof_now();
    stamp++;
    slots(plan.need);
    uint32_t t2 = prof_now();
    smkx_ppu_stat[PPU_ST_T_UPLOAD] += t2 - t1;
    rt_m7_record(screen, PPU_SCREEN_BYTES);
    put(0x10);                              /* FRAME */
    put((uint32_t)plan.n - 1);
    for (int i = 1; i < plan.n; i++) put(plan.w[i]);
    smkx_ppu_stat[PPU_ST_T_SEND] += prof_now() - t2;
    smkx_ppu_stat[PPU_ST_FRAMES]++;
}

/* The LC build: the planned frame's pages, LCMASK, and the stream, with DMA record armed on list for `words`
 * words (the list's length is known: nine words a group, END, zeros to a multiple of eight). */
void smkx_ppu_send_lc(const uint16_t *mask, uint16_t *list, uint32_t words)
{
    uint32_t t1 = prof_now();
    stamp++;
    slots(plan.need);
    uint32_t t2 = prof_now();
    smkx_ppu_stat[PPU_ST_T_UPLOAD] += t2 - t1;
    rt_m7_record(list, words * 2);
    put(DSP_CMD_LCMASK);
    for (int y = 0; y < PPU_H; y++) put(mask[y]);
    put(0x10);                              /* FRAME */
    put((uint32_t)plan.n - 1);
    for (int i = 1; i < plan.n; i++) put(plan.w[i]);
    smkx_ppu_stat[PPU_ST_T_SEND] += prof_now() - t2;
    smkx_ppu_stat[PPU_ST_FRAMES]++;
}

int smkx_ppu_done(void)
{
    return rt_m7_record_done();
}

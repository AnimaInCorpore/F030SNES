/* Falcon side of the DSP renderer.
 *
 * dsp_plan() turns the frame's registers into the DSP's stream (ppu_dsp.h)
 * for the program the frame needs: ppu.asm for the tile modes, its Mode 7
 * build for frames whose visible rows are all Mode 7. The pages of VRAM the
 * frame reads go up first. They come from the runtime's render copy of
 * VRAM (rt_vram_r), which takes the game's writes only where a frame's
 * inputs are fixed (rt_vram_sync) and marks the pages it took (rt_m7_dirty,
 * all 128): the work, spanning VBLs whose NMIs stream new sprite
 * graphics, sees the VRAM of the frame whose OAM and registers it draws.
 *   tile program: 93 slots of 256 words (DSP_TILE_SLOTS); a page stays in
 *     its slot until a frame needs the slot for another page, the least
 *     recently used one goes. Its window build (frames with main-screen
 *     windows) the same with 89 (DSP_WIN_SLOTS).
 *   Mode 7 program: pages 0-63 at P:$1300 + 256 * page, packed as
 *     (tile * 64) << 8 | texel; the sprites' pages raw in 12 slots at
 *     P:$7400-$7FFF (those below 64 a second time, after their plane copy:
 *     the page table then points to the raw one).
 * Then DMA record is armed on the screen buffer and the stream follows;
 * the DSP composes and sends the rows while the game runs on.
 * With SMKX_PPU_SSI enabled, RAWPAGE/RAWPLANE headers go over the host
 * port. Each 512-byte page goes directly from rt_vram_r by DMA playback
 * to SSI; the DSP swaps tile words or packs Mode 7. No CPU staging buffer,
 * byte-order loop or page-data host writes. Ready/done replies delimit
 * each window; no outstanding request crosses the final slot. The command
 * stream still goes over the host port. SMKX_PPU_SSI=0 and experimental
 * LC kernels retain host-port page uploads. PPU replay/page selection and
 * the runtime's stable render-VRAM copy still run on the CPU.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "smkx_ppu.h"
#include "smkx_prof.h"

extern uint8_t rt_vram_r[];               /* the renderer's copy (rt_vram_sync) */
extern uint8_t rt_m7_dirty[DSP_VPAGES];
extern uint8_t rt_plan_dirty[DSP_VPAGES];
extern const uint8_t ppu_image[], ppu7_image[], ppuw_image[];
extern const uint32_t ppu_image_bytes, ppu7_image_bytes, ppuw_image_bytes;
#if SMKX_LC
extern const uint8_t ppulc_image[], ppulcw_image[];   /* the list compositor's build and its window build (SCIVX_LC=1) */
extern const uint32_t ppulc_image_bytes, ppulcw_image_bytes;
extern const uint8_t ppulc7_image[];
extern const uint32_t ppulc7_image_bytes;
#endif
extern void rt_dsp_boot(const uint8_t *image, uint32_t bytes);
extern void rt_m7_record(void *start, uint32_t bytes);
extern int rt_m7_record_done(void);
extern void rt_ssi_play(const uint16_t *start, uint32_t bytes);
extern int rt_ssi_play_done(void);

#ifndef SMKX_PPU_SSI
#define SMKX_PPU_SSI 1
#endif

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
static uint8_t plane_in[64];                /* Mode 7 program: plane page in place */
static uint32_t stamp;
static DspPlan plan;
static DspPlan kept[4];                     /* renders planned ahead (smkx_ppu_keep: the list compositor) */

static uint32_t slot_addr(int k)
{
    if (loaded == DSP_KERNEL_M7 || loaded == DSP_KERNEL_LC7) return DSP_M7_SPRITE_P + (uint32_t)k * 256;
    if (loaded == DSP_KERNEL_LC || loaded == DSP_KERNEL_LCW) return dsp_lc_slot_p(k);
    return dsp_tile_slot_p(k);
}

int smkx_ppu_boot(int kernel)
{
    if (kernel == DSP_KERNEL_M7) rt_dsp_boot(ppu7_image, ppu7_image_bytes);
    else if (kernel == DSP_KERNEL_WIN) rt_dsp_boot(ppuw_image, ppuw_image_bytes);
#if SMKX_LC
    else if (kernel == DSP_KERNEL_LC) rt_dsp_boot(ppulc_image, ppulc_image_bytes);
    else if (kernel == DSP_KERNEL_LCW) rt_dsp_boot(ppulcw_image, ppulcw_image_bytes);
    else if (kernel == DSP_KERNEL_LC7) rt_dsp_boot(ppulc7_image, ppulc7_image_bytes);
#endif
    else rt_dsp_boot(ppu_image, ppu_image_bytes);
    loaded = -1;
    put(4);
    uint32_t spin;
    for (spin = 0; spin < 4000000 && !(HSR & 1); spin++) {}
    if (!(HSR & 1)) return 0;
    if ((HRX & 0xFFFFFF) != 0x505055) return 0;
    loaded = kernel;
    nslots = kernel == DSP_KERNEL_M7 || kernel == DSP_KERNEL_LC7 ? DSP_M7_SPRITE_PAGES : kernel == DSP_KERNEL_WIN ? DSP_WIN_SLOTS :
             kernel == DSP_KERNEL_LC ? DSP_LC_SLOTS : kernel == DSP_KERNEL_LCW ? DSP_LCW_SLOTS : SLOTS;
    dsp_plan_reset();                       /* the Mode 7 kernel cleared flags meanwhile */
    for (int i = 0; i < DSP_VPAGES; i++) slot_of[i] = -1;
    for (int k = 0; k < SLOTS; k++) {
        page_in[k] = -1;
        used_at[k] = 0;
    }
    for (int i = 0; i < 64; i++) plane_in[i] = 0;
    return 1;
}

int smkx_ppu_loaded(void)
{
    return loaded;
}

/* A page into P memory at addr; packed: the Mode 7 plane's format. */
static void upload_to(int pg, uint32_t addr, int packed)
{
    const uint8_t *v = rt_vram_r + pg * 512;
    if (SMKX_PPU_SSI && loaded != DSP_KERNEL_LC && loaded != DSP_KERNEL_LCW && loaded != DSP_KERNEL_LC7) {
        /* Headers/control only on the CPU. VRAM is already a stable render
         * buffer: DMA reads its raw bytes directly; DSP handles conversion. */
        while (!rt_ssi_play_done()) {}
        put(packed ? DSP_CMD_RAWPLANE : DSP_CMD_RAWPAGE);
        put((uint32_t)pg);
        put(addr);
        while (!(HSR & 1)) {}
        (void)HRX;
        rt_ssi_play((const uint16_t *)(const void *)v, 512);
        while (!rt_ssi_play_done()) {}
        while (!(HSR & 1)) {}
        (void)HRX;                         /* DSP consumed the last word, receiver back on host port */
        smkx_ppu_stat[PPU_ST_UPLOADS]++;
        smkx_ppu_stat[PPU_ST_SSI]++;
        return;
    }
    put(0x20);
    put((uint32_t)pg);
    put(addr);
    if (packed)
        for (int i = 0; i < 256; i++, v += 2) put((uint32_t)v[0] << 14 | v[1]);
    else
        for (int i = 0; i < 256; i++, v += 2) put((uint32_t)v[0] | (uint32_t)v[1] << 8);
    smkx_ppu_stat[PPU_ST_UPLOADS]++;
}

int smkx_ppu_plan(const PpuInput *in)
{
    uint32_t t0 = prof_now();
    plan.vdirty = rt_plan_dirty;
    plan.prefer = loaded >= 0 && loaded != DSP_KERNEL_LC && loaded != DSP_KERNEL_LCW && loaded != DSP_KERNEL_LC7 ? loaded : DSP_KERNEL_TILE;
    int bad = dsp_plan(in, &plan) ? -1 :
              (plan.kernel == DSP_KERNEL_M7 ? plan.nsprneed > DSP_M7_SPRITE_PAGES ||
                                              plan.n - 1 > DSP_M7_STREAM_MAX
                                            : plan.nneed > (plan.kernel == DSP_KERNEL_WIN ? DSP_WIN_SLOTS : SLOTS) ||
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
 * with windows (or force_win: the other render of the frame has them), its window build (DSP_KERNEL_LCW); -1 when the DSP
 * cannot take the frame. Independent of the program loaded: the caller boots the one returned. */
int smkx_ppu_plan_lc(const PpuInput *in, int force_win)
{
    return smkx_ppu_plan_lc_kind(in, force_win, 0);
}

int smkx_ppu_plan_lc_kind(const PpuInput *in, int force_win, int kind)
{
    uint32_t t0 = prof_now();
    plan.vdirty = rt_plan_dirty;
    plan.prefer = DSP_KERNEL_TILE;
    plan.force_win = force_win;
    plan.lc_kind = kind;
    int bad = dsp_plan(in, &plan);
    plan.force_win = 0;
    plan.lc_kind = 0;
    int k = -1, lim = 0;
    if (!bad && plan.kernel == DSP_KERNEL_TILE) k = DSP_KERNEL_LC, lim = DSP_LC_SLOTS;
    else if (!bad && plan.kernel == DSP_KERNEL_WIN) k = DSP_KERNEL_LCW, lim = DSP_LCW_SLOTS;
    else if (!bad && plan.kernel == DSP_KERNEL_M7) k = DSP_KERNEL_LC7, lim = DSP_M7_SPRITE_PAGES;
    smkx_ppu_stat[PPU_ST_T_PLAN] += prof_now() - t0;
    if (k < 0 || (k == DSP_KERNEL_LC7 ? plan.nsprneed : plan.nneed) > lim ||
        plan.n - 1 > (k == DSP_KERNEL_LC7 ? DSP_M7_STREAM_MAX : DSP_LC_STREAM_MAX)) {
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

int smkx_ppu_stale(void)
{
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        if (plan.need[pg] && rt_plan_dirty[pg]) return 1;
    return 0;
}

/* The slot pages: the tile program's need, the Mode 7 program's sprites. */
static void slots(const uint8_t *want, const uint8_t *fresh)
{
    /* Pages already in place and clean are kept; their slots are taken. */
    for (int pg = 0; pg < DSP_VPAGES; pg++) {
        if (!want[pg] || slot_of[pg] < 0) continue;
        used_at[slot_of[pg]] = stamp;
        if (rt_m7_dirty[pg] || fresh[pg]) {
            rt_m7_dirty[pg] = 0;            /* before: a write while it goes up marks it again */
            upload_to(pg, slot_addr(slot_of[pg]), 0);
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
        upload_to(pg, slot_addr(best), 0);
    }
}

/* Pages first, then command stream; output DMA targets the framebuffer. */
void smkx_ppu_send(uint16_t *screen)
{
    uint32_t t1 = prof_now();
    static uint8_t fresh[DSP_VPAGES];       /* plane pages sent this frame */
    stamp++;
    for (int pg = 0; pg < DSP_VPAGES; pg++) fresh[pg] = 0;
    if (loaded == DSP_KERNEL_M7) {
        /* the Mode 7 plane, then the sprites' pages (a plane page sent now
         * has its raw copy sent again: the page table points to the last) */
        for (int pg = 0; pg < 64; pg++) {
            if (plane_in[pg] && !rt_m7_dirty[pg]) continue;
            rt_m7_dirty[pg] = 0;
            upload_to(pg, DSP_M7_PLANE_P + (uint32_t)pg * 256, 1);
            plane_in[pg] = 1;
            fresh[pg] = 1;
        }
        slots(plan.sprneed, fresh);
    } else {
        slots(plan.need, fresh);
    }
    uint32_t t2 = prof_now();
    smkx_ppu_stat[PPU_ST_T_UPLOAD] += t2 - t1;
    rt_m7_record(screen, PPU_SCREEN_BYTES);
    put(0x10);
    put((uint32_t)plan.n - 1);
    for (int i = 1; i < plan.n; i++) put(plan.w[i]);
    smkx_ppu_stat[PPU_ST_T_SEND] += prof_now() - t2;
    smkx_ppu_stat[PPU_ST_FRAMES]++;
}

/* The LC build (DSP_KERNEL_LC): the planned frame's pages, LCMASK, and the stream, with DMA record armed on list for
 * `words` words (its length is known: eight words a group). The pages go over the host port (the SSI window is the
 * whole-frame renderers'). */
void smkx_ppu_send_lc(const uint16_t *mask, uint16_t *list, uint32_t words)
{
    static uint8_t nofresh[DSP_VPAGES];
    uint32_t t1 = prof_now();
    stamp++;
    for (int pg = 0; pg < DSP_VPAGES; pg++) nofresh[pg] = 0;
    if (loaded == DSP_KERNEL_LC7) {
        for (int pg = 0; pg < 64; pg++) {
            if (plane_in[pg] && !rt_m7_dirty[pg]) continue;
            rt_m7_dirty[pg] = 0;
            upload_to(pg, DSP_M7_PLANE_P + (uint32_t)pg * 256, 1);
            plane_in[pg] = 1;
            nofresh[pg] = 1;
        }
        slots(plan.sprneed, nofresh);
    } else slots(plan.need, nofresh);
    uint32_t t2 = prof_now();
    smkx_ppu_stat[PPU_ST_T_UPLOAD] += t2 - t1;
    rt_m7_record(list, words * 2);
    put(DSP_CMD_LCMASK);
    for (int y = 0; y < PPU_H; y++) put(mask[y]);
    put(DSP_CMD_FRAME);
    put((uint32_t)plan.n - 1);
    for (int i = 1; i < plan.n; i++) put(plan.w[i]);
    smkx_ppu_stat[PPU_ST_T_SEND] += prof_now() - t2;
    smkx_ppu_stat[PPU_ST_FRAMES]++;
}

int smkx_ppu_start(const PpuInput *in, uint16_t *screen)
{
    int k = smkx_ppu_plan(in);
    if (k < 0) return 0;
    if (k != loaded && !smkx_ppu_boot(k)) return 0;
    smkx_ppu_send(screen);
    return 1;
}

void smkx_ppu_palette(uint8_t *cgram)
{
    if (plan.mathpal) {
        dsp_math_palette(&plan, cgram, cgram);
        smkx_ppu_stat[PPU_ST_MATHPAL]++;
    }
    if (plan.subrows) {
        dsp_sub_palette(&plan, cgram, cgram);
        smkx_ppu_stat[PPU_ST_SUB]++;
    }
}

int smkx_ppu_done(void)
{
    return rt_m7_record_done();
}

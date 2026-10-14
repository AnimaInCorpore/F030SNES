/* Falcon side of the true-colour DSP renderer (falcon/dsp/tc.asm, FF6X_TC=1):
 * the smkx_ppu.h interface, as src/smkx_ppu.c gives it for the planar
 * program, with RGB565 rows recorded into a 16-bit screen.
 *
 * dsp_plan() with DspPlan.tc names the program: tc.bin (BG modes 0-6) or
 * its TC7 build (frames whose visible rows are all Mode 7). The pages the
 * frame reads go up first, each in the conversions it is read in
 * (DspPlan.fmt; TPAGE: page, format, slot address, the 256 VRAM words; the
 * DSP converts, ppu_tc.h). They come from the runtime's render copy of VRAM
 * (rt_vram_r); a page the game wrote since (rt_m7_dirty) goes up again in
 * every conversion the DSP holds of it.
 *   tc.bin: 74 slots (tc_slot_p); a page in one conversion stays in its
 *     slot until a frame needs the slot, the least recently used goes.
 *   TC7: the Mode 7 plane (TC_M7) in slots 0-63, plane page p in slot p;
 *     the sprites' pages (TC_F4) in slots 64-73, least recently used.
 * Then DMA record is armed on the screen and the stream follows. A frame
 * that sends pages goes by DMA playback into the DSP's SSI receive when the
 * route answers (SMKX_PPU_SSI, as in smkx_ppu.c: a protocol word two slots,
 * a page word one), else over the host port.
 * A frame goes in parts (plan_rows): whole; in bands of rows (BAND) when
 * its pages do not fit at once; in runs of Mode 7 and tile rows, each for
 * its program, when it mixes them (dsp_tc_runs). Parts go over the host
 * port, each into its rows of the screen; the 68030 waits while the DSP
 * draws one before the next one's pages (or program).
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "smkx_ppu.h"
#include "ppu_tc.h"
#include "smkx_prof.h"
#include "ppu_rows.h"

extern uint8_t rt_vram_r[];               /* the renderer's copy (rt_vram_sync) */
extern uint8_t rt_m7_dirty[DSP_VPAGES];
extern uint8_t rt_plan_dirty[DSP_VPAGES];
extern const uint8_t tc_image[], tc7_image[];
extern const uint32_t tc_image_bytes, tc7_image_bytes;
extern void rt_dsp_boot(const uint8_t *image, uint32_t bytes);
extern void rt_m7_record(void *start, uint32_t bytes);
extern int rt_m7_record_done(void);
extern void rt_ssi_play(const uint16_t *start, uint32_t bytes);
extern int rt_ssi_play_done(void);
extern uint16_t *volatile rt_back, *volatile rt_screen;   /* the two screen buffers */
extern volatile int smkx_cpu_bound;                        /* smkx_video.c: the DSP mostly waits for the CPU */

#ifndef SMKX_PPU_SSI
#define SMKX_PPU_SSI 1
#endif
#ifndef SMKX_PPU_PRE
#define SMKX_PPU_PRE 1                       /* the SSI window is built while the DSP draws */
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

#define PLANE 64                            /* TC7: the plane's slots */
#define NKEY (DSP_VPAGES * 4)               /* page * 4 + conversion (F2, F4, F8, RAW) */
#define SSI_SLOTS (TC_SLOTS * (8 + 256) + 2 * (TC_STREAM_MAX + 3) + 16)

static int ssi_cap, ssi;
static int pre_ok;                          /* sbuf holds the picture planned last (pre_build) */
static void take_dirty(void);
static void upload(int pg, int fmt, uint32_t addr);
static void slots(int first);
/* Two windows: the one playing is not written (the next picture's is built while it plays) */
static uint16_t sbufs[2][SSI_SLOTS];
static uint16_t *sbuf = sbufs[0];
static int sbuf_cur;

static inline void ssi_played(void)          /* rt_ssi_play took sbuf: the other one is free */
{
    sbuf_cur ^= 1;
    sbuf = sbufs[sbuf_cur];
}
static uint32_t nslot;

static void w24(uint32_t w)
{
    if (ssi) {
        sbuf[nslot++] = (uint16_t)(w >> 8);
        sbuf[nslot++] = (uint16_t)(w & 0xFF);
    } else {
        put(w);
    }
}

static int loaded = -1;
static int16_t slot_of[NKEY];               /* page and conversion -> slot, or -1 */
static int16_t key_in[TC_SLOTS];            /* slot -> key, or -1 */
static uint32_t used_at[TC_SLOTS];
static uint8_t stale[NKEY];                 /* in its slot, but the page was written */
static uint8_t plane_in[PLANE];             /* TC7: plane page in place and clean */
static uint32_t stamp;
static DspPlan plan;
/* The frame's parts, in row order: one (the whole frame), bands of rows
 * whose pages would not fit at once, runs of Mode 7 and tile rows (each
 * banded as it needs): per part its program, rows, pages and stream. */
#define PARTS_MAX 8
#define BANDS_MAX 4
typedef struct {
    int kernel, band0, band1, n;
    uint8_t fmt[DSP_VPAGES];
    uint32_t w[TC_STREAM_MAX + 1];
} Part;
static Part part[PARTS_MAX];
static int nparts;
#if SMKX_PARTIAL_CHECK == 2
static Part chk_part[PARTS_MAX];             /* the whole picture, planned for the check */
static int chk_np, chk_ok, chk_fresh, chk_drawn, chk_pnp;
static uint16_t chk_screen[PPU_W * PPU_H];
#endif
int smkx_tc_runs;                           /* the last plan's runs of Mode 7 and tile
                                               rows (0: one kind) */

/* Partial rendering (ppu_rows.h) */
#ifndef SMKX_PARTIAL
#define SMKX_PARTIAL 2
#endif
static PartialState ps = {.mode = SMKX_PARTIAL};
int smkx_ppu_sync;

void smkx_ppu_drawn_full(uint16_t *screen)
{
    pr_drawn_full(&ps, screen);
}

int smkx_ppu_skipped(void)
{
    return ps.skipped;
}

uint32_t smkx_ppu_pic(void)
{
    return ps.pic_id;
}

/* The buffer holding the newest picture (drawn, or on its way): what will be on screen */
uint16_t *smkx_ppu_newest(void)
{
    return (uint16_t *)pr_newest(&ps, rt_back, rt_screen);
}


int smkx_ppu_boot(int kernel)
{
    if (kernel == DSP_KERNEL_TC7) rt_dsp_boot(tc7_image, tc7_image_bytes);
    else rt_dsp_boot(tc_image, tc_image_bytes);
    loaded = -1;
    put(4);
    uint32_t spin;
    for (spin = 0; spin < 4000000 && !(HSR & 1); spin++) {}
    if (!(HSR & 1)) return 0;
    if ((HRX & 0xFFFFFF) != 0x544353) return 0;     /* "TCS" */
    ssi = 0;
    ssi_cap = SMKX_PPU_SSI != 0;
    pre_ok = 0;                             /* (the reload reset the slot bookkeeping) */
    loaded = kernel;
    dsp_plan_reset();
    for (int i = 0; i < NKEY; i++) slot_of[i] = -1, stale[i] = 0;
    for (int k = 0; k < TC_SLOTS; k++) key_in[k] = -1, used_at[k] = 0;
    for (int i = 0; i < PLANE; i++) plane_in[i] = 0;
    return 1;
}

int smkx_ppu_loaded(void)
{
    return loaded;
}

/* Slots the frame needs beyond what the program keeps fixed. */
static int slots_needed(void)
{
    int n = 0;
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        for (int b = 0; b < 4; b++) n += (plan.fmt[pg] >> b) & 1;
    return n;
}

/* Rows [r0, r1) as 1-4 bands that fit (appended to part[]): 0, or -1
 * with plan.fail set. A band plan may be the whole frame (0, 0). */
static int plan_rows(const PpuInput *in, int r0, int r1)
{
    int first = nparts;
    for (int nb = 1; nb <= BANDS_MAX; nb++) {
        int ok = 1;
        nparts = first;
        for (int b = 0; b < nb && ok; b++) {
            if (nparts == PARTS_MAX) return plan.fail = 10, -1;
            int b0 = r0 + b * (r1 - r0) / nb, b1 = r0 + (b + 1) * (r1 - r0) / nb;
            plan.band0 = r0 == 0 && r1 == PPU_H && nb == 1 ? 0 : b0;
            plan.band1 = r0 == 0 && r1 == PPU_H && nb == 1 ? 0 : b1;
            if (dsp_plan(in, &plan)) {
                if (plan.fail != 3) return -1;  /* offset-per-tile differs: bands may not */
                ok = 0;
                break;
            }
            if (plan.n - 1 > TC_STREAM_MAX) {  /* a band's stream has only its rows */
                plan.fail = 9;
                ok = 0;
                break;
            }
            int n = slots_needed();
            if (n > (plan.kernel == DSP_KERNEL_TC7 ? TC_SLOTS - PLANE : TC_SLOTS_TILE)) {
                ok = 0;
                break;
            }
            Part *q = &part[nparts++];
            q->kernel = plan.kernel;
            q->band0 = b0;
            q->band1 = b1;
            q->n = plan.n;
            int k = 0;
            for (; k + 4 <= plan.n; k += 4) {
                q->w[k] = plan.w[k];
                q->w[k + 1] = plan.w[k + 1];
                q->w[k + 2] = plan.w[k + 2];
                q->w[k + 3] = plan.w[k + 3];
            }
            for (; k < plan.n; k++) q->w[k] = plan.w[k];
            for (int pg = 0; pg < DSP_VPAGES; pg++) q->fmt[pg] = plan.fmt[pg];
        }
        if (ok) return 0;
    }
    if (!plan.fail) plan.fail = 11;             /* not even 4 bands fit */
    return -1;
}

/* Rows [r0, r1) planned: whole, or as runs of one kind (Mode 7, tile) each for its program */
static int plan_run(const PpuInput *in, int r0, int r1)
{
    int bad = plan_rows(in, r0, r1);
    if (bad && plan.fail == 6) {
        int run[3][2], nr = dsp_tc_runs(plan.rowkind, run, 3);
        if (nr > smkx_tc_runs) smkx_tc_runs = nr;
        bad = nr ? 0 : -1;
        for (int k = 0; k < nr && !bad; k++) {
            int a = run[k][0] > r0 ? run[k][0] : r0, b = run[k][1] < r1 ? run[k][1] : r1;
            if (a < b) bad = plan_rows(in, a, b);
        }
        if (!bad) smkx_ppu_stat[PPU_ST_BANDED]++;
    }
    return bad;
}

/* The single part's SSI window built ahead: the plan's pages and stream go into sbuf while the DSP still
 * draws the last picture, so that after its frame is done only the handshake and the DMA start are left
 * (the DSP idles for the DMA, not for the CPU's copying). The slot bookkeeping is as if it was sent: a
 * picture that is not sent (the VRAM changed, another program, the CPU drew it) puts it back. */
typedef struct {
    int16_t slot_of[NKEY], key_in[TC_SLOTS];
    uint32_t used_at[TC_SLOTS], stamp;
    uint8_t stale[NKEY], plane_in[PLANE], dirty[DSP_VPAGES];   /* (dirty: take_dirty consumes it) */
} SlotState;
static SlotState pre_save;
static uint32_t pre_nslot;

static void slot_save(SlotState *z)
{
    for (int i = 0; i < NKEY; i++) z->slot_of[i] = slot_of[i], z->stale[i] = stale[i];
    for (int k = 0; k < TC_SLOTS; k++) z->key_in[k] = key_in[k], z->used_at[k] = used_at[k];
    for (int i = 0; i < PLANE; i++) z->plane_in[i] = plane_in[i];
    for (int i = 0; i < DSP_VPAGES; i++) z->dirty[i] = rt_m7_dirty[i];
    z->stamp = stamp;
}

static void slot_restore(const SlotState *z)
{
    for (int i = 0; i < NKEY; i++) slot_of[i] = z->slot_of[i], stale[i] = z->stale[i];
    for (int k = 0; k < TC_SLOTS; k++) key_in[k] = z->key_in[k], used_at[k] = z->used_at[k];
    for (int i = 0; i < PLANE; i++) plane_in[i] = z->plane_in[i];
    for (int i = 0; i < DSP_VPAGES; i++) if (z->dirty[i]) rt_m7_dirty[i] = 1;   /* (a page written meanwhile stays marked) */
    stamp = z->stamp;
}

static void pre_discard(void)
{
    if (!pre_ok) return;
    pre_ok = 0;
    slot_restore(&pre_save);
    smkx_ppu_stat[PPU_ST_PRE_LOST]++;
}

static void pre_build(void)
{
    Part *q = &part[0];
    uint32_t t0 = prof_now();
    slot_save(&pre_save);
    ssi = 1;
    nslot = 0;
    stamp++;
    take_dirty();
    for (int pg = 0; pg < DSP_VPAGES; pg++) plan.fmt[pg] = q->fmt[pg];
    if (loaded == DSP_KERNEL_TC7) {
        for (int pg = 0; pg < PLANE; pg++) {
            if (plane_in[pg]) continue;
            upload(pg, TC_M7, tc_slot_p(pg));
            plane_in[pg] = 1;
        }
        slots(PLANE);
    } else {
        slots(0);
    }
    if (q->band0 != 0 || q->band1 != PPU_H) {
        w24(0x11);                          /* BAND */
        w24((uint32_t)q->band0);
        w24((uint32_t)q->band1);
    }
    w24(0x10);                              /* FRAME */
    w24((uint32_t)q->n - 1);
    for (int i = 1; i < q->n; i++) w24(q->w[i]);
    while ((nslot + 2) & 7) w24(0);         /* whole DMA frames; 0 is no command */
    w24(9);                                 /* SSIOFF: the last slots */
    ssi = 0;
    pre_nslot = nslot;
    pre_ok = 1;
    smkx_ppu_stat[PPU_ST_T_PRE] += prof_now() - t0;
}

int smkx_ppu_plan(const PpuInput *in)
{
    uint32_t t0 = prof_now();
    pre_discard();                          /* (a picture planned but not sent) */
    plan.vdirty = rt_plan_dirty;
    plan.tc = 1;
    plan.prefer = loaded >= 0 ? loaded : DSP_KERNEL_TC;
    nparts = 0;
    smkx_tc_runs = 0;
    /* both buffer pointers with interrupts off: a flip must not come between */
    uint16_t *ba, *bb;
    {
        uint32_t sr;
        __asm__ volatile("move.w %%sr,%0\n\tor.w #0x700,%%sr" : "=d"(sr) : : "memory", "cc");
        ba = rt_back;
        bb = rt_screen;
        __asm__ volatile("move.w %0,%%sr" : : "d"((uint16_t)sr) : "memory", "cc");
    }
    ps.mode = smkx_cpu_bound && !SMKX_PARTIAL_CHECK ? 0 : SMKX_PARTIAL;   /* (no change detection while the CPU is the limit) */
    pr_begin(&ps, in, rt_plan_dirty, ba, bb, smkx_ppu_sync);
    plan.use_events = ps.mode != 0 && dsp_rows_events_ok();   /* one replay of the HDMA tables: they change under us */
#if SMKX_PARTIAL_CHECK == 2
    {
        int bad0 = plan_run(in, 0, PPU_H);
        chk_ok = !bad0;
        if (chk_ok) {
            chk_np = nparts;
            for (int k = 0; k < nparts; k++) chk_part[k] = part[k];
        }
        nparts = 0;
        smkx_tc_runs = 0;
    }
#endif
    if (ps.changed) smkx_ppu_stat[PPU_ST_CHANGED]++;
    if (ps.skipped) {
        smkx_ppu_stat[PPU_ST_SKIPPED]++;
        smkx_ppu_stat[PPU_ST_T_PLAN] += prof_now() - t0;
        return loaded >= 0 ? loaded : DSP_KERNEL_TC;
    }
    int run[32][2], nr = pr_runs(ps.need, 8, 3, run);
    int bad = 0;
    for (int k = 0; k < nr && !bad; k++) bad = plan_run(in, run[k][0], run[k][1]);
    if (!bad && (smkx_tc_runs || nparts > 1)) smkx_ppu_stat[PPU_ST_BANDED]++;
    smkx_ppu_stat[PPU_ST_T_PLAN] += prof_now() - t0;
    if (bad) {
        nparts = 0;
        smkx_ppu_stat[PPU_ST_FALLBACKS]++;
        smkx_ppu_stat[PPU_ST_BAD_SIZE] = (uint32_t)slots_needed() << 16 | (uint32_t)plan.n;
        smkx_ppu_stat[PPU_ST_BAD_KIND] = 0xFFu << 16 | (uint32_t)plan.fail;
        return -1;
    }
    if (SMKX_PPU_PRE && ssi_cap && nparts == 1 && part[0].kernel == loaded) pre_build();
    return part[0].kernel;
}

int smkx_ppu_stale(void)
{
    for (int k = 0; k < nparts; k++)
        for (int pg = 0; pg < DSP_VPAGES; pg++)
            if (part[k].fmt[pg] && rt_plan_dirty[pg]) return 1;
    return 0;
}

/* TPAGE: a page in one conversion into P memory at addr. */
static void upload(int pg, int fmt, uint32_t addr)
{
    const uint8_t *v = rt_vram_r + pg * 512;
    w24(0x22);
    w24((uint32_t)pg);
    w24((uint32_t)fmt);
    w24(addr);
    if (ssi) {
        uint16_t *o = sbuf + nslot;
        for (int i = 0; i < 256; i++, v += 2) *o++ = (uint16_t)(v[0] | v[1] << 8);
        nslot = (uint32_t)(o - sbuf);
    } else {
        for (int i = 0; i < 256; i++, v += 2) put((uint32_t)v[0] | (uint32_t)v[1] << 8);
    }
    smkx_ppu_stat[PPU_ST_UPLOADS]++;
}

/* Pages written since they went up: every conversion the DSP holds of them
 * is stale (the dirty mark goes before the upload: a write meanwhile marks
 * it again). */
static void take_dirty(void)
{
    for (int pg = 0; pg < DSP_VPAGES; pg++) {
        if (!rt_m7_dirty[pg]) continue;
        rt_m7_dirty[pg] = 0;
        for (int b = 0; b < 4; b++)
            if (slot_of[pg * 4 + b] >= 0) stale[pg * 4 + b] = 1;
        if (pg < PLANE) plane_in[pg] = 0;
    }
}

static int pages_due(void)
{
    if (loaded == DSP_KERNEL_TC7)
        for (int pg = 0; pg < PLANE; pg++)
            if (!plane_in[pg] || rt_m7_dirty[pg]) return 1;
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        for (int b = 0; b < 4; b++) {
            if (!((plan.fmt[pg] >> b) & 1)) continue;
            int key = pg * 4 + b;
            if (slot_of[key] < 0 || stale[key] || rt_m7_dirty[pg]) return 1;
        }
    return 0;
}

/* The slot pages: kept where they are and clean, else into the least
 * recently used slot from first on. */
static void slots(int first)
{
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        for (int b = 0; b < 4; b++) {
            int key = pg * 4 + b;
            if (!((plan.fmt[pg] >> b) & 1) || slot_of[key] < 0) continue;
            used_at[slot_of[key]] = stamp;
            if (stale[key]) {
                stale[key] = 0;
                upload(pg, 1 << b, tc_slot_p(slot_of[key]));
            }
        }
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        for (int b = 0; b < 4; b++) {
            int key = pg * 4 + b;
            if (!((plan.fmt[pg] >> b) & 1) || slot_of[key] >= 0) continue;
            int best = first;
            for (int k = first + 1; k < (loaded == DSP_KERNEL_TC7 ? TC_SLOTS : TC_SLOTS_TILE); k++)
                if (used_at[k] < used_at[best]) best = k;
            if (key_in[best] >= 0) slot_of[key_in[best]] = -1, stale[key_in[best]] = 0;
            key_in[best] = (int16_t)key;
            slot_of[key] = (int16_t)best;
            used_at[best] = stamp;
            upload(pg, 1 << b, tc_slot_p(best));
        }
}

/* Parts over the host port, each part's pages, BAND and its stream into its own rows of the screen; a
 * part for the other program boots it (its pages go up again); before the next part the last one is in
 * the screen. 0 when the DSP did not boot. */
static int send_multi(Part *pp, int np, uint16_t *screen)
{
    take_dirty();
    for (int k = 0; k < np; k++) {
        Part *q = &pp[k];
        if (loaded != q->kernel) {
            if (!smkx_ppu_boot(q->kernel)) return 0;
            take_dirty();
        }
        stamp++;
        for (int pg = 0; pg < DSP_VPAGES; pg++) plan.fmt[pg] = q->fmt[pg];
        if (loaded == DSP_KERNEL_TC7) {
            for (int pg = 0; pg < PLANE; pg++) {
                if (plane_in[pg]) continue;
                upload(pg, TC_M7, tc_slot_p(pg));
                plane_in[pg] = 1;
            }
            slots(PLANE);
        } else {
            slots(0);
        }
        rt_m7_record(screen + q->band0 * PPU_W, (uint32_t)(q->band1 - q->band0) * PPU_W * 2);
        put(0x11);                          /* BAND */
        put((uint32_t)q->band0);
        put((uint32_t)q->band1);
        put(0x10);
        put((uint32_t)q->n - 1);
        for (int i = 1; i < q->n; i++) put(q->w[i]);
        if (k + 1 < np)
            while (!rt_m7_record_done()) {}
    }
    return 1;
}

#if SMKX_PARTIAL_CHECK == 2
/* SMKX_PARTIAL_CHECK=2: every picture is also drawn whole by the DSP into chk_screen (before the partial one
 * is sent), and when the partial picture is complete the buffer is compared with it. */
extern void rt_cprint(const char *text);
extern void rt_cprint_hex(uint32_t value);
extern void rt_dump(const char *name, const void *buf, long len);
int smkx_ppu_check_full(void)
{
    if (!chk_ok) return 0;
    ssi = 0;
    if (!send_multi(chk_part, chk_np, chk_screen)) return 0;
    while (!rt_m7_record_done()) {}
    return 1;
}

const uint16_t *smkx_ppu_check_screen(void)
{
    return chk_screen;
}

void smkx_ppu_check_compare(const uint16_t *buf)
{
    int bad = 0, first = -1;
    for (int r = 0; r < PPU_H; r++) {
        int d = 0;
        for (int x = 0; x < PPU_W; x++) d |= buf[r * PPU_W + x] != chk_screen[r * PPU_W + x];
        if (d) { bad++; if (first < 0) first = r; }
    }
    smkx_ppu_stat[PPU_ST_CHK]++;
    if (bad) {
        smkx_ppu_stat[PPU_ST_CHK_BAD]++;
        rt_cprint("SMKX PARTIAL CHECK: rows differing ");
        rt_cprint_hex((uint32_t)bad);
        rt_cprint(" first row ");
        rt_cprint_hex((uint32_t)first);
        rt_cprint(" pic ");
        rt_cprint_hex(ps.pic_id);
        rt_cprint(" rows drawn ");
        rt_cprint_hex((uint32_t)chk_drawn);
        rt_cprint(" parts ");
        rt_cprint_hex((uint32_t)chk_pnp);
        rt_cprint(" fresh ");
        rt_cprint_hex((uint32_t)chk_fresh);
        rt_cprint("\n");
        static int dumped;
        if (dumped < 6) {                   /* the first pictures, for a look on the host */
            static char na[] = "CA0.BIN", nb[] = "CB0.BIN";
            na[2] = nb[2] = (char)('0' + dumped);
            dumped++;
            rt_dump(na, buf, PPU_W * PPU_H * 2);
            rt_dump(nb, chk_screen, PPU_W * PPU_H * 2);
        }
    }
}
#endif

extern uint32_t smkx_done_at;
extern int smkx_plan_early;

static void send_real(uint16_t *screen)
{
    uint32_t t1 = prof_now();
    if (smkx_done_at) {                     /* how long the last frame waited for this send */
        smkx_ppu_stat[PPU_ST_T_GAP] += t1 - (smkx_done_at & ~1u);
        if (smkx_plan_early) smkx_ppu_stat[PPU_ST_T_GAPE] += t1 - (smkx_done_at & ~1u);
        smkx_done_at = 0;
    }
    ssi = 0;
    pr_commit(&ps, screen);
    if (nparts == 0) return;                /* nothing to draw: the buffer is current */
    {
        int rowsdrawn = 0;
        for (int k = 0; k < nparts; k++) rowsdrawn += part[k].band1 - part[k].band0;
        smkx_ppu_stat[PPU_ST_ROWS] += (uint32_t)rowsdrawn;
        if (rowsdrawn < PPU_H) smkx_ppu_stat[PPU_ST_PARTIAL]++;
    }
    if (nparts > 1) {
        send_multi(part, nparts, screen);
        smkx_ppu_stat[PPU_ST_T_SEND] += prof_now() - t1;
        smkx_ppu_stat[PPU_ST_FRAMES]++;
        return;
    }
    if (nparts == 1)                        /* (plan holds the last one planned) */
        for (int pg = 0; pg < DSP_VPAGES; pg++) plan.fmt[pg] = part[0].fmt[pg];
    if (pre_ok) {
        uint32_t t2 = prof_now();
        while (!rt_ssi_play_done()) {}
        put(3);                             /* SSION: answers 1 */
        uint32_t spin;
        for (spin = 0; spin < 4000000 && !(HSR & 1); spin++) {}
        if ((HSR & 1) && (HRX & 0xFFFFFF) == 1) {
            int b0 = part[0].band0, b1 = part[0].band1;
            rt_m7_record(screen + b0 * PPU_W, (uint32_t)(b1 - b0) * PPU_W * 2);
            rt_ssi_play(sbuf, pre_nslot * 2);
            ssi_played();
            pre_ok = 0;
            smkx_ppu_stat[PPU_ST_SSI]++;
            smkx_ppu_stat[PPU_ST_T_SEND] += prof_now() - t2;
            smkx_ppu_stat[PPU_ST_FRAMES]++;
            return;
        }
        ssi_cap = 0;                        /* no answer: the host port from now on */
        pre_discard();
    }
    if (ssi_cap && pages_due()) {
        while (!rt_ssi_play_done()) {}      /* the last window */
        put(3);                             /* SSION: answers 1 */
        uint32_t spin;
        for (spin = 0; spin < 4000000 && !(HSR & 1); spin++) {}
        if ((HSR & 1) && (HRX & 0xFFFFFF) == 1) ssi = 1;
        else ssi_cap = 0;                   /* no answer: the host port from now on */
        nslot = 0;
    }
    stamp++;
    take_dirty();
    if (loaded == DSP_KERNEL_TC7) {
        for (int pg = 0; pg < PLANE; pg++) {
            if (plane_in[pg]) continue;
            upload(pg, TC_M7, tc_slot_p(pg));
            plane_in[pg] = 1;
        }
        slots(PLANE);
    } else {
        slots(0);
    }
    uint32_t t2 = prof_now();
    smkx_ppu_stat[PPU_ST_T_UPLOAD] += t2 - t1;
    int b0 = part[0].band0, b1 = part[0].band1;
    int whole = b0 == 0 && b1 == PPU_H;
    uint16_t *dst = screen + b0 * PPU_W;
    uint32_t bytes = (uint32_t)(b1 - b0) * PPU_W * 2;
    if (ssi) {
        if (!whole) {
            w24(0x11);                      /* BAND */
            w24((uint32_t)b0);
            w24((uint32_t)b1);
        }
        w24(0x10);                          /* FRAME */
        w24((uint32_t)part[0].n - 1);
        for (int i = 1; i < part[0].n; i++) w24(part[0].w[i]);
        while ((nslot + 2) & 7) w24(0);     /* whole DMA frames; 0 is no command */
        w24(9);                             /* SSIOFF: the last slots */
        rt_m7_record(dst, bytes);
        rt_ssi_play(sbuf, nslot * 2);
        ssi_played();
        smkx_ppu_stat[PPU_ST_SSI]++;
    } else {
        rt_m7_record(dst, bytes);
        if (!whole) {
            put(0x11);                      /* BAND */
            put((uint32_t)b0);
            put((uint32_t)b1);
        }
        put(0x10);
        put((uint32_t)part[0].n - 1);
        for (int i = 1; i < part[0].n; i++) put(part[0].w[i]);
    }
    smkx_ppu_stat[PPU_ST_T_SEND] += prof_now() - t2;
    smkx_ppu_stat[PPU_ST_FRAMES]++;
}

void smkx_ppu_send(uint16_t *screen)
{
#if SMKX_PARTIAL_CHECK == 2
    /* the picture goes as it would (the pre-built window too); when it is complete the DSP draws it whole
     * (the host port, the slots it keeps are the bookkeeping's) for the comparison */
    int np = nparts, drawn = 0;
    for (int k = 0; k < np; k++) drawn += part[k].band1 - part[k].band0;
    send_real(screen);
    if (np == 0) return;
    while (!rt_m7_record_done()) {}
    chk_fresh = smkx_ppu_check_full();
    chk_pnp = np;
    chk_drawn = drawn;
#else
    send_real(screen);
#endif
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
    (void)cgram;                            /* true colour: every colour exact */
}

int smkx_ppu_done(void)
{
    return rt_m7_record_done();
}

/* The list compositor on the Falcon (analysis/list-compositor-dsp-plan.md).
 *
 * Two pages, each a scroll ring of LC_N groups (plus LC_DUP mirrored behind it); the Videl shows a window of
 * one of them (start group, HSCROLL, line offset). Each frame lc_plan() decides which groups to compose; the
 * DSP (falcon/dsp/ppu.asm, LCB build) composes them, rendering the frame shifted so that the world's groups
 * line up with its own, and sends them as list records into RAM; this code only walks the list into the
 * page being drawn, copies the groups the other page missed, and points the Videl at the window. The window's
 * 17th group needs a second render of the frame, shifted by dl - 256; both are planned when the frame is
 * planned and sent one after the other.
 *
 * Frames are planned ahead: a job (plan, the DSP's two planned renders, its list buffers) is made from a copy of the
 * frame's registers, scroll and OAM while the DSP is still busy with the one before, so the CPU's planning and the
 * DSP's rendering overlap; the VRAM is the live one (what changed since is planned again by the next job: the
 * planner's pages hold the VRAM as of each plan). With SMKX_LC_SYNC there is one job at a time.
 *
 * smkx_lc_render() never waits for the DSP: it finishes what it can and returns. Freestanding
 * (m68k-atari-mintelf-gcc -ffreestanding). */
#include "snes_ppu.h"
#include "lc_plan.h"
#include "smkx_ppu.h"
#include "smkx_prof.h"

extern void smkx_vid_set_window(uint32_t addr, int hscroll, int lineoff);
extern void smkx_set_palette(const uint8_t *cgram, uint8_t inidisp);   /* src/smkx_video.c: the Videl palette from the next VBL on */
extern uint8_t rt_plan_dirty[128];  /* VRAM pages the game wrote since the planner looked (falcon/smkx_m7dsp.s) */

#define PAGE_GROUPS (LC_N + LC_DUP)
#define LIST_WORDS (PPU_H * 16 * 8)

static uint8_t page_raw[2][PAGE_GROUPS * 16 + 256];
static uint8_t *page[2];

/* stages of a job */
enum { J_FREE, J_PLANNED, J_DSP1, J_DSP2, J_DONE, J_BLIT };
/* what a job draws */
enum { K_DSP, K_COPY, K_WINDOW };

#ifndef SMKX_LC_DIAG
#define SMKX_LC_DIAG 0          /* 1: the job log (SMKX LCLOG), list and copy checks (statistics 16-18); CTX_LC_DIAG=1 */
#endif
extern uint32_t rt_frames;
extern void rt_cprint(const char *text);
extern void rt_cprint_hex(uint32_t value);

#if SMKX_LC_DIAG
/* The last jobs planned (printed at the end of a run: SMKX LCLOG frame S groups copies kind/cold/parity/na/nb, newest last) */
#define LOGN 256
static struct { uint32_t frame; uint16_t S; uint16_t ng, nc; uint8_t info, na, nb, ev; } lclog[LOGN];
static int lclog_n;
static void lclog_add(const LcPlan *pl, int info, int na, int nb, int ev)
{
    int i = lclog_n++ % LOGN;
    lclog[i].frame = rt_frames;
    lclog[i].S = (uint16_t)pl->S;
    lclog[i].ng = (uint16_t)pl->ngroups;
    lclog[i].nc = (uint16_t)pl->ncopies;
    lclog[i].info = (uint8_t)info;
    lclog[i].na = (uint8_t)(na > 255 ? 255 : na);
    lclog[i].nb = (uint8_t)(nb > 255 ? 255 : nb);
    lclog[i].ev = (uint8_t)ev;
}
void smkx_lc_dump(void)
{
    int n = lclog_n < LOGN ? lclog_n : LOGN, first = lclog_n < LOGN ? 0 : lclog_n % LOGN;
    for (int k = 0; k < n; k++) {
        int i = (first + k) % LOGN;
        rt_cprint("SMKX LCLOG ");
        rt_cprint_hex(lclog[i].frame);
        rt_cprint_hex(lclog[i].S);
        rt_cprint_hex(lclog[i].ng);
        rt_cprint_hex(lclog[i].nc);
        rt_cprint_hex((uint32_t)lclog[i].info << 24 | (uint32_t)lclog[i].na << 16 | (uint32_t)lclog[i].nb << 8 | lclog[i].ev);
        rt_cprint("\n");
    }
}
#else
#define lclog_add(pl, info, na, nb, ev) ((void)0)
void smkx_lc_dump(void) {}
#endif

typedef struct {
    LcPlan plan;
    int stage, kind, parity;
    int kernel;                         /* the DSP program: DSP_KERNEL_LC, or DSP_KERNEL_LCW for frames with windows */
    int na, nb;                         /* groups of the two renders */
    uint32_t words_a, words_b;
    uint16_t mask_b[PPU_H];
    uint8_t cgram[512], inidisp;        /* the palette that goes with the picture */
    uint16_t list[2][LIST_WORDS];
} Job;

#ifndef SMKX_LC_SYNC
#define SMKX_LC_SYNC 0
#endif
#ifndef SMKX_LC_CHECK
#define SMKX_LC_CHECK 0
#endif
#ifndef SMKX_LC_ALLPAGES
#define SMKX_LC_ALLPAGES 0
#endif
#define NJOB 2
static int depth = SMKX_LC_SYNC ? 1 : NJOB;   /* jobs queued at most: 1 while a snapshot's frame must be one instant */
#define DEPTH depth

void smkx_lc_set_depth(int d)
{
    depth = d < 1 ? 1 : d > NJOB ? NJOB : d;
}

static Job jobs[NJOB];
static int head, nq;                /* the oldest job and how many are queued (jobs[(head + i) % NJOB]) */
static int next_parity;             /* the page the next drawing job draws into */
static int last_S, last_dl, have_last;   /* the window of the newest planned job */
static uint8_t last_cgram[512], last_inidisp;   /* and its palette: a change alone makes a job */
static int fallback;                /* the newest frame is not covered: drain the queue, then let the caller draw */
static int shown = -1;              /* the page on display */
static int shown_S, shown_dl;
static int ready;                   /* the DSP runs the LC build */

int smkx_lc_blank;                  /* set when a frame that starts in forced blank was planned (nothing drawn); the caller clears it */
uint32_t smkx_lc_stat[22];          /* frames shown, frames planned, cold, groups, copies, fallbacks, plan ticks, walk ticks; list, copy, window ticks */

static void put_window(int p, int S, int dl)
{
    smkx_vid_set_window((uint32_t)(uintptr_t)page[p] + (uint32_t)S * 16, dl, (LC_NGR - (dl ? LC_WINW : LC_WINW - 1)) * 8);
    shown = p;
    shown_S = S;
    shown_dl = dl;
}

int smkx_lc_busy(void)
{
    return nq != 0;
}

int smkx_lc_active(void)
{
    return shown >= 0;
}

/* The shown picture as a plain screen of eight-plane groups (the snapshot's format). */
void smkx_lc_window(uint16_t *out)
{
    const uint16_t *pg = (const uint16_t *)(const void *)page[shown];
    int dl = shown_dl;
    for (int y = 0; y < PPU_H; y++)
        for (int j = 0; j < 16; j++) {
            const uint16_t *g0 = pg + (shown_S + y * LC_NGR + j) * 8;       /* as the Videl reads it: past the ring's end, in the mirror */
            const uint16_t *g1 = pg + (shown_S + y * LC_NGR + j + 1) * 8;
            for (int b = 0; b < 8; b++)
                *out++ = dl ? (uint16_t)((g0[b] << dl) | (g1[b] >> (16 - dl))) : g0[b];
        }
}

/* The frame's inputs with the picture shifted right by dx pixels: every BG scroll less dx, every sprite's x plus
 * dx (9 bits). Copies of what the planner reads, so two shifts of one frame stay apart. */
static uint8_t s_oam[2][PPU_OAM_BYTES];
static PpuOamSeg s_seg[2][PPU_OAMSEG_MAX];

static void shift_oam(uint8_t *oam, int dx)
{
    for (int i = 0; i < 128; i++) {
        int hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int x = oam[i * 4] | ((hi & 1) << 8);
        x = (x + dx) & 0x1FF;
        oam[i * 4] = (uint8_t)x;
        oam[512 + (i >> 2)] = (uint8_t)((oam[512 + (i >> 2)] & ~(1 << ((i & 3) * 2))) | ((x >> 8) << ((i & 3) * 2)));
    }
}

static void copy_bytes(void *d, const void *s, unsigned n)
{
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
}

static void shifted(const PpuInput *in, int dx, int k, PpuInput *out)
{
    *out = *in;                         /* the BG scrolls are shifted by the replay itself (ppu_replay_dx) */
    copy_bytes(s_oam[k], in->oam, PPU_OAM_BYTES);       /* the frame's OAM serves the rows before a segment's line */
    shift_oam(s_oam[k], dx);
    out->oam = s_oam[k];
    if (in->noamseg > 0) {
        for (int i = 0; i < in->noamseg; i++) {
            s_seg[k][i] = in->oamseg[i];
            shift_oam(s_seg[k][i].oam, dx);
        }
        out->oamseg = s_seg[k];
    }
}

static int popcount16(unsigned v)
{
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

static int count_groups(const uint16_t *m)
{
    int n = 0;
    for (int y = 0; y < PPU_H; y++) n += popcount16(m[y]);
    return n;
}

static uint32_t list_words(int groups)
{
    return (uint32_t)groups * 8;
}

extern void smkx_lc_cpu(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t groups, uint32_t rows);
extern void smkx_lc_blit(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t groups, uint32_t rows);
extern int smkx_lc_blit_busy(void);
extern void smkx_lc_dcache_clear(void);

/* Blocks of BLIT_MIN groups or more are queued while a job is walked; the biggest goes to the blitter (the job is
 * shown when the blitter is idle again: job stage J_BLIT), the others are copied by the CPU. Shorter blocks are
 * copied at once. */
#ifndef LC_BLIT_MIN
#define LC_BLIT_MIN 24
#endif
#define BLIT_MIN LC_BLIT_MIN
#define BQN 48
static struct { uint8_t *dst; const uint8_t *src; uint32_t sstride, len, rows; } bq[BQN];
static int bq_n;
static int blit_used;               /* the blitter wrote into a page since the data cache was cleared */

extern uint32_t lc_prof[10];
static void bq_start(int i)
{
    lc_prof[8] += bq[i].len * bq[i].rows;
    smkx_lc_blit(bq[i].dst, bq[i].src, bq[i].sstride, bq[i].len, bq[i].rows);
    blit_used = 1;
}

static void emit(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t len, uint32_t rows)
{
    if (len * rows < BLIT_MIN) {
        smkx_lc_cpu(dst, src, sstride, len, rows);
        return;
    }
    if (bq_n == BQN) {                              /* full: the CPU copies what is queued */
        for (int i = 0; i < bq_n; i++) smkx_lc_cpu(bq[i].dst, bq[i].src, bq[i].sstride, bq[i].len, bq[i].rows);
        bq_n = 0;
    }
    bq[bq_n].dst = dst; bq[bq_n].src = src; bq[bq_n].sstride = sstride; bq[bq_n].len = len; bq[bq_n].rows = rows;
    bq_n++;
}

/* The CPU copies the queued blocks but the biggest, which goes to the blitter. The blitter runs in hog mode
 * (falcon/smkx_lcput.s): in non-hog mode, with the CPU copying or planning meanwhile, Hatari (v2.6.1-devel, Sep 22
 * 2026, cpu-exact) left scattered groups wrong, differently from run to run (the synchronous build's snapshots show
 * it); hog mode was exact in every run. No speed gain was measured in Hatari (about 4.4 microseconds a group, the
 * CPU's 4.7), and on a Falcon the hog blit may starve the DSP's DMA record of the next job: untested. Switch:
 * SMWX_LC_BLIT_MIN=100000 keeps everything on the CPU. */
static int dsp_running(void);

static void bq_flush(void)
{
    if (dsp_running()) {
        /* A DSP job is recording its list meanwhile: a blitter run in hog mode keeps the bus, which a DMA record running at the
         * same time may not stand (on a Falcon: untested; Hatari's did not show it), so the CPU copies then. */
        for (int i = 0; i < bq_n; i++) smkx_lc_cpu(bq[i].dst, bq[i].src, bq[i].sstride, bq[i].len, bq[i].rows);
        bq_n = 0;
        return;
    }
    int big = 0;
    for (int i = 1; i < bq_n; i++)
        if (bq[i].len * bq[i].rows > bq[big].len * bq[big].rows) big = i;
    for (int i = 0; i < bq_n; i++)
        if (i != big) smkx_lc_cpu(bq[i].dst, bq[i].src, bq[i].sstride, bq[i].len, bq[i].rows);
    if (bq_n) bq_start(big);
    bq_n = 0;
}

/* 1 once the blitter is idle and the data cache is clear of what it wrote. */
static int blit_done(void)
{
    if (smkx_lc_blit_busy()) return 0;
    if (blit_used) {
        smkx_lc_dcache_clear();
        blit_used = 0;
    }
    return 1;
}

/* One group of ring group L from src (and from src + mdelta into the mirror behind the ring, when L lies in it). */
static void put_group(uint8_t *dst, const uint8_t *src, uint32_t mdelta, int L)
{
    smkx_lc_cpu(dst + L * 16, src, 16, 1, 1);
    if (L < LC_DUP) smkx_lc_cpu(dst + (L + LC_N) * 16, src + mdelta, 16, 1, 1);
}

/* A block of groups len wide, rows high, from ring group L0 on, one row every 32 groups (the ring's rows), out of
 * src (first row; its rows sstride bytes apart; the mirror's source is src + mdelta). The pieces that cross the
 * end of the ring or the edge of the mirror go group by group. Blocks of 24+ groups go to the blitter. */
static void put_block(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t mdelta, int L0, int len, int rows)
{
    /* A copy (mdelta != 0) takes ring group L from the same place of the other page, whatever row of the block it is: its source
     * pointer is recomputed from the ring position where the block wraps (advancing it by sstride would run on into the source
     * page's mirror, which is then no source for the destination's mirror: src + mdelta lies beyond it). A walk (mdelta 0)
     * reads a list, which has no ring position: its source goes on linearly. */
    const uint8_t *base = mdelta ? src - (uint32_t)L0 * 16 : 0;
    while (rows > 0) {
        if (L0 + len > LC_N) {
            for (int j = 0; j < len; j++) {
                int L = (L0 + j) & (LC_N - 1);
                put_group(dst, base ? base + (uint32_t)L * 16 : src + j * 16, mdelta, L);
            }
            L0 = (L0 + 32) & (LC_N - 1);
            src = base ? base + (uint32_t)L0 * 16 : src + sstride;
            rows--;
            continue;
        }
        int n = ((LC_N - len - L0) >> 5) + 1, m = 0, r;
        if (n > rows) n = rows;
        if (L0 + len <= LC_DUP) {
            m = ((LC_DUP - len - L0) >> 5) + 1;
            if (m > n) m = n;
            emit(dst + L0 * 16, src, sstride, (uint32_t)len, (uint32_t)m);
            emit(dst + (L0 + LC_N) * 16, src + mdelta, sstride, (uint32_t)len, (uint32_t)m);
        }
        r = m;
        if (r < n && L0 + r * 32 < LC_DUP) {
            for (int j = 0; j < len; j++) put_group(dst, src + r * sstride + j * 16, mdelta, L0 + r * 32 + j);
            r++;
        }
        if (r < n) emit(dst + (L0 + r * 32) * 16, src + r * sstride, sstride, (uint32_t)len, (uint32_t)(n - r));
        L0 = (L0 + n * 32) & (LC_N - 1);
        src = base ? base + (uint32_t)L0 * 16 : src + (uint32_t)n * sstride;
        rows -= n;
    }
}

/* The DSP's list: the eight plane words of each group, in row order and in a row in group order, so the masks say
 * where each one goes. Rows that repeat a mask are a band; a run of a band is one block (its source rows are the
 * band's row bytes apart). mask16: the bits of the window groups 0-15 (the 17th is jbase 16's bit 0). */
static void walk(const uint16_t *lst, uint8_t *pg, int S, int jbase, const uint16_t *mask)
{
    const uint8_t *data = (const uint8_t *)lst;
    int y = 0;
    while (y < PPU_H) {
        uint32_t m = mask[y];
        if (!m) { y++; continue; }
        int h = 1;
        while (y + h < PPU_H && mask[y + h] == m) h++;
        int nbits = 0;
        for (uint32_t v = m; v; v &= v - 1) nbits++;
        uint32_t rowbytes = (uint32_t)nbits * 16;
        const uint8_t *src = data;
        for (int j = 0; m >> j;) {
            if (!((m >> j) & 1)) { j++; continue; }
            int len = 0;
            while ((m >> (j + len)) & 1) len++;
            put_block(pg, src, rowbytes, 0, (S + jbase + y * LC_NGR + j) & (LC_N - 1), len, h);
            src += len * 16;
            j += len;
        }
        data += rowbytes * (uint32_t)h;
        y += h;
    }
}

/* The groups the other page missed, as blocks: runs of a row that repeat on the rows below (a scrolled column, a
 * sprite's box) are one block each. Big blocks go to the blitter, which runs on while the CPU goes on; the data cache
 * is cleared when it is done. */
#define MAXR 32
static struct { int y0, rows, j0, len; } rc[MAXR];

#if SMKX_LC_DIAG
static uint32_t cp_bad, cp_blocks, cp_first;
static int cp_pending;
static struct { int y0, rows, j0, len; } cp_rc[MAXR * 4];
#endif
static void flush_rect(const LcPlan *pl, uint8_t *dst, const uint8_t *src, int i)
{
    int L0 = (pl->S + rc[i].y0 * LC_NGR + rc[i].j0) & (LC_N - 1);
    put_block(dst, src + L0 * 16, 512, (uint32_t)LC_N * 16, L0, rc[i].len, rc[i].rows);
#if SMKX_LC_DIAG
    if (cp_pending < MAXR * 4) {                    /* checked after the job's blocks are all written (queued ones too) */
        cp_rc[cp_pending].y0 = rc[i].y0; cp_rc[cp_pending].rows = rc[i].rows; cp_rc[cp_pending].j0 = rc[i].j0; cp_rc[cp_pending].len = rc[i].len;
        cp_pending++;
    }
#endif
}

#if SMKX_LC_DIAG
static void cp_verify(const LcPlan *pl, const uint8_t *dst, const uint8_t *src)
{
    for (int k = 0; k < cp_pending; k++)
        for (int r = 0; r < cp_rc[k].rows; r++)
            for (int j = 0; j < cp_rc[k].len; j++) {
                int L = (pl->S + (cp_rc[k].y0 + r) * LC_NGR + cp_rc[k].j0 + j) & (LC_N - 1);
                int bad = 0;
                for (int b = 0; b < 16; b++) bad |= dst[L * 16 + b] != src[L * 16 + b];
                if (L < LC_DUP)
                    for (int b = 0; b < 16; b++) bad |= dst[(L + LC_N) * 16 + b] != src[(L + LC_N) * 16 + b];
                if (bad) { cp_bad++; if (!cp_first) cp_first = rt_frames; }
            }
    cp_blocks += (uint32_t)cp_pending;
    cp_pending = 0;
    smkx_lc_stat[16] = cp_bad;
    smkx_lc_stat[17] = cp_blocks;
    smkx_lc_stat[18] = cp_first;
}
#endif

static void copy_groups(const LcPlan *pl, uint8_t *dst, const uint8_t *src)
{
    int n = 0;
    for (int y = 0; y < PPU_H; y++) {
        uint32_t f = pl->cmask[y];
        uint32_t touched = 0;
        while (f) {
            int j0 = 0, len = 0;
            while (!((f >> j0) & 1)) j0++;
            while ((f >> (j0 + len)) & 1) len++;
            f &= ~(((2u << (len - 1)) - 1) << j0);
            int i;
            for (i = 0; i < n; i++)
                if (rc[i].j0 == j0 && rc[i].len == len && rc[i].y0 + rc[i].rows == y && !((touched >> i) & 1)) break;
            if (i < n) {
                rc[i].rows++;
                touched |= 1u << i;
            } else {
                if (n == MAXR) {
                    for (int k = 0; k < n; k++) flush_rect(pl, dst, src, k);
                    n = 0;
                    touched = 0;
                }
                rc[n].y0 = y; rc[n].rows = 1; rc[n].j0 = j0; rc[n].len = len;
                touched |= 1u << n;
                n++;
            }
        }
        /* rects that did not continue on this row are done (their slots stay: y0 + rows no longer matches) */
    }
    for (int k = 0; k < n; k++) flush_rect(pl, dst, src, k);
}

int smkx_lc_boot(void)
{
    uint32_t a = ((uint32_t)(uintptr_t)page_raw[0] + 255) & ~255u;
    page[0] = (uint8_t *)(uintptr_t)a;
    page[1] = page[0] + ((PAGE_GROUPS * 16 + 255) & ~255u);
    if (!smkx_ppu_boot(DSP_KERNEL_LC)) {
        ready = 0;
        return 0;
    }
    ready = 1;
    head = nq = 0;
    fallback = 0;
    have_last = 0;
    for (int i = 0; i < NJOB; i++) jobs[i].stage = J_FREE;
    lc_plan_reset();
    return 1;
}

/* The frame's inputs as they are now, in private arrays (the VRAM stays the live one). */
/* The planner reads the HDMA tables (a scroll or window effect per line) from WRAM, and the game goes on writing them while a
 * plan takes tens of frames: lc_plan reads one version of the tables, the DSP plans of the same job later ones, and the groups
 * the DSP composes are then not those the planner chose (found in the game: bands of a few rows that stayed wrong, in every picture
 * after, only in the pictures planned while the game ran; synchronous snapshots, which plan with the game stopped, were exact).
 * Every plan reads one private copy, taken as the plan starts (a torn copy is as good: both planners read the same). */
static uint8_t wram_snap[0x20000] __attribute__((aligned(16)));
static PpuBus bus_snap;
extern void smkx_lc_cpu(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t groups, uint32_t rows);
static uint8_t io_snap[0x2400];
static uint8_t cgram_snap[512];
static uint16_t bgofs_snap[8], m7_snap[6], m7ofs_snap[2];
static uint8_t oam_snap[PPU_OAM_BYTES];
static PpuOamSeg seg_snap[PPU_OAMSEG_MAX];
static PpuWrite log_snap[256];

static int snapshot(const PpuInput *in, PpuInput *out)
{
    if (in->nlog > 256 || in->noamseg > PPU_OAMSEG_MAX) return -1;
    *out = *in;
    copy_bytes(io_snap + 0x100, in->io + 0x100, 0x100);
    copy_bytes(io_snap + 0x2200, in->io + 0x2200, 0x200);
    copy_bytes(cgram_snap, in->cgram, 512);
    copy_bytes(bgofs_snap, in->bgofs, sizeof bgofs_snap);
    copy_bytes(m7_snap, in->m7, sizeof m7_snap);
    copy_bytes(m7ofs_snap, in->m7ofs, sizeof m7ofs_snap);
    out->io = io_snap;
    out->cgram = cgram_snap;
    out->bgofs = bgofs_snap;
    out->m7 = m7_snap;
    out->m7ofs = m7ofs_snap;
    copy_bytes(oam_snap, in->oam, PPU_OAM_BYTES);
    out->oam = oam_snap;
    if (in->noamseg > 0) {
        for (int i = 0; i < in->noamseg; i++) copy_bytes(&seg_snap[i], &in->oamseg[i], sizeof seg_snap[i]);
        out->oamseg = seg_snap;
    }
    copy_bytes(log_snap, in->log, (unsigned)in->nlog * sizeof(PpuWrite));
    out->log = log_snap;
    if (in->ctx && in->abus == ppu_abus_hirom) {
        const PpuBus *b = (const PpuBus *)in->ctx;
        smkx_lc_cpu(wram_snap, b->wram, 512, 32, 256);        /* 256 rows of 512 bytes */
        bus_snap = *b;
        bus_snap.wram = wram_snap;
        out->ctx = &bus_snap;
    }
    return 0;
}

#if SMKX_LC_DIAG
/* Diagnosis: the list is filled with a pattern before the DSP records into it; a group still holding it afterwards was not recorded. */
#define SENT 0xA5C3
static uint32_t sent_missing, sent_jobs, sent_first;
static void sent_fill(uint16_t *list, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++) list[i] = SENT;
}
static void sent_scan(const uint16_t *list, uint32_t words)
{
    uint32_t miss = 0, first = 0xFFFFFFFF;
    for (uint32_t g = 0; g < words / 8; g++) {
        int all = 1;
        for (int k = 0; k < 8; k++) if (list[g * 8 + k] != SENT) { all = 0; break; }
        if (all) { miss++; if (first == 0xFFFFFFFF) first = g; }
    }
    if (miss) {
        sent_missing += miss;
        sent_jobs++;
        if (!sent_first) sent_first = rt_frames;
    }
    smkx_lc_stat[16] = sent_missing;
    smkx_lc_stat[17] = sent_jobs;
    smkx_lc_stat[18] = sent_first;
    smkx_lc_stat[19] = miss ? first : smkx_lc_stat[19];
}
#else
#define sent_fill(l, w) ((void)0)
#define sent_scan(l, w) ((void)0)
#endif

static Job *job_at(int i)
{
    return &jobs[(head + i) % NJOB];
}

static int dsp_running(void)
{
    for (int i = 0; i < NJOB; i++)
        if (jobs[i].stage == J_DSP1 || jobs[i].stage == J_DSP2) return 1;
    return 0;
}

/* The walk of a finished job into its page, and the copies the other page missed; the blocks for the blitter are
 * started. The picture is on the screen (finish) when the blitter is idle again. */
static void issue(Job *j)
{
    uint32_t t0 = prof_now();
    uint8_t *pg = page[j->parity];
    if (j->kind == K_DSP) {
        if (j->na) walk(j->list[0], pg, j->plan.S, 0, j->plan.maskA);
        if (j->nb) walk(j->list[1], pg, j->plan.S, 16, j->mask_b);
    }
    uint32_t t1 = prof_now();
    if (j->kind != K_WINDOW) copy_groups(&j->plan, pg, page[j->parity ^ 1]);
    bq_flush();
#if SMKX_LC_DIAG
    if (j->kind != K_WINDOW) { smkx_lc_dcache_clear(); cp_verify(&j->plan, pg, page[j->parity ^ 1]); }
#endif
    smkx_lc_stat[8] += t1 - t0;
    smkx_lc_stat[9] += prof_now() - t1;
    smkx_lc_stat[7] += prof_now() - t0;
}

#if SMKX_LC_CHECK
/* SMKX_LC_CHECK (CTX_LC_CHECK=1, with SMKX_LC_SYNC): before a picture goes up, the page window is compared with the CPU reference of
 * the frame's inputs (colour math left out, as the compositor leaves it): the first differing jobs are reported with their plan.
 * One job at a time, so that the inputs (the planner's snapshot arrays) are still those of the job. Slow: seconds a picture. */
extern void rt_cprint(const char *text);
extern void rt_cprint_hex(uint32_t value);
static PpuInput chk_in;
static uint8_t chk_chunky[PPU_W * PPU_H], chk_ident[256];
static uint16_t chk_ref[PPU_W * PPU_H / 2], chk_win[PPU_W * PPU_H / 2];
static uint32_t chk_hist[256];
static int chk_reports;

#ifndef SMKX_SNAP_FROM
#define SMKX_SNAP_FROM 0
#endif
extern uint32_t rt_frames;

static void lc_check(const Job *j, int p, int S, int dl)
{
    if (rt_frames < (uint32_t)SMKX_SNAP_FROM) return;          /* (the check is slow: from the window's first frame on) */
    if (j->kind == K_WINDOW && (j->inidisp & 0x80)) return;
    const uint16_t *pg = (const uint16_t *)(const void *)page[p];
    uint16_t *out = chk_win;
    for (int y = 0; y < PPU_H; y++)
        for (int jj = 0; jj < 16; jj++) {
            const uint16_t *g0 = pg + (S + y * LC_NGR + jj) * 8;
            const uint16_t *g1 = pg + (S + y * LC_NGR + jj + 1) * 8;
            for (int b = 0; b < 8; b++) *out++ = dl ? (uint16_t)((g0[b] << dl) | (g1[b] >> (16 - dl))) : g0[b];
        }
    for (int i = 0; i < 256; i++) chk_ident[i] = (uint8_t)i;
    PpuInput ci = chk_in;
    ci.map = chk_ident;
    ci.hist = chk_hist;
    ci.m7sink = 0;
    io_snap[0x131] = 0;
    ppu_replay_nocg = 1;
    ppu_render(&ci, chk_chunky);
    ppu_replay_nocg = 0;
    ppu_planar8(chk_chunky, chk_ref);
    int nd = 0, fy = -1, fg = -1;
    for (int y = 0; y < PPU_H; y++)
        for (int g = 0; g < 16; g++) {
            int d = 0;
            for (int b = 0; b < 8; b++) d |= chk_win[(y * 16 + g) * 8 + b] != chk_ref[(y * 16 + g) * 8 + b];
            if (d) { nd++; if (fy < 0) { fy = y; fg = g; } }
        }
    if (!nd || chk_reports >= 40) return;
    chk_reports++;
    rt_cprint("LCCHK groups wrong ");
    rt_cprint_hex((uint32_t)nd);
    rt_cprint(" first row ");
    rt_cprint_hex((uint32_t)fy);
    rt_cprint(" group ");
    rt_cprint_hex((uint32_t)fg);
    rt_cprint(" kind ");
    rt_cprint_hex((uint32_t)j->kind);
    rt_cprint(" kernel ");
    rt_cprint_hex((uint32_t)j->kernel);
    rt_cprint(" page ");
    rt_cprint_hex((uint32_t)p);
    rt_cprint(" S ");
    rt_cprint_hex((uint32_t)S);
    rt_cprint(" dl ");
    rt_cprint_hex((uint32_t)dl);
    rt_cprint(" composed ");
    rt_cprint_hex((uint32_t)j->plan.ngroups);
    rt_cprint(" copies ");
    rt_cprint_hex((uint32_t)j->plan.ncopies);
    rt_cprint(" cold ");
    rt_cprint_hex((uint32_t)j->plan.cold);
    rt_cprint(" row masks A ");
    rt_cprint_hex(j->plan.maskA[fy]);
    rt_cprint(" B ");
    rt_cprint_hex(j->plan.maskB[fy]);
    rt_cprint(" C ");
    rt_cprint_hex(j->plan.cmask[fy]);
    rt_cprint("\n");
}
#endif

static void finish(Job *j)
{
    uint32_t t0 = prof_now();
#if SMKX_LC_CHECK
    lc_check(j, j->kind == K_WINDOW ? (shown >= 0 ? shown : j->parity) : j->parity, j->plan.S, j->plan.dl);
#endif
    lclog_add(&j->plan, 0x10 | j->kind << 1 | j->parity, 0, 0, 5);
    put_window(j->kind == K_WINDOW ? (shown >= 0 ? shown : j->parity) : j->parity, j->plan.S, j->plan.dl);
    smkx_set_palette(j->cgram, j->inidisp);
    smkx_lc_stat[10] += prof_now() - t0;
    smkx_lc_stat[0]++;
}

/* Moves the queue on: the DSP's finished render, the next planned job onto the DSP, the oldest finished job onto the
 * screen. Returns 1 when a picture went up. */
static int pump(void)
{
    int shown_new = 0;
    smkx_lc_stat[12] = lc_diff;
    smkx_lc_stat[13] = nq ? (uint32_t)job_at(0)->stage | (uint32_t)(nq > 1 ? job_at(1)->stage : 0) << 8 : 0xFF;
    smkx_lc_stat[14]++;
    smkx_lc_stat[15] = (uint32_t)smkx_ppu_done();
    for (;;) {
        int progress = 0;
        int busy = 0;
        for (int i = 0; i < nq; i++) {
            Job *j = job_at(i);
            if (j->stage != J_DSP1 && j->stage != J_DSP2) continue;
            busy = 1;
            if (!smkx_ppu_done()) break;
            progress = 1;
            sent_scan(j->stage == J_DSP1 ? j->list[0] : j->list[1], j->stage == J_DSP1 ? j->words_a : j->words_b);
            if (j->stage == J_DSP1 && j->nb) {
                smkx_ppu_restore((int)(j - jobs) * 2 + 1);          /* the second render, planned with the first */
                sent_fill(j->list[1], j->words_b);
                smkx_ppu_send_lc(j->mask_b, j->list[1], j->words_b);
                j->stage = J_DSP2;
            } else {
                j->stage = J_DONE;
                busy = 0;
            }
            break;
        }
        if (!busy)
            for (int i = 0; i < nq; i++) {
                Job *j = job_at(i);
                if (j->stage != J_PLANNED || j->kind != K_DSP) continue;
                int idx = (int)(j - jobs);
                if (smkx_ppu_loaded() != j->kernel && !smkx_ppu_boot(j->kernel)) {
                    ready = 0;                          /* no answer from the DSP: the compositor stops (the jobs are dropped) */
                    for (int k = 0; k < NJOB; k++) jobs[k].stage = J_FREE;
                    head = nq = 0;
                    lc_plan_reset();
                    return 0;
                }
                if (j->na) {
                    smkx_ppu_restore(idx * 2);
                    sent_fill(j->list[0], j->words_a);
                    smkx_ppu_send_lc(j->plan.maskA, j->list[0], j->words_a);
                    j->stage = J_DSP1;
                } else {
                    smkx_ppu_restore(idx * 2 + 1);
                    sent_fill(j->list[1], j->words_b);
                    smkx_ppu_send_lc(j->plan.maskB, j->list[1], j->words_b);
                    j->stage = J_DSP2;
                }
                progress = 1;
                break;
            }
        Job *h = job_at(0);
        if (nq && (h->stage == J_DONE || (h->stage == J_PLANNED && h->kind != K_DSP))) {
            issue(h);
            h->stage = J_BLIT;
            progress = 1;
        }
        if (nq && h->stage == J_BLIT && blit_done()) {
            finish(h);
            h->stage = J_FREE;
            head = (head + 1) % NJOB;
            nq--;
            shown_new = 1;
            progress = 1;
        }
        if (!progress) break;
    }
    return shown_new;
}

static void lc_fail(void)
{
    /* the DSP did not take the frame: start cold next time, and let the caller draw this one */
    lc_plan_reset();
    smkx_lc_stat[5]++;
}

/* Plans the next frame into a new job. 1: queued; 0: nothing to draw; -1: not a frame the compositor draws. */
static int plan_job(const PpuInput *live)
{
    PpuInput in;
    if (snapshot(live, &in) < 0) {
        lc_plan_reset();
        return -1;
    }
    Job *j = &jobs[(head + nq) % NJOB];
    uint32_t t0 = prof_now();
#if SMKX_LC_CHECK
    chk_in = in;
#endif
    int rc = lc_plan(&in, &j->plan, SMKX_LC_ALLPAGES ? 0 : rt_plan_dirty);
   /* (ALLPAGES: compare every page, as the host model does) */
    if (rc > 0) {
        /* Forced blank at the frame's start and no IRQ writes: nothing to draw, nothing forgotten (CT's scene changes hold
         * it for many frames). The ring stays; what is on screen is hidden by the palette (the Videl's palette is black
         * in forced blank), as a window job of its own so that it comes after the jobs queued before it. */
        smkx_lc_blank = 1;
        if (!have_last || (last_inidisp & 0x80)) return 0;
        copy_bytes(j->cgram, in.cgram, 512);
        j->inidisp = in.io[0x100];
        j->plan.S = last_S;
        j->plan.dl = last_dl;
        j->na = j->nb = 0;
        j->kind = K_WINDOW;
        j->parity = next_parity ^ 1;
        j->stage = J_PLANNED;
        copy_bytes(last_cgram, j->cgram, 512);
        last_inidisp = j->inidisp;
        nq++;
        return 1;
    }
    if (rc < 0) {
        smkx_lc_stat[11] = lc_why;              /* the last reason (lc_plan.c) */
        return -1;
    }
    smkx_lc_stat[6] += prof_now() - t0;
    lclog_add(&j->plan, j->plan.cold, count_groups(j->plan.maskA), count_groups(j->plan.maskB), 1);
    smkx_lc_stat[1]++;
    smkx_lc_stat[2] += j->plan.cold;
    smkx_lc_stat[3] += (uint32_t)j->plan.ngroups;
    smkx_lc_stat[4] += (uint32_t)j->plan.ncopies;
    copy_bytes(j->cgram, in.cgram, 512);
    j->inidisp = in.io[0x100];
    int pal_changed = !have_last || j->inidisp != last_inidisp;
    for (int i = 0; i < 512; i++) if (j->cgram[i] != last_cgram[i]) { pal_changed = 1; break; }
    j->na = count_groups(j->plan.maskA);
    j->nb = count_groups(j->plan.maskB);
    j->stage = J_PLANNED;
    if (j->na + j->nb == 0) {
        if (j->plan.ncopies == 0) {
            /* nothing to draw: the page holding the newest frame is the picture; only the window may have moved */
            if (have_last && last_S == j->plan.S && last_dl == j->plan.dl && !pal_changed) { lclog_add(&j->plan, j->plan.cold, 0, 0, 2); return 0; }
            j->kind = K_WINDOW;
            j->parity = next_parity ^ 1;
        } else {
            j->kind = K_COPY;                           /* copies only: no DSP work */
            j->parity = next_parity;
            next_parity ^= 1;
        }
    } else {
        j->kind = K_DSP;
        j->parity = next_parity;
        /* colour math is left out (the compositor has no sub screen): CGADSUB reads 0 while the DSP plans the frame */
        io_snap[0x131] = 0;
        PpuInput ia, ib;
        int idx = (int)(j - jobs);
        ppu_replay_nocg = 1;            /* the compositor draws indices: HDMA gradients on CGRAM cost nothing */
        int ka = -1, kb = -1;
        if (j->na) {
            shifted(&in, j->plan.dl, 0, &ia);
            ppu_replay_dx = j->plan.dl;
            ka = smkx_ppu_plan_lc(&ia, 0);
            ppu_replay_dx = 0;
            if (ka < 0) { ppu_replay_nocg = 0; lc_fail(); return -1; }
            smkx_ppu_keep(idx * 2);
        }
        if (j->nb) {
            shifted(&in, j->plan.dl - 256, 1, &ib);
            ppu_replay_dx = j->plan.dl - 256;
            kb = smkx_ppu_plan_lc(&ib, 0);
            ppu_replay_dx = 0;
            if (kb < 0) { ppu_replay_nocg = 0; lc_fail(); return -1; }
            smkx_ppu_keep(idx * 2 + 1);
        }
        /* windows: both renders of a frame run on one program, so the one that did not need the window build is planned again for it */
        j->kernel = ka >= 0 ? ka : kb;
        if (ka >= 0 && kb >= 0 && ka != kb) {
            j->kernel = DSP_KERNEL_LCW;
            int k2 = -1;
            if (ka == DSP_KERNEL_LC) {
                ppu_replay_dx = j->plan.dl;
                k2 = smkx_ppu_plan_lc(&ia, 1);
                ppu_replay_dx = 0;
                if (k2 == DSP_KERNEL_LCW) smkx_ppu_keep(idx * 2);
            } else {
                ppu_replay_dx = j->plan.dl - 256;
                k2 = smkx_ppu_plan_lc(&ib, 1);
                ppu_replay_dx = 0;
                if (k2 == DSP_KERNEL_LCW) smkx_ppu_keep(idx * 2 + 1);
            }
            if (k2 != DSP_KERNEL_LCW) { ppu_replay_nocg = 0; lc_fail(); return -1; }
        }
        ppu_replay_nocg = 0;
        j->words_a = list_words(j->na);
        j->words_b = list_words(j->nb);
        for (int y = 0; y < PPU_H; y++) j->mask_b[y] = j->plan.maskB[y];
        next_parity ^= 1;
    }
    last_S = j->plan.S;
    last_dl = j->plan.dl;
    have_last = 1;
    copy_bytes(last_cgram, j->cgram, 512);
    last_inidisp = j->inidisp;
    nq++;
    return 1;
}

/* Moves the jobs on without planning a new one (the frame at hand is not a picture). */
void smkx_lc_pump(void)
{
    if (ready) pump();
}

/* Returns 1: a new picture is on the screen (the Videl points at it); 0: nothing new; -1: the frame is not one
 * the list compositor draws (or the DSP did not answer): the caller draws it another way. */
int smkx_lc_render(const PpuInput *in)
{
    if (!ready) return -1;
    int r = pump();
    if (fallback) {
        if (nq) return r;
        fallback = 0;
        if (shown >= 0) smkx_lc_stat[5]++;
        shown = -1;                                     /* the plain screen is shown from now on */
        have_last = 0;
        return -1;
    }
    if (nq < DEPTH && !(depth == 1 && r)) {         /* (one job at a time: the frame just shown is this call's) */
        int p = plan_job(in);
        if (p < 0) {
            fallback = 1;
            have_last = 0;
            if (!nq) {
                fallback = 0;
                if (shown >= 0) smkx_lc_stat[5]++;
                shown = -1;
                return -1;
            }
        } else if (p > 0) {
            r |= pump();
        }
    }
    return r;
}

/* Host model of the DSP graphics orchestrator (analysis/list-compositor-dsp-plan.md).
 *
 * What the DSP decides, from what the runtime can tell it (VRAM written, OAM, registers, scroll) and
 * nothing else: which 16-pixel plane groups of the page each frame has to be written, in a scroll ring
 * with two pages. The content of a written group here comes from the reference renderer (colour math off,
 * the window extended by 16 pixels each side through shifted renders): composing is a separate question
 * (the DSP's group composer). What this model proves is the orchestration: after the stores, the shown
 * page equals the reference picture on every frame, and how many groups that took.
 *
 *   lcd_model rom.sfc SNAP.BIN...        consecutive frames, oldest first (SMW snapshots in CT's layout)
 *
 * The list the walker would get per frame: STORE (ring row, group, 16 pixels) for every group the frame
 * composes and COPY (ring row, group) for every group the other page missed: its last frame's groups.
 *
 * Covered frames: one uniform register state for the whole frame, BG mode 0 or 1 with 8x8 tiles, one
 * OAM, no windows, no mosaic, no forced blank. Others are "slow": the exact renderer draws them, the
 * pages are invalid afterwards and the next covered frame starts cold.
 */
#define main lc_model_main
#include "lc_model.c"
#undef main
#include "lc_plan.h"

#define RH LC_RH
#define NGR LC_NGR
#define N LC_N
#define DUP LC_DUP
#define WINW LC_WINW

static uint8_t page[2][N + DUP][16];   /* the two pages: 16 CGRAM indices a group (planar on the Falcon) */

typedef struct { int ok; uint8_t F[PPU_H][PPU_W], EL[PPU_H][16], ER[PPU_H][16]; } LFrame;

/* F with colour math off: CGADSUB cleared in the shadow and in the IRQ log. */
static void render_nomath(const Snap *s, int dx, uint8_t *out)
{
    Snap *t = snap_clone(s);
    t->io[0x2131 - 0x2000] = 0;
    for (int i = 0; i < t->nlog; i++)
        if (t->log[i].reg == 0x2131) t->log[i].value = 0;
    if (dx) {
        for (int b = 0; b < 4; b++) t->bgofs[b * 2] = (uint16_t)((t->bgofs[b * 2] - dx) & 0x3FF);
        /* sprites move with the picture: x + dx, 9 bits */
        for (int seg = -1; seg < t->noamseg; seg++) {
            uint8_t *oam = seg < 0 ? t->oam : t->oamseg[seg].oam;
            for (int i = 0; i < 128; i++) {
                int hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
                int x = oam[i * 4] | ((hi & 1) << 8);
                x = (x + dx) & 0x1FF;
                oam[i * 4] = (uint8_t)x;
                oam[512 + (i >> 2)] = (uint8_t)((oam[512 + (i >> 2)] & ~(1 << ((i & 3) * 2))) | ((x >> 8) << ((i & 3) * 2)));
            }
        }
    }
    PpuInput in = snap_input(t, t->io);
    ppu_render(&in, out);
    free(t);
}

static void frame_renders(const Snap *s, LFrame *f)
{
    static uint8_t l[PPU_H][PPU_W], r[PPU_H][PPU_W];
    render_nomath(s, 0, &f->F[0][0]);
    render_nomath(s, 16, &l[0][0]);
    render_nomath(s, -16, &r[0][0]);
    for (int y = 0; y < PPU_H; y++) {
        memcpy(f->EL[y], l[y], 16);             /* original x -16..-1 are shifted x 0..15 */
        memcpy(f->ER[y], r[y] + 240, 16);       /* original x 256..271 are shifted x 240..255 */
    }
}


/* The walker's list for one frame. */
typedef struct { int L; uint8_t px[16]; } Store;
static Store stores[N];
static int nstores;
static FILE *rec;

static void c2p_group(const uint8_t *px, uint16_t *w)
{
    for (int b = 0; b < 8; b++) {
        uint16_t v = 0;
        for (int k = 0; k < 16; k++) v |= (uint16_t)(((px[k] >> b) & 1) << (15 - k));
        w[b] = v;
    }
}

static void put16(uint16_t v) { fputc(v >> 8, rec); fputc(v & 255, rec); }
static void put32(uint32_t v) { put16((uint16_t)(v >> 16)); put16((uint16_t)v); }

static uint32_t window_sum(int p, int S, int dl)
{
    uint32_t sum = 0;
    for (int y = 0; y < PPU_H; y++)
        for (int j = 0; j < (dl ? WINW : WINW - 1); j++) {
            uint16_t w[8];
            c2p_group(page[p][S + y * NGR + j], w);
            for (int b = 0; b < 8; b++) sum = (sum << 1 | sum >> 31) + w[b];
        }
    return sum;
}

static LcPlan plan;

static void dbg_win_fn(void *ctx, int row, const PpuRegs *r)
{
    (void)ctx;
    uint16_t sp[3][2];
    printf("  row %3d: bg %04x %04x %04x %04x %04x %04x TM %02x TMW %02x W12 %02x W34 %02x WOBJ %02x WH %02x %02x %02x %02x layers %02x", row, r->bgofs[0], r->bgofs[1], r->bgofs[2], r->bgofs[3], r->bgofs[4], r->bgofs[5], r->r[0x2C], r->r[0x2E], r->r[0x23], r->r[0x24], r->r[0x25], r->r[0x26], r->r[0x27], r->r[0x28], r->r[0x29], ppu_win_layers(r));
    for (int i = 0; i < 5; i++) { int n = (ppu_win_layers(r) >> i) & 1 ? ppu_win_spans(r, i, sp) : 0; for (int k = 0; k < n; k++) printf(" L%d[%d+%d]", i, sp[k][0], sp[k][1]); }
    printf("\n");
}
static const uint8_t *lcd_cgram;
static int plan_inidisp;

/* One covered frame: the plan's groups composed from the reference, the copies, the check. */
static void execute(const LFrame *f, int parity, int *bad)
{
    int p = parity, q = parity ^ 1;
    int x0 = plan.wx >> 4, wx = plan.wx, dl = plan.dl;
    nstores = 0;
    for (int y = 0; y < PPU_H; y++)
        for (int j = 0; j < WINW; j++) {
            int on = j < 16 ? (plan.maskA[y] >> j) & 1 : plan.maskB[y] & 1;
            if (!on) continue;
            int L = (plan.S + y * NGR + j) & (N - 1);
            Store *sp = &stores[nstores++];
            sp->L = L;
            for (int k = 0; k < 16; k++) {
                int xs = 16 * (x0 + j) + k - wx;
                uint8_t v = 0;
                if (xs < 0) v = xs >= -16 ? f->EL[y][xs + 16] : 0;
                else if (xs < 256) v = f->F[y][xs];
                else v = xs < 272 ? f->ER[y][xs - 256] : 0;
                sp->px[k] = v;
            }
            memcpy(page[p][L], sp->px, 16);
            if (L < DUP) memcpy(page[p][L + N], sp->px, 16);
        }
    for (int i = 0; i < plan.ncopies; i++) {
        int L = plan.copies[i];
        memcpy(page[p][L], page[q][L], 16);
        if (L < DUP) memcpy(page[p][L + N], page[q][L + N], 16);
    }
    int b = 0;
    for (int y = 0; y < PPU_H; y++)
        for (int x = 0; x < PPU_W; x++)
            b += page[p][plan.S + y * NGR + ((x + dl) >> 4)][(x + dl) & 15] != f->F[y][x];
    *bad = b;
    if (rec) {
        /* the Videl palette: CGRAM scaled by INIDISP brightness, as smkx_video.c make_clut, sent when it changed */
        static uint32_t last_pal[256];
        static int have_pal;
        uint32_t pal[256];
        int bright = plan_inidisp & 15;
        for (int i = 0; i < 256; i++) {
            unsigned c = lcd_cgram[i * 2] | (lcd_cgram[i * 2 + 1] << 8);
            uint32_t r = (c & 31) * 63 * bright / (31 * 15), g = ((c >> 5) & 31) * 63 * bright / (31 * 15),
                     b = ((c >> 10) & 31) * 63 * bright / (31 * 15);
            pal[i] = (r << 26) | (g << 18) | (b << 2);
        }
        if (!have_pal || memcmp(pal, last_pal, sizeof pal)) {
            put16(3); put16(0);
            for (int i = 0; i < 256; i++) put32(pal[i]);
            memcpy(last_pal, pal, sizeof pal);
            have_pal = 1;
        }
        put16(1); put16((uint16_t)nstores);
        for (int i = 0; i < nstores; i++) {
            uint16_t w[8];
            put16((uint16_t)stores[i].L);
            c2p_group(stores[i].px, w);
            for (int k = 0; k < 8; k++) put16(w[k]);
        }
        put16(2); put16((uint16_t)plan.ncopies);
        for (int i = 0; i < plan.ncopies; i++) put16(plan.copies[i]);
        put16(0); put16((uint16_t)plan.S); put16((uint16_t)dl); put32(window_sum(p, plan.S, dl));
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s rom.sfc SNAP.BIN...   (LCD_REC=file writes the walker's stream)\n", argv[0]);
        return 2;
    }
    long n;
    const uint8_t *rom = load(argv[1], &n);
    smw_rules = 1;
    ppu_init();
    static LFrame fr;
    int parity = 0, slow = 0, covered = 0, badframes = 0, totbad = 0, nfull = 0, frames_out = 0;
    long tcomp = 0, tcopy = 0, maxcomp = 0;
    if (getenv("LCD_REC")) {
        rec = fopen(getenv("LCD_REC"), "wb");
        fwrite("LCW1", 1, 4, rec);
        put32(0);                      /* frames, patched at the end */
        put32(N);
        put32(DUP);
    }
    printf("frame          comp  copy  bad  note\n");
    for (int a = 2; a < argc; a++) {
        Snap *s = snap_load(argv[a], rom);
        PpuInput in = snap_input(s, s->io);
        if (lc_plan(&in, &plan, NULL) != 0) {
            printf("%-12s   slow (why %03x) BGMODE %02x TM %02x TS %02x MOSAIC %02x W12 %02x W34 %02x WOBJ %02x TMW %02x CGWSEL %02x CGADSUB %02x"
                   " HDMAEN %02x IRQ writes %d OAM segs %d first differing reg $21%02x row %d\n", s->name, lc_why, s->io[0x105], s->io[0x12C], s->io[0x12D], s->io[0x106],
                   s->io[0x123], s->io[0x124], s->io[0x125], s->io[0x12E], s->io[0x130], s->io[0x131], s->io[0x220C], s->nlog, s->noamseg, (lc_diff >> 16) & 0xFF, (lc_diff >> 8) & 0xFF);
            slow++;
            free(s);
            continue;
        }
        if (getenv("LCD_WIN")) { PpuInput wi = snap_input(s, s->io); ppu_replay_nocg = 1; ppu_replay_changes(&wi, dbg_win_fn, NULL); ppu_replay_nocg = 0; }
        lcd_cgram = s->cgram;
        plan_inidisp = s->io[0x100];
        frame_renders(s, &fr);
        if (getenv("LCD_PPM") && covered + 1 == atoi(getenv("LCD_PPM"))) {       /* the reference picture of one frame */
            FILE *pf = fopen(getenv("LCD_PPM_FILE"), "wb");
            int bright = plan_inidisp & 15;
            fprintf(pf, "P6\n%d %d\n255\n", PPU_W, PPU_H);
            for (int y = 0; y < PPU_H; y++)
                for (int x = 0; x < PPU_W; x++) {
                    unsigned c = lcd_cgram[fr.F[y][x] * 2] | (lcd_cgram[fr.F[y][x] * 2 + 1] << 8);
                    for (int k = 0; k < 3; k++) {
                        int v6 = ((c >> (5 * k)) & 31) * 63 * bright / (31 * 15);
                        fputc(v6 << 2 | v6 >> 4, pf);
                    }
                }
            fclose(pf);
        }
        int bad;
        execute(&fr, parity, &bad);
        parity ^= 1;
        covered++;
        frames_out++;
        totbad += bad;
        badframes += bad != 0;
        tcomp += plan.ngroups;
        tcopy += plan.ncopies;
        if (plan.ngroups > maxcomp) maxcomp = plan.ngroups;
        if (plan.cold) nfull++;
        printf("%-12s %5d %5d %5d  %s S %d dl %d wx %d wy %d\n", s->name, plan.ngroups, plan.ncopies, bad, plan.cold ? "cold" : "",
               plan.S, plan.dl, plan.wx, plan.wy);
        free(s);
    }
    if (rec) {
        fseek(rec, 4, SEEK_SET);
        put32((uint32_t)frames_out);
        fclose(rec);
    }
    printf("%d covered, %d slow; %d cold starts; mean per frame: %ld groups composed, %ld copied; max %ld;"
           " %d frames wrong, %d wrong pixels\n",
           covered, slow, nfull, covered ? tcomp / covered : 0, covered ? tcopy / covered : 0, maxcomp, badframes,
           totbad);
    return badframes != 0;
}

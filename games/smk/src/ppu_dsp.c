/* The frame stream for the DSP renderer (falcon/dsp/ppu.asm, FRAME).
 *
 * The frame loop is the reference's (ppu_render_rows: HDMA, IRQ-logged
 * writes, OAM segments); per row only the register fields the DSP derives
 * its lines from are compared with what was sent, and changes go out as
 * SET records; unchanged rows go out as runs of LINE. The DSP keeps the
 * fields as its shadow and derives every line itself (layer descriptors,
 * paint order, Mode 7 setup). OAM goes up as the compact sprite list
 * whenever the OAM segment or OBSEL changes.
 * Shared by the Falcon runtime and PPUTEST; freestanding. */
#include "ppu_dsp.h"
#include "ppu_internal.h"

enum { OP_END = 0, OP_LINE = 1, OP_SET = 2, OP_OAM = 3 };
enum {
    F_INI = 0, F_MODE = 1, F_SC = 2, F_NBA = 6, F_OFS = 8, F_M7SEL = 16,
    F_M7 = 17, F_M7OFS = 23, F_TM = 25, F_N = 26,
    /* the windows: per layer (BG1-4, OBJ) three spans x << 12 | len, len 0
     * for none (ppu_win_spans); one dirty bit (26) for all of them */
    F_WIN = 26, F_WINN = 15
};

static const uint8_t obj_sz[8][2][2] = {
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

int ppu_dsp_oam(const uint8_t *oam, uint8_t obsel, uint32_t *w)
{
    uint32_t base = (uint32_t)(obsel & 7) << 13;
    uint32_t gap = (uint32_t)(((obsel >> 3) & 3) + 1) << 12;
    int n = 0;
    for (int i = 0; i < 128; i++) {
        const uint8_t *e = oam + i * 4;
        uint8_t hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int wd = obj_sz[obsel >> 5][hi >> 1][0], h = obj_sz[obsel >> 5][hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        if (x <= -wd || x >= 256) continue;     /* never in a line's list */
        if (e[1] >= PPU_H && e[1] + h <= 256) continue;  /* on no screen row: never */
        uint32_t tbl = ((base + ((e[3] & 1) ? gap : 0)) >> 12) & 7;
        w[1 + n * 2] = (uint32_t)(x + 64) << 15 | (uint32_t)h << 8 | e[1];
        w[2 + n * 2] = e[2] | (uint32_t)e[3] << 8 | tbl << 16 | (uint32_t)(wd / 8 - 1) << 19;
        n++;
    }
    w[0] = (uint32_t)n;
    return 1 + n * 2;
}

static PpuDspPut out_put;
static int32_t sent[F_WIN + F_WINN];
static int pending;             /* rows not yet sent as LINE */
static const uint8_t *sent_oam;
static uint8_t sent_obsel;

static void flush(void)
{
    if (pending) out_put((uint32_t)OP_LINE << 16 | (uint32_t)pending);
    pending = 0;
}

static int first;

static int32_t field(const Regs *s, int f)
{
    if (f == F_INI) return s->r[0x00];
    if (f == F_MODE) return s->r[0x05];
    if (f < F_NBA) return s->r[0x07 + f - F_SC];
    if (f < F_OFS) return s->r[0x0B + f - F_NBA];
    if (f < F_M7SEL) return s->bgofs[f - F_OFS];
    if (f == F_M7SEL) return s->r[0x1A];
    if (f < F_M7OFS) return s->m7[f - F_M7];
    if (f < F_TM) return s->m7ofs[f - F_M7OFS];
    return s->r[0x2C];
}

/* Per row: only the fields written since the last row (Regs.dirty, set by
 * the frame loop's register writes) are compared with what was sent. */
static void dsp_row(const PpuInput *in, const Regs *s, int row, uint8_t *o)
{
    (void)in;
    (void)row;
    (void)o;
    uint32_t d = s->dirty;
    if (first || (d & (1u << F_WIN))) {
        int32_t w[F_WINN] = {0};
        for (int i = 0; i < 5; i++) {
            uint16_t sp[3][2];
            int n = ppu_win_spans(s, i, sp);    /* (TM decides what is drawn) */
            for (int k = 0; k < n; k++) w[i * 3 + k] = (int32_t)(sp[k][0] << 12 | sp[k][1]);
        }
        for (int k = 0; k < F_WINN; k++) {
            if (!first && w[k] == sent[F_WIN + k]) continue;
            flush();
            sent[F_WIN + k] = w[k];
            out_put((uint32_t)OP_SET << 16 | (uint32_t)(F_WIN + k));
            out_put((uint32_t)w[k]);
        }
        d &= ~(1u << F_WIN);
        ((Regs *)s)->dirty &= ~(1u << F_WIN);
    }
    if (d) {
        ((Regs *)s)->dirty = 0;             /* (the frame loop's own state) */
        for (int f = 0; d; f++, d >>= 1) {
            while (!(d & 0xFF)) {           /* (whole bytes of clean fields) */
                d >>= 8;
                f += 8;
            }
            if (!(d & 1) || f >= F_N) continue;
            int32_t v = field(s, f);
            if (!first && v == sent[f]) continue;
            flush();
            sent[f] = v;
            out_put((uint32_t)OP_SET << 16 | (uint32_t)f);
            out_put((uint32_t)v & 0xFFFFFF);
        }
    }
    if (first || s->oam != sent_oam || s->r[0x01] != sent_obsel) {
        static uint32_t w[1 + 128 * 2];
        int n = ppu_dsp_oam(s->oam, s->r[0x01], w);
        flush();
        out_put((uint32_t)OP_OAM << 16);
        for (int i = 0; i < n; i++) out_put(w[i]);
        sent_oam = s->oam;
        sent_obsel = s->r[0x01];
    }
    first = 0;
    pending++;
}

/* ---- the same stream as SSI slots, with a fast path ------------------------
 *
 * ppu_dsp_frame_slots writes what ppu_dsp_frame sends as the 16-bit slots
 * the DSP reads from the SSI (a word: bits 23-8, then 7-0). Runs of plain
 * lines take a small loop instead of the frame loop and dsp_row (whose code,
 * several KB, does not stay in the 68030's 256-byte instruction cache): a
 * run has no IRQ write, no OAM segment and no HDMA entry change, and every
 * transferring channel is a repeat-mode Mode 7 matrix channel reading one
 * word a line from a contiguous region (SMK's race: 168 of 224 lines). The
 * loop writes the SET records dsp_row would write for those fields. */

static uint16_t *so, *so_end;
static int so_over;

static void slot_put(uint32_t w)
{
    if (so >= so_end) { so_over = 1; return; }
    so[0] = (uint16_t)(w >> 8);
    so[1] = (uint16_t)(w & 0xFF);
    so += 2;
}

uint32_t ppu_dsp_fast_lines;            /* lines the fast path took (statistics) */
/* Diagnostics: with a clock set (PPUTEST's benchmark), the slot builder
 * adds its time per part: frame start, fast runs, slow lines, the end. */
uint32_t (*ppu_dsp_clock)(void);
uint32_t ppu_dsp_t[4];
uint32_t ppu_dsp_tline[PPU_H + 1];      /* (with the clock) each slow line's time */
static const uint8_t *fp[8];            /* the run's matrix channels, by field */
static uint8_t ff[8], fch[8];
static int fk;

static int fast_len(const PpuInput *in, PpuFrame *F, int line)
{
    if (F->s.dirty) return 0;
    int n = PPU_H + 1 - line;
    if (F->logi < in->nlog) {
        int e = in->log[F->logi].line - line;
        if (e < n) n = e;
    }
    if (F->segi < in->noamseg) {
        int e = in->oamseg[F->segi].line - line;
        if (e < n) n = e;
    }
    fk = 0;
    for (int ch = 0; ch < 8 && n > 0; ch++) {
        Hdma *c = &F->h[ch];
        if (!c->active) continue;
        if (c->count < 2) return 0;
        if (c->count - 1 < n) n = c->count - 1;
        if (!c->dotransfer) continue;
        if (!c->repeat || c->m7i < 0 || c->n != 2) return 0;
        uint32_t len = c->ind ? c->ilen : c->tlen, a = c->ind ? c->indirect : c->table;
        int t = (int)((len - 1) / 2), u = (int)((0xFFFF - (a & 0xFFFF)) / 2);
        if (!len) return 0;
        if (t < n) n = t;
        if (u < n) n = u;
        int f = F_M7 + c->m7i, j = fk++;
        while (j > 0 && ff[j - 1] > f) { ff[j] = ff[j - 1]; fp[j] = fp[j - 1]; fch[j] = fch[j - 1]; j--; }
        if (j > 0 && ff[j - 1] == f) return 0;       /* two channels, one field */
        ff[j] = (uint8_t)f;
        fp[j] = c->ind ? c->ip : c->tp;
        fch[j] = (uint8_t)ch;
    }
    if (n > 0 && so + (uint32_t)n * (4 + 6 * (uint32_t)fk) + 8 > so_end) return 0;
    return n > 0 ? n : 0;
}

/* n lines of the run fast_len found: the values, the SET records of those
 * that changed, the line count; then the frame loop's state as n lines of
 * the generic code would leave it. */
__attribute__((noinline)) static void fast_run(PpuFrame *F, int n)
{
    uint16_t *o = so;
    int32_t *snt = sent;
    int pend = pending, k = fk;
    for (int l = 0; l < n; l++) {
        for (int j = 0; j < k; j++) {
            const uint8_t *q = fp[j];
            int32_t v = (int16_t)(q[1] << 8 | q[0]);
            fp[j] = q + 2;
            int f = ff[j];
            if (v != snt[f]) {
                if (pend) { o[0] = (uint16_t)(OP_LINE << 8); o[1] = (uint16_t)pend; o += 2; pend = 0; }
                snt[f] = v;
                o[0] = (uint16_t)(OP_SET << 8); o[1] = (uint16_t)f;
                o[2] = (uint16_t)((uint32_t)v >> 8); o[3] = (uint16_t)(v & 0xFF);
                o += 4;
            }
        }
        pend++;
    }
    so = o;
    pending = pend;
    Regs *s = &F->s;
    int lastch = -1;
    uint8_t lasthi = s->m7old;
    for (int j = 0; j < k; j++) {
        Hdma *c = &F->h[fch[j]];
        const uint8_t *q = fp[j] - 2;               /* the last unit */
        s->m7[c->m7i] = (int16_t)(q[1] << 8 | q[0]);
        s->r[c->breg] = q[1];
        if (fch[j] > lastch) { lastch = fch[j]; lasthi = q[1]; }
        uint32_t b = 2u * (uint32_t)n;
        if (c->ind) { c->ip += b; c->ilen -= b; c->indirect += b; }
        else { c->tp += b; c->tlen -= b; c->table += b; }
    }
    s->m7old = lasthi;
    for (int ch = 0; ch < 8; ch++)
        if (F->h[ch].active) F->h[ch].count = (uint8_t)(F->h[ch].count - n);
}

uint16_t *ppu_dsp_frame_slots(const PpuInput *in, uint16_t *out, uint16_t *end)
{
    static PpuFrame F;
    so = out;
    so_end = end;
    so_over = 0;
    out_put = slot_put;
    pending = 0;
    first = 1;
    uint32_t (*clk)(void) = ppu_dsp_clock;
    uint32_t t = clk ? clk() : 0, u;
    slot_put(15);
    ppu_frame_start(in, &F);
    if (clk) { u = clk(); ppu_dsp_t[0] += u - t; t = u; }
    for (int line = 0; line <= PPU_H;) {
        int n = line >= 2 ? fast_len(in, &F, line) : 0;
        if (n > 0) {
            fast_run(&F, n);
            ppu_dsp_fast_lines += (uint32_t)n;
            line += n;
            if (clk) { u = clk(); ppu_dsp_t[1] += u - t; t = u; }
            continue;
        }
        ppu_frame_line(in, &F, line);
        if (line >= 1) dsp_row(in, &F.s, line - 1, 0);
        if (clk) { u = clk(); ppu_dsp_t[2] += u - t; ppu_dsp_tline[line] = u - t; t = u; }
        line++;
    }
    flush();
    slot_put(OP_END);
    if (clk) { u = clk(); ppu_dsp_t[3] += u - t; }
    return so_over ? 0 : so;
}

void ppu_dsp_frame(const PpuInput *in, PpuDspPut put)
{
    out_put = put;
    pending = 0;
    first = 1;
    put(15);
    ppu_render_rows(in, 0, dsp_row);
    flush();
    put(OP_END);
}

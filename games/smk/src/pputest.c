/* PPUTEST: the DSP PPU (falcon/dsp/ppu.asm) on the Falcon, milestone a:
 * the program beyond the boot image goes up with LOAD, random VRAM goes up
 * in random pieces (then random short overwrites, which exercise the packed
 * neighbours), and every word is read back and compared.
 * Freestanding: falcon/pputest.s provides the glue. */
#include <stdint.h>
#include "ppu_internal.h"
#include "smkx_prof.h"
#include "m7_plan.h"
#include "ppu_dsp.h"

extern void pt_print(const char *s);
extern void pt_hex(uint32_t v);
extern const uint8_t ppu_ext[], ppu_ext_end[];
extern int pt_record(uint16_t *buf, uint32_t bytes);
extern int pt_record_done(void);
extern void pt_play(const uint16_t *buf, uint32_t bytes);
extern int pt_play_done(void);

static uint16_t vram[32768];
static uint32_t rng = 0x2468ACE1;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

#define HP ((volatile uint8_t *)0xFFFFA200)

static void put(uint32_t w)
{
    while (!(HP[2] & 2)) {}
    HP[5] = (uint8_t)(w >> 16);
    HP[6] = (uint8_t)(w >> 8);
    HP[7] = (uint8_t)w;
}

static uint32_t get(void)
{
    while (!(HP[2] & 1)) {}
    return ((uint32_t)HP[5] << 16) | ((uint32_t)HP[6] << 8) | HP[7];
}

/* 1/38400 s -> us, without the 32-bit overflow of units * 1000000 */
static uint32_t us_of(uint32_t units) { return units * 625u / 24u; }

static uint32_t w24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

static void upload(uint32_t first, uint32_t count)
{
    put(5);
    put(first);
    put(count);
    for (uint32_t i = 0; i < count; i++) put(vram[first + i]);
}

/* ---- frames: the DSP's FRAME against the reference ppu_render --------- */

static uint8_t vbytes[65536], lut[256];
static uint8_t chunky[PPU_W * PPU_H];
#define LW 64                   /* words per recorded line: the 64 plane words */
static uint16_t refp[PPU_W * PPU_H / 4], fcap[LW * PPU_H + 256 + 64];   /* + the histogram */
static uint8_t io[0x6000], oam[PPU_OAM_BYTES];
static PpuWrite logw[256];
static PpuOamSeg segs[2];
static uint16_t bgofs[8], m7r[6], m7ofs[2];
static uint32_t hist[256];
static uint32_t stream[20000];
static int nstream;

static void to_buf(uint32_t w) { if (nstream < 20000) stream[nstream++] = w; }

/* ---- the SSI input: commands by DMA playback (ppu.asm's SSION) -------- */
static int via_ssi;                     /* frame_test sends by DMA */
static uint16_t sbuf[70000];            /* 16-bit slots, played in order */
static int nslot;
static void s24(uint32_t w) { sbuf[nslot++] = (uint16_t)(w >> 8); sbuf[nslot++] = (uint16_t)(w & 0xFF); }
static void s16(uint32_t w) { sbuf[nslot++] = (uint16_t)w; }

/* Plays sbuf (padded with no-op commands to whole crossbar frames of
 * eight slots) and waits until it has gone out; 0 on success. */
static int ssi_send(void)
{
    while (nslot & 7) s24(0);           /* command 0: nothing */
    while (!pt_play_done()) {}          /* the window before */
    pt_play(sbuf, (uint32_t)nslot * 2);
    uint32_t spin = 0;
    while (!pt_play_done() && ++spin < 30000000) {}
    nslot = 0;
    if (spin >= 30000000) {
        pt_print("PPUTEST ssi play timeout\n");
        return 1;
    }
    return 0;
}

enum { K_BG, K_OBJ, K_M7 };

/* A random frame of the given kind, with mid-frame writes. */
static void make_frame(int kind, PpuInput *in)
{
    for (int i = 0; i < 0x6000; i++) io[i] = 0;
    io[0x100] = 0x0F;
    io[0x101] = (uint8_t)rnd();
    int mode = kind == K_M7 ? ((rnd() & 1) ? 7 : 1) : (int)(rnd() & 1);
    io[0x105] = (uint8_t)(mode | (mode == 1 && (rnd() & 1) ? 8 : 0));
    for (int k = 0; k < 4; k++) io[0x107 + k] = (uint8_t)rnd();
    io[0x10B] = (uint8_t)rnd();
    io[0x10C] = (uint8_t)rnd();
    static const uint8_t sels[6] = {0x00, 0x80, 0xC0, 0x01, 0x02, 0xC3};
    io[0x11A] = sels[rnd() % 6];
    io[0x12C] = (uint8_t)(kind == K_M7 ? (1 | (rnd() & 0x17)) : (rnd() & 0x0F) | (kind == K_OBJ ? 0x10 : 0));
    for (int k = 0; k < 8; k++) bgofs[k] = (uint16_t)(rnd() & 0x3FF);
    for (int k = 0; k < 4; k++) m7r[k] = (uint16_t)((rnd() & 0x3FF) - 0x200);
    m7r[4] = (uint16_t)(rnd() & 0x1FFF);
    m7r[5] = (uint16_t)(rnd() & 0x1FFF);
    m7ofs[0] = (uint16_t)(rnd() & 0x1FFF);
    m7ofs[1] = (uint16_t)(rnd() & 0x1FFF);
    for (int i = 0; i < PPU_OAM_BYTES; i++) oam[i] = (uint8_t)rnd();
    for (int i = 0; i < 128; i++) oam[i * 4 + 1] = (uint8_t)(rnd() % 240 - 16);
    if (rnd() & 1) {                    /* windows (TMW) in half the frames */
        io[0x12E] = (uint8_t)(rnd() & 0x1F);
        io[0x123] = (uint8_t)rnd();
        io[0x124] = (uint8_t)rnd();
        io[0x125] = (uint8_t)(rnd() & 0x0F);
        for (int k = 0; k < 4; k++) io[0x126 + k] = (uint8_t)rnd();
        io[0x12A] = (uint8_t)rnd();
        io[0x12B] = (uint8_t)(rnd() & 3);
    }
    /* mid-frame writes, ascending lines */
    static const uint16_t bgregs[] = {0x2100, 0x2105, 0x2107, 0x2108, 0x2109, 0x210A, 0x210B,
                                      0x210C, 0x210D, 0x210E, 0x210F, 0x2110, 0x2111, 0x2112,
                                      0x2113, 0x2114, 0x212C, 0x2126, 0x2127, 0x2128,
                                      0x2129, 0x212E, 0x2123};
    static const uint16_t m7regs[] = {0x211B, 0x211C, 0x211D, 0x211E, 0x211F, 0x2120,
                                      0x210D, 0x210E, 0x211A, 0x212C, 0x2100, 0x2105,
                                      0x2126, 0x2127, 0x212E, 0x2123, 0x2125};
    int n = (int)(rnd() % 40), line = 1;
    for (int i = 0; i < n; i++) {
        line += (int)(rnd() % 12);
        if (line > PPU_H) { n = i; break; }
        uint16_t reg = kind == K_M7 ? m7regs[rnd() % (sizeof m7regs / sizeof m7regs[0])]
                                    : bgregs[rnd() % (sizeof bgregs / sizeof bgregs[0])];
        uint8_t v = (uint8_t)rnd();
        if (reg == 0x2100) v = (rnd() & 7) ? 0x0F : 0x8F;
        if (reg == 0x2105) v = kind == K_M7 ? ((rnd() & 1) ? 7 : 1) : (uint8_t)((rnd() & 1) | ((rnd() & 1) ? 8 : 0));
        if (reg == 0x212C) v = kind == K_M7 ? (uint8_t)(1 | (v & 0x17)) : (uint8_t)(v & (kind == K_OBJ ? 0x1F : 0x0F));
        if (reg == 0x211A) v = sels[rnd() % 6];
        if (reg == 0x212E) v &= 0x1F;
        logw[i].line = (uint16_t)line;
        logw[i].reg = reg;
        logw[i].value = v;
        logw[i].pad = 0;
    }
    /* a second OAM segment half way down (as SMK's split screen makes) */
    int nseg = kind != K_BG && (rnd() & 1);
    if (nseg) {
        segs[0].line = 0;
        for (int i = 0; i < PPU_OAM_BYTES; i++) segs[0].oam[i] = oam[i];
        segs[1].line = (uint16_t)(60 + rnd() % 100);
        for (int i = 0; i < PPU_OAM_BYTES; i++) segs[1].oam[i] = (uint8_t)rnd();
        for (int i = 0; i < 128; i++) segs[1].oam[i * 4 + 1] = (uint8_t)(rnd() % 240 - 16);
    }
    for (int i = 0; i < 256; i++) hist[i] = 0;
    for (unsigned k = 0; k < sizeof *in; k++) ((uint8_t *)in)[k] = 0;
    in->vram = vbytes;
    in->cgram = io;             /* unused: the slots come from lut */
    in->oam = oam;
    in->io = io;
    in->bgofs = bgofs;
    in->m7 = m7r;
    in->m7ofs = m7ofs;
    in->log = logw;
    in->nlog = n;
    in->map = lut;
    in->hist = hist;
    in->oamseg = segs;
    in->noamseg = nseg ? 2 : 0;
}

#ifdef PPU_DEBUG
static int m7row;
static PpuM7Line m7l;
static void m7grab(const PpuInput *in, const Regs *r, int row, uint8_t *o)
{
    (void)in; (void)o;
    if (row == m7row) ppu_mode7_setup(r, row, &m7l);
}
#endif

static int frame_test(int kind, int frames, const char *name)
{
    int bad = 0, shown = 0;
    uint32_t dsp_units = 0;
    for (int f = 0; f < frames; f++) {
        PpuInput in;
        make_frame(kind, &in);
        ppu_render(&in, chunky);
        ppu_planar(chunky, refp);
        nstream = 0;
        ppu_dsp_frame(&in, to_buf);
        if (pt_record(fcap, (LW * PPU_H + 256) * 2)) return 1;
        uint32_t t0 = prof_now();
        if (via_ssi) {
            nslot = 0;
            for (int i = 0; i < nstream; i++) s24(stream[i]);
            while (nslot & 7) s24(0);
            while (!pt_play_done()) {}      /* the last frame's padding */
            pt_play(sbuf, (uint32_t)nslot * 2);
            nslot = 0;
        } else
            for (int i = 0; i < nstream; i++) put(stream[i]);
        uint32_t spin = 0;
        while (!pt_record_done() && ++spin < 30000000) {}
        if (spin >= 30000000) {
            /* the DSP stopped: where (FROW, MNSEG, the planner's x, lo, hi,
             * end, SEGB's end) */
            static const uint32_t at[7] = {0x2EFA, 0x2EF9, 0x3300 + 3 * 272 + 200 + 10,
                                          0x3300 + 3 * 272 + 200 + 11, 0x3300 + 3 * 272 + 200 + 12,
                                          0x3300 + 3 * 272 + 200 + 13, 0x3300 + 3 * 272 + 200 + 14};
            pt_print("PPUTEST timeout frame ");
            pt_hex((uint32_t)f);
            for (int k = 0; k < 7; k++) {
                put(8);
                put(0x10000u + at[k]);
                uint32_t sp2 = 0;
                while (!(HP[2] & 1) && ++sp2 < 2000000) {}
                pt_print(" ");
                if (HP[2] & 1) pt_hex(get()); else pt_print("-");
            }
            pt_print(" mode ");
            pt_hex(io[0x105]);
            pt_print(" sel ");
            pt_hex(io[0x11A]);
            pt_print("\n");
            return 1;
        }
        dsp_units += prof_now() - t0;
#ifdef PPU_M7ONLY
        {
            put(8);
            put(0x10000u + 0x2EFFu);
            uint32_t badrow = get();
            if (badrow) {
                pt_print("PPUTEST m7 bad segments row+1 ");
                pt_hex(badrow);
                for (int k = 0; k < 1 + 72; k++) {
                    put(8);
                    put(0x10000u + 0x3300u + 3u * 272u + (uint32_t)k);
                    pt_print(" ");
                    pt_hex(get());
                }
                pt_print(" sel ");
                pt_hex(io[0x11A]);
                pt_print("\n");
            }
        }
#endif
        int fb = 0, first = -1;
        for (int i = 0; i < PPU_W * PPU_H / 4; i++)
            if (fcap[i / 64 * LW + i % 64] != refp[i]) { fb++; if (first < 0) first = i; }
        static uint32_t dh[256];
        for (int i = 0; i < 256; i++) dh[i] = 0;
        for (int i = 0; i < 256; i++) dh[i] = (uint32_t)fcap[LW * PPU_H + i] * 16u;
        int hb = 0;
        for (int i = 0; i < 256; i++) hb += dh[i] != hist[i];
        if (hb) {
            pt_print("PPUTEST ");
            pt_print(name);
            pt_print(" frame ");
            pt_hex((uint32_t)f);
            pt_print(" histogram counts differing ");
            pt_hex((uint32_t)hb);
            pt_print("\n");
            fb += hb;
        }
        bad += fb;
        if (fb && shown++ < 4) {
            pt_print("PPUTEST ");
            pt_print(name);
            pt_print(" frame ");
            pt_hex((uint32_t)f);
            pt_print(" words ");
            pt_hex((uint32_t)fb);
            pt_print(" first row ");
            pt_hex((uint32_t)first / 64);
            pt_print(" word ");
            pt_hex((uint32_t)first % 64);
            pt_print(" got ");
            pt_hex(fcap[first / 64 * LW + first % 64]);
            pt_print(" want ");
            pt_hex(refp[first]);
            pt_print(" mode ");
            pt_hex(io[0x105]);
            pt_print(" tm ");
            pt_hex(io[0x12C]);
            pt_print(" writes ");
            pt_hex((uint32_t)in.nlog);
            pt_print(" stream ");
            pt_hex((uint32_t)nstream);
            pt_print("\n");
#ifdef PPU_DEBUG
            if (shown == 1) {
                pt_print("PPUTEST  stream");
                for (int k = 0; k < nstream && k < 120; k++) { pt_print(" "); pt_hex(stream[k]); }
                pt_print("\nPPUTEST  rows");
                for (int r = 0; r < PPU_H; r++) {
                    int d = 0;
                    for (int w = 0; w < 64; w++) d += fcap[r * LW + w] != refp[r * 64 + w];
                    pt_print(d ? "x" : ".");
                }
                if (io[0x105] == 7 || io[0x105] == 1) {
                    int fr = -1;
                    for (int r = 0; r < PPU_H && fr < 0; r++)
                        for (int w = 0; w < 64; w++) if (fcap[r * LW + w] != refp[r * 64 + w]) { fr = r; break; }
                    put(6); put(2); put(0x3300u + 3u * 272u + 80u); put(1); put((uint32_t)fr);
                    put(10); put(1);
                    for (int i = 0; i < nstream; i++) put(stream[i]);
                    put(4); get();
                    put(10); put(0);
                    pt_print("\nPPUTEST  dsp segs row ");
                    pt_hex((uint32_t)fr);
                    for (int k = 0; k < 1 + 6 * 4; k++) { put(8); put(0x10000u + 0x3300u + 3u * 272u + (uint32_t)k); pt_print(" "); pt_hex(get()); }
                    pt_print("\nPPUTEST  host segs");
                    m7row = fr;
                    ppu_render_rows(&in, 0, m7grab);
                    M7Seg sg[M7_MAX_SEGS];
                    int ns = m7_plan(&m7l, sg);
                    pt_hex((uint32_t)ns);
                    for (int k = 0; k < ns && k < 4; k++) {
                        pt_print(" | "); pt_hex(sg[k].kind); pt_print(" "); pt_hex(sg[k].count);
                        pt_print(" "); pt_hex((uint32_t)sg[k].u & 0xFFFFFF); pt_print(" "); pt_hex((uint32_t)sg[k].v & 0xFFFFFF);
                        pt_print(" "); pt_hex((uint32_t)sg[k].du & 0xFFFFFF); pt_print(" "); pt_hex((uint32_t)sg[k].dv & 0xFFFFFF);
                    }
                    pt_print(" sel "); pt_hex(m7l.sel);
                    pt_print(" vx "); pt_hex((uint32_t)m7l.vx); pt_print(" vy "); pt_hex((uint32_t)m7l.vy);
                    pt_print(" a "); pt_hex((uint32_t)m7l.a); pt_print(" c "); pt_hex((uint32_t)m7l.c);
                }                pt_print("\nPPUTEST  shadow");
                for (int k = 0; k < 26; k++) { put(8); put(0x10000u + 0x2DA0u + (uint32_t)k); pt_print(" "); pt_hex(get()); }
                pt_print("\n");
            }
#endif
            for (int k = 0; k < in.nlog; k++) {
                pt_print("PPUTEST  write line ");
                pt_hex(logw[k].line);
                pt_print(" reg ");
                pt_hex(logw[k].reg);
                pt_print(" value ");
                pt_hex(logw[k].value);
                pt_print("\n");
            }
        }
    }
    pt_print("PPUTEST ");
    pt_print(name);
    pt_print(" frames ");
    pt_hex((uint32_t)frames);
    pt_print(" differing words ");
    pt_hex((uint32_t)bad);
    pt_print(" us per frame ");
    pt_hex(us_of(dsp_units) / (uint32_t)frames);
    pt_print("\n");
    return bad != 0;
}

#ifdef PT_SNAP
#define PAL_SNAP_BYTES_PT (96 + 48 + 256 + 4 + 2)   /* palette.h: PAL_SNAP_BYTES */
/* A runtime snapshot (snes_ppu.h: the SNAP.BIN layout) timed on the DSP:
 * as it is, without sprites, without BG1, with neither; lines not sent
 * (OPTIONS 1), a PING after the last. HDMA tables in ROM read as zero
 * here (SMK's live in WRAM). */
extern const uint8_t pt_snap[], pt_snap_end[];
static uint8_t zrom[16];
static uint32_t be32(const uint8_t *q) { return (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]; }
static uint16_t be16(const uint8_t *q) { return (uint16_t)(q[0] << 8 | q[1]); }
static uint8_t snap_abus(uint32_t a, void *ctx)
{
    const uint8_t *wram = ctx;
    uint32_t bank = a >> 16, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) return wram[(bank - 0x7E) << 16 | off];
    if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && off < 0x2000) return wram[off];
    return zrom[0];
}
static const uint8_t *snap_aptr(uint32_t a, void *ctx, uint32_t *len)
{
    const uint8_t *wram = ctx;
    uint32_t bank = a >> 16, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) { *len = 0x10000 - off; return wram + ((bank - 0x7E) << 16 | off); }
    if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && off < 0x2000) { *len = 0x2000 - off; return wram + off; }
    *len = 0;
    return 0;
}
static PpuWrite slog[256];
static PpuOamSeg sseg[PPU_OAMSEG_MAX];
static uint16_t sbg[8], sm7[6], sm7o[2];

static uint32_t pt_clock(void) { return prof_now(); }

#ifdef PT_PROF
/* scripts/dsp_profile.py --start: profiling from here on (the frames only,
 * without the snapshot's VRAM upload) */
__attribute__((noinline)) void pt_prof_start(void) { __asm__ volatile(""); }
#endif

static void snap_bench(void)
{
    const uint8_t *q = pt_snap + 12;
    const uint8_t *sv = q; q += 65536;
    const uint8_t *scg = q; q += 512;
    const uint8_t *soam = q; q += 544;
    const uint8_t *sio = q; q += 0x6000;
    for (int i = 0; i < 8; i++) { sbg[i] = be16(q); q += 2; }
    for (int i = 0; i < 6; i++) { sm7[i] = be16(q); q += 2; }
    for (int i = 0; i < 2; i++) { sm7o[i] = be16(q); q += 2; }
    uint32_t nlog = be32(q); q += 4;
    for (int i = 0; i < 256; i++) { slog[i].line = be16(q); slog[i].reg = be16(q + 2); slog[i].value = q[4]; q += 6; }
    const uint8_t *swram = q; q += 131072 + 2048 + 28672 + 2 * PAL_SNAP_BYTES_PT;
    uint32_t nseg = be32(q); q += 4;
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
        sseg[i].line = be16(q);
        for (int k = 0; k < PPU_OAM_BYTES; k++) sseg[i].oam[k] = q[4 + k];
        q += 4 + PPU_OAM_BYTES;
    }
    for (uint32_t i = 0; i < 32768; i++) vram[i] = (uint16_t)(sv[2 * i] | sv[2 * i + 1] << 8);
    upload(0, 32768);
    PpuInput in;
    for (unsigned k = 0; k < sizeof in; k++) ((uint8_t *)&in)[k] = 0;
    in.vram = sv; in.cgram = scg; in.oam = soam; in.io = sio;
    in.bgofs = sbg; in.m7 = sm7; in.m7ofs = sm7o;
    in.abus = snap_abus; in.aptr = snap_aptr; in.ctx = (void *)swram;
    in.log = slog; in.nlog = (int)(nlog > 256 ? 256 : nlog);
    in.map = lut; in.hist = hist;
    in.oamseg = sseg; in.noamseg = (int)(nseg > PPU_OAMSEG_MAX ? PPU_OAMSEG_MAX : nseg);
    /* the 68030's side: the stream, then without HDMA, then with one OAM
     * segment */
    {
        static uint8_t io2[0x6000];
        for (int i = 0; i < 0x6000; i++) io2[i] = sio[i];
        uint32_t t0 = prof_now();
        nstream = 0;
        ppu_dsp_frame(&in, to_buf);
        uint32_t t1 = prof_now();
        io2[0x420C - 0x2000] = 0;
        in.io = io2;
        nstream = 0;
        ppu_dsp_frame(&in, to_buf);
        uint32_t t2 = prof_now();
        in.io = sio;
        int ns = in.noamseg;
        in.noamseg = 0;
        nstream = 0;
        ppu_dsp_frame(&in, to_buf);
        uint32_t t3 = prof_now();
        in.noamseg = ns;
        {   /* the SSI slot builder (fast path): whole, one OAM segment, parts */
            static uint16_t slots[40000];
            extern uint32_t (*ppu_dsp_clock)(void);
            extern uint32_t ppu_dsp_t[4];
            uint32_t u0 = prof_now();
            ppu_dsp_frame_slots(&in, slots, slots + 40000);
            uint32_t u1 = prof_now();
            int ns2 = in.noamseg;
            in.noamseg = 0;
            ppu_dsp_frame_slots(&in, slots, slots + 40000);
            uint32_t u2 = prof_now();
            in.noamseg = ns2;
            pt_print("PPUTEST snap 68030 slots us: whole ");
            pt_hex(us_of(u1 - u0));
            pt_print(" one OAM segment ");
            pt_hex(us_of(u2 - u1));
            pt_print("\n");
            ppu_dsp_clock = pt_clock;
            for (int k = 0; k < 4; k++) ppu_dsp_t[k] = 0;
            ppu_dsp_frame_slots(&in, slots, slots + 40000);
            ppu_dsp_clock = 0;
            pt_print("PPUTEST snap slots parts us: start ");
            pt_hex(us_of(ppu_dsp_t[0]));
            pt_print(" fast ");
            pt_hex(us_of(ppu_dsp_t[1]));
            pt_print(" slow ");
            pt_hex(us_of(ppu_dsp_t[2]));
            pt_print(" end ");
            pt_hex(us_of(ppu_dsp_t[3]));
            pt_print("\n");
            extern uint32_t ppu_dsp_tline[];
            pt_print("PPUTEST snap slow lines (line us):");
            for (int l = 0; l <= PPU_H; l++)
                if (ppu_dsp_tline[l]) { pt_print(" "); pt_hex((uint32_t)l); pt_print(":"); pt_hex(us_of(ppu_dsp_tline[l])); }
            pt_print("\n");
        }
        pt_print("PPUTEST snap 68030 stream us: whole ");
        pt_hex(us_of(t1 - t0));
        pt_print(" no HDMA ");
        pt_hex(us_of(t2 - t1));
        pt_print(" one OAM segment ");
        pt_hex(us_of(t3 - t2));
        pt_print("\n");
    }
    nstream = 0;
    ppu_dsp_frame(&in, to_buf);
    static uint32_t orig[20000];
    int n0 = nstream;
    for (int i = 0; i < n0; i++) orig[i] = stream[i];
    static const char *names[4] = {"as it is", "no sprites", "no BG1", "neither"};
#ifdef PT_PROF
    const int nv = 1;           /* the frame as it is (host port, then SSI) */
#else
    const int nv = 4;
#endif
    for (int v = 0; v < nv; v++) {
        /* TM (field 25) values masked in the stream; OAM records emptied */
        int n = 0;
        for (int i = 0; i < n0; i++) {
            uint32_t w = orig[i];
            stream[n++] = w;
            if (i + 1 < n0 && w == (2u << 16 | 25u)) {
                uint32_t t = orig[++i];
                if (v & 1) t &= ~0x10u;
                if (v & 2) t &= ~0x01u;
                stream[n++] = t;
            } else if (w == 3u << 16 && (v & 1)) {
                stream[n++] = 0;                /* no sprites */
                i += 1 + 2 * (int)orig[i + 1];
            } else if (w == 3u << 16) {
                int cnt = 1 + 2 * (int)orig[i + 1];
                for (int k = 0; k < cnt; k++) stream[n++] = orig[++i];
            }
        }
        put(10);
        put(1);
#ifdef PT_PROF
        pt_prof_start();
#endif
        uint32_t t0 = prof_now();
        for (int i = 0; i < n; i++) put(stream[i]);
        put(4);
        get();
        uint32_t us = us_of(prof_now() - t0);
        put(10);
        put(0);
        if (v == 0) {                           /* the same with the SSI */
            pt_record(fcap, (LW * PPU_H + 256) * 2);
            uint32_t t1 = prof_now();
            for (int i = 0; i < n; i++) put(stream[i]);
            while (!pt_record_done()) {}
            uint32_t dt = prof_now() - t1;
            ppu_render(&in, chunky);            /* the reference, planes */
            ppu_planar(chunky, refp);
            int d = 0;
            for (int i = 0; i < PPU_W * PPU_H / 4; i++) d += fcap[i / 64 * LW + i % 64] != refp[i];
            pt_print("PPUTEST snap check differing words ");
            pt_hex((uint32_t)d);
            pt_print("\n");
            pt_print("PPUTEST snap with the SSI: us per frame ");
            pt_hex(us_of(dt));
            pt_print(" sprites sent ");
            for (int i = 0; i < n0; i++) if (orig[i] == 3u << 16) { pt_hex(orig[i + 1]); pt_print(" "); }
            pt_print("\n");
        }
        pt_print("PPUTEST snap ");
        pt_print(names[v]);
        pt_print(": us per frame ");
        pt_hex(us);
        pt_print(" stream words ");
        pt_hex((uint32_t)n);
        pt_print("\n");
    }
}
#endif

/* The same frames with every command by DMA playback: SSION over the host
 * port, new random VRAM (VRAMSET) and colour tables (COLOUR) over the
 * SSI, then frames whose streams go by DMA, then SSIOFF and a PING over
 * the host port again. */
static int ssi_tests(void)
{
    put(3);                                             /* SSION */
    if (get() != 1) {
        pt_print("PPUTEST ssi: no SSION\n");
        return 1;
    }
    for (int i = 0; i < 32768; i++) {
        vram[i] = (uint16_t)rnd();
        vbytes[2 * i] = (uint8_t)vram[i];
        vbytes[2 * i + 1] = (uint8_t)(vram[i] >> 8);
    }
    for (int i = 0; i < 256; i++) lut[i] = (uint8_t)(rnd() & 15);
    nslot = 0;
    s24(5); s24(0); s24(32768);                         /* VRAMSET, all of it */
    for (int i = 0; i < 32768; i++) s16(vram[i]);
    s24(2);                                             /* COLOUR */
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 256; i++) s16(((uint32_t)(lut[i] >> p) & 1) << 8);
    if (ssi_send()) return 1;
    via_ssi = 1;
    int r = frame_test(K_BG, 8, "ssi bg");
    r |= frame_test(K_OBJ, 8, "ssi obj");
    r |= frame_test(K_M7, 8, "ssi m7");
    via_ssi = 0;
    while ((nslot + 2) & 7) s24(0);                     /* SSIOFF last: after */
    s24(9);                                             /* it nothing is read */
    if (ssi_send()) return 1;
    put(4);                                             /* PING, host port */
    uint32_t ping = get();
    pt_print("PPUTEST ssi back on the host port, ping ");
    pt_hex(ping);
    pt_print("\n");
    return r | (ping != 0x505055);
}

static int frame_tests(void)
{
    for (int i = 0; i < 32768; i++) {
        vbytes[2 * i] = (uint8_t)vram[i];
        vbytes[2 * i + 1] = (uint8_t)(vram[i] >> 8);
    }
    for (int i = 0; i < 256; i++) lut[i] = (uint8_t)(rnd() & 15);
    put(2);
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 256; i++) put(((uint32_t)(lut[i] >> p) & 1) << 8);
#ifdef PT_SNAP
    snap_bench();
    return 0;
#endif
#ifdef PPU_M7ONLY
    int r = frame_test(K_M7, 16, "m7");
#else
    int r = frame_test(K_BG, 16, "bg");
    r |= frame_test(K_OBJ, 16, "obj");
    r |= frame_test(K_M7, 16, "m7");
    r |= ssi_tests();
#endif
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

int pputest_main(void)
{
    pt_print("PPUTEST start\n");
    for (const uint8_t *e = ppu_ext; e < ppu_ext_end;) {
        uint32_t n = w24(e + 6);
        put(6);
        put(w24(e));
        put(w24(e + 3));
        put(n);
        for (uint32_t i = 0; i < n; i++) put(w24(e + 9 + i * 3));
        e += 9 + n * 3;
    }
    pt_print("PPUTEST extension bytes ");
    pt_hex((uint32_t)(ppu_ext_end - ppu_ext));
    put(8);
    put(0x3000);
    pt_print(" P:$3000 = ");
    pt_hex(get());
    pt_print("\n");
    put(4);
    uint32_t ping = get();
    pt_print("PPUTEST ping ");
    pt_hex(ping);
    pt_print("\n");
    if (ping != 0x505055) {
        pt_print("PPUTEST bad ping\n");
        return 1;
    }
    pt_print("PPUTEST ping ok\n");

    {
        uint16_t sr;
        __asm__ volatile ("move.w %%sr,%0" : "=d"(sr));
        pt_print("PPUTEST sr ");
        pt_hex(sr);
        pt_print(" hz200 ");
        pt_hex(*(volatile uint32_t *)0x4BA);
        pt_print("\n");
    }
#ifdef PT_PROF
    return frame_tests();       /* profiling the snapshot frame: nothing else */
#endif
    uint32_t up0 = prof_now(), hz0 = *(volatile uint32_t *)0x4BA;
    pt_print("PPUTEST upload\n");
    for (uint32_t i = 0; i < 32768; i++) vram[i] = (uint16_t)rnd();
    for (uint32_t pos = 0; pos < 32768;) {
        uint32_t len = 1 + rnd() % 700;
        if (pos + len > 32768) len = 32768 - pos;
        upload(pos, len);
        pos += len;
    }
    pt_print("PPUTEST upload took (1/38400 s) ");
    pt_hex(prof_now() - up0);
    pt_print(" hz200 ticks ");
    pt_hex(*(volatile uint32_t *)0x4BA - hz0);
    pt_print("\n");
    pt_print("PPUTEST overwrite\n");
    for (int t = 0; t < 400; t++) {             /* short overwrites anywhere */
        uint32_t a = rnd() % 32768, len = 1 + rnd() % 5;
        if (a + len > 32768) len = 32768 - a;
        for (uint32_t i = 0; i < len; i++) vram[a + i] = (uint16_t)rnd();
        upload(a, len);
    }

    put(4);
    {
        uint32_t spin = 0;
        while (!(HP[2] & 1) && ++spin < 4000000) {}
        pt_print((HP[2] & 1) ? "PPUTEST ping after upload answered\n" : "PPUTEST ping after upload: no answer\n");
        if (HP[2] & 1) get();
    }
    pt_print("PPUTEST read back\n");
    {   /* one word from each half first, with a timeout */
        static const uint32_t probe[4] = {0x0000, 0x3FFF, 0x4000, 0x7FFF};
        for (int i = 0; i < 4; i++) {
            put(7);
            put(probe[i]);
            put(1);
            uint32_t spin = 0;
            while (!(HP[2] & 1) && ++spin < 2000000) {}
            pt_print("PPUTEST probe ");
            pt_hex(probe[i]);
            if (!(HP[2] & 1)) { pt_print(" no answer\n"); return 1; }
            pt_print(" got ");
            pt_hex(get());
            pt_print(" want ");
            pt_hex(vram[probe[i]]);
            pt_print("\n");
        }
    }
    uint32_t bad = 0, first_bad = 0xFFFFFFFF, phase_bad[3] = {0, 0, 0};
    for (uint32_t pos = 0; pos < 32768; pos += 1024) {
        put(7);
        put(pos);
        put(1024);
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t w = get();
            if (w != vram[pos + i]) {
                if (!bad) first_bad = pos + i;
                if (bad < 12) {
                    pt_print("PPUTEST bad ");
                    pt_hex(pos + i);
                    pt_print(" got ");
                    pt_hex(w);
                    pt_print(" want ");
                    pt_hex(vram[pos + i]);
                    pt_print("\n");
                }
                if (pos + i >= 0x4000) phase_bad[(pos + i - 0x4000) % 3]++;
                bad++;
            }
        }
    }
    pt_print("PPUTEST vram words 00008000 mismatches ");
    pt_hex(bad);
    pt_print(" first ");
    pt_hex(first_bad);
    pt_print(" by phase ");
    pt_hex(phase_bad[0]);
    pt_print(" ");
    pt_hex(phase_bad[1]);
    pt_print(" ");
    pt_hex(phase_bad[2]);
    pt_print("\n");
    if (bad) return 1;
    return frame_tests();
}

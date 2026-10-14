/* Host check of the true-colour renderer's design: for each SNAP.BIN the
 * frame through dsp_plan() (DspPlan.tc) and the model (tools/tc_model.c)
 * against ppu_render_565(); the conversions the model read must be ones
 * the plan asked for (DspPlan.fmt). Prints what the DSP would do.
 *
 *   build/tc_snap rom.sfc SNAP.BIN... [-pack FILE] [-planhash]
 *
 * -pack writes the frames for TCTEST (tools/tc_pack.c).
 * -planhash prints only a hash of dsp_plan's output per frame (see check()); compare it before and
 * after a change to the planner: it must not move.
 */
#include "tc_model.h"
#include "ppu_tc.h"
#include "ppu_rows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*size);
    if (!d || fread(d, 1, (size_t)*size, f) != (size_t)*size) { perror(path); exit(2); }
    fclose(f);
    return d;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

#define PAL_SNAP_BYTES_V3 406   /* palette.h: PAL_SNAP_BYTES */

static TcStats total;
static int bands;                         /* -bands N: each frame in N bands too */
static int rowclean, rowgap = 1, rowfuzz, rowbuf;          /* -rowclean [-rowgap N]: dsp_rows against the reference renderer */
static long rb_pics, rb_bad, rb_badpics, rb_skip, rb_sync, rb_rows;
static long fz_n, fz_rows, fz_clean, fz_pix, fz_bad, fz_badn;
static long rc_frames, rc_rows, rc_clean, rc_pixclean, rc_bad, rc_badframes, rc_static;
static int planhash;                      /* -planhash: only the hash of dsp_plan's output, four ways */
static int band_max, band_bad;
static int falcon_frames, falcon_bad;     /* snapshots of true-colour runs */
static FILE *pack;
static uint32_t packed;
void tc_pack(FILE *f, const DspPlan *plan, const uint8_t *vram, const uint16_t *pic);
void tc_pack_count(FILE *f, uint32_t frames);

static int check(const char *path, const uint8_t *rom)
{
    long n;
    uint8_t *snap = load(path, &n);
    if (n < 12 || memcmp(snap, "SMKXSNAP", 8) || be32(snap + 8) != PPU_SNAP_VERSION) {
        fprintf(stderr, "%s: not a version %d SMKX snapshot\n", path, PPU_SNAP_VERSION);
        return 2;
    }
    const uint8_t *p = snap + 12;
    const uint8_t *vram = p; p += 65536;
    const uint8_t *cgram = p; p += 512;
    const uint8_t *oam = p; p += 544;
    const uint8_t *io = p; p += 0x6000;
    uint16_t bgofs[8], m7[6], m7ofs[2];
    for (int i = 0; i < 8; i++) { bgofs[i] = be16(p); p += 2; }
    for (int i = 0; i < 6; i++) { m7[i] = be16(p); p += 2; }
    for (int i = 0; i < 2; i++) { m7ofs[i] = be16(p); p += 2; }
    uint32_t nlog = be32(p); p += 4;
    static PpuWrite log[256];
    for (int i = 0; i < 256; i++) {
        log[i].line = be16(p); log[i].reg = be16(p + 2); log[i].value = p[4]; log[i].pad = 0;
        p += 6;
    }
    const uint8_t *wram = p; p += 131072;
    const uint8_t *sram = p; p += 8192;
    uint32_t planes = be32(p); p += 4;
    const uint8_t *falcon = p;
    p += ppu_snap_frame(planes) + 2 * PAL_SNAP_BYTES_V3;   /* the Falcon's frame and palettes */
    uint32_t noamseg = be32(p); p += 4;
    static PpuOamSeg oamseg[PPU_OAMSEG_MAX];
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
        oamseg[i].line = be16(p);
        memcpy(oamseg[i].oam, p + 4, PPU_OAM_BYTES);
        p += 4 + PPU_OAM_BYTES;
    }
    if (noamseg > PPU_OAMSEG_MAX) noamseg = PPU_OAMSEG_MAX;
    if (p - snap != n) {
        fprintf(stderr, "%s: %ld bytes, expected %ld\n", path, n, (long)(p - snap));
        return 2;
    }
    PpuBus bus = {wram, sram, rom};
    static uint8_t ident[256];
    for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
    static uint32_t hist[256];
    PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_hirom, &bus,
                   log, (int)(nlog > 256 ? 256 : nlog), ident, hist, NULL, oamseg, (int)noamseg, ppu_aptr_hirom, NULL};

    static uint16_t ref[PPU_W * PPU_H], tc[PPU_W * PPU_H];
    static DspPlan plan;
    memset(&plan, 0, sizeof plan);
    plan.tc = 1;
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (rowbuf) {
        /* The runtime's partial rendering (ppu_rows.c, the code smkx_tc.c calls) with two simulated screen
         * buffers: rows are copied from the reference picture as the DSP would draw them, pictures are planned
         * while the last is in flight (or after), some are sync (snapshot). The buffer that is shown when the
         * flight is over must equal the reference of the picture just handled, all rows. */
        static uint8_t pvram[65536], pd[DSP_VPAGES];
        static uint16_t bufs[2][PPU_W * PPU_H];
        static PartialState st = {.mode = 2};
        static int back = 0, have, pending_flip, seed = 7;
        ppu_render_565(&in, ref);
        for (int pg = 0; pg < DSP_VPAGES; pg++) pd[pg] = !have || memcmp(vram + pg * 512, pvram + pg * 512, 512) != 0;
        memcpy(pvram, vram, 65536);
        seed = seed * 1103515245 + 12345;
        int during = (seed >> 16) & 1, sync = ((seed >> 17) & 7) == 0;
        if (!during && pending_flip) { back ^= 1; pending_flip = 0; }
        /* the plan: buffers as they are now (a flight, if any, is into bufs[back]) */
        pr_begin(&st, &in, pd, bufs[back], bufs[back ^ 1], sync);
        if (pending_flip) { back ^= 1; pending_flip = 0; }          /* the flight completes */
        const void *shown;
        if (st.skipped == 1) {
            shown = pr_newest(&st, bufs[0], bufs[1]);
            rb_skip++;
        } else if (st.skipped == 2) {
            shown = bufs[back];                                     /* (sync) presented as it is */
            rb_sync++;
            back ^= 1;
        } else {
            int run[32][2], nr = pr_runs(st.need, 8, 3, run);
            uint16_t *t = bufs[back];
            for (int k = 0; k < nr; k++)
                for (int r = run[k][0]; r < run[k][1]; r++) {
                    memcpy(t + r * PPU_W, ref + r * PPU_W, PPU_W * 2);
                    rb_rows++;
                }
            pr_commit(&st, t);
            shown = t;
            pending_flip = 1;                                       /* it shows when the flight is over */
        }
        int bad = 0;
        for (int r = 0; r < PPU_H; r++) bad += memcmp((const uint16_t *)shown + r * PPU_W, ref + r * PPU_W, PPU_W * 2) != 0;
        rb_pics++;
        rb_bad += bad;
        rb_badpics += bad != 0;
        have = 1;
        free(snap);
        return bad != 0;
    }
    if (rowfuzz) {
        /* mutations of one picture: a word of a VRAM page the plan reads, a CGRAM entry or an OAM byte
         * changes, and only that page is marked written: the rows dsp_rows calls unchanged must render as
         * they did */
        static uint8_t mv[65536], mc[512], mo[PPU_OAM_BYTES], vd[DSP_VPAGES];
        static uint16_t base[PPU_W * PPU_H], mut[PPU_W * PPU_H];
        static DspRows R;
        static DspPlan fp;
        static uint32_t rng = 12345;
#define RND() (rng = rng * 1664525u + 1013904223u, rng >> 8)
        ppu_render_565(&in, base);
        memset(&fp, 0, sizeof fp);
        fp.tc = 1;
        if (dsp_plan(&in, &fp)) { free(snap); return 0; }
        int need[DSP_VPAGES], nn = 0;
        for (int pg = 0; pg < DSP_VPAGES; pg++) if (fp.need[pg]) need[nn++] = pg;
        memset(vd, 1, sizeof vd);
        R.vdirty = vd;
        dsp_rows_forget();
        dsp_rows(&in, &R);
        dsp_rows_commit();
        for (int m = 0; m < rowfuzz; m++) {
            memcpy(mv, vram, 65536);
            memcpy(mc, cgram, 512);
            memcpy(mo, oam, PPU_OAM_BYTES);
            memset(vd, 0, sizeof vd);
            int kind = (int)(RND() % 8);
            if (kind < 5 && nn) {                       /* VRAM: a word, or a run of 32 (a map row) */
                int pg = need[RND() % nn], n = kind == 0 ? 32 : 1;
                int w = (int)(RND() % 256);
                for (int i = 0; i < n; i++) {
                    int ww = (w & ~31) + ((w + i) & 31);
                    mv[pg * 512 + ww * 2] ^= (uint8_t)(RND() | 1);
                    if (RND() & 1) mv[pg * 512 + ww * 2 + 1] ^= (uint8_t)(RND());
                }
                vd[pg] = 1;
            } else if (kind < 7) {                      /* CGRAM: an entry (a palette block) */
                int e = (int)(RND() % 256);
                mc[e * 2] ^= (uint8_t)(RND() | 1);
                mc[e * 2 + 1] ^= (uint8_t)(RND() & 0x7F);
            } else {                                    /* OAM: a byte of an entry */
                int i = (int)(RND() % 128), b = (int)(RND() % 4);
                mo[i * 4 + b] ^= (uint8_t)(RND() | 1);
            }
            PpuInput in2 = in;
            in2.vram = mv;
            in2.cgram = mc;
            in2.oam = mo;
            PpuOamSeg segs[PPU_OAMSEG_MAX];
            if (noamseg) {
                for (int i = 0; i < (int)noamseg && i < PPU_OAMSEG_MAX; i++) segs[i] = oamseg[i];
                if (kind >= 7) memcpy(segs[0].oam, mo, PPU_OAM_BYTES);
                in2.oamseg = segs;
            }
            ppu_render_565(&in2, mut);
            R.vdirty = vd;
            dsp_rows(&in2, &R);
            for (int r = 0; r < PPU_H; r++) {
                int same = memcmp(base + r * PPU_W, mut + r * PPU_W, PPU_W * 2) == 0;
                fz_rows++;
                fz_pix += same;
                if (!R.dirty[r]) {
                    fz_clean++;
                    if (!same) fz_bad++;
                }
            }
            fz_n++;
        }
        free(snap);
        return 0;
    }
    if (rowclean) {
        /* dsp_rows (partial rendering): the rows it calls unchanged, against the reference's pixels of the
         * committed picture (every rowgap-th one). A row called clean whose pixels differ is a bug. */
        static uint8_t cvram[65536], vd[DSP_VPAGES];
        static uint16_t cpix[PPU_W * PPU_H];
        static int have, seq;
        static DspRows R;
        ppu_render_565(&in, ref);
        for (int pg = 0; pg < DSP_VPAGES; pg++) vd[pg] = !have || memcmp(vram + pg * 512, cvram + pg * 512, 512) != 0;
        R.vdirty = vd;
        int rc = dsp_rows(&in, &R);
        int clean = 0, pixclean = 0, bad = 0;
        if (have && rc == 0)
            for (int r = 0; r < PPU_H; r++) {
                int same = memcmp(ref + r * PPU_W, cpix + r * PPU_W, PPU_W * 2) == 0;
                pixclean += same;
                if (!R.dirty[r]) {
                    clean++;
                    bad += !same;
                }
            }
        rc_frames += have;
        rc_rows += have ? PPU_H : 0;
        rc_clean += clean;
        rc_pixclean += pixclean;
        rc_bad += bad;
        rc_badframes += bad != 0;
        rc_static += have && R.ndirty == 0;
        const char *nm = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        printf("ROWCLEAN %s mode %d: %s dirty %d clean %d pixel-identical %d wrong %d\n", nm, io[0x105] & 7,
               !have ? "first" : R.all ? "all" : "cmp", R.ndirty, clean, pixclean, bad);
        if (rc < 0) {
            dsp_rows_forget();
            have = 0;
        } else if (seq++ % rowgap == 0) {
            dsp_rows_commit();
            memcpy(cvram, vram, 65536);
            memcpy(cpix, ref, sizeof cpix);
            have = 1;
        }
        free(snap);
        return bad != 0;
    }
    if (planhash) {
        /* dsp_plan's output (stream, page needs, formats, row kinds) hashed: whole frame and in four
         * bands, each with the block cache compared (vdirty off) and trusted (vdirty: the pages that
         * differ from the previous snapshot on the command line). A change to the planner must
         * leave every hash as it was; the trusted and compared runs must agree. */
        static uint8_t prev[65536];
        static int have;
        static uint8_t vd[DSP_VPAGES];
        static DspPlan bp;
        uint64_t hm[4], all = 1469598103934665603ULL;
        for (int mode = 0; mode < 4; mode++) {
            int trusted = mode & 1, nb = mode < 2 ? 1 : 4;
            if (trusted)
                for (int pg = 0; pg < DSP_VPAGES; pg++)
                    vd[pg] = !have || memcmp(vram + pg * 512, prev + pg * 512, 512) != 0;
            uint64_t h = 1469598103934665603ULL;
#define HB(x) do { h ^= (uint64_t)(x); h *= 1099511628211ULL; } while (0)
            for (int b = 0; b < nb; b++) {
                memset(&bp, 0, sizeof bp);
                bp.tc = 1;
                bp.vdirty = trusted ? vd : 0;
                if (nb > 1) { bp.band0 = b * PPU_H / nb; bp.band1 = (b + 1) * PPU_H / nb; }
                int r = dsp_plan(&in, &bp);
                HB(r); HB(bp.fail); HB(bp.n); HB(bp.kernel); HB(bp.nneed); HB(bp.nsprneed); HB(bp.changes); HB(bp.segs);
                HB(bp.sprites); HB(bp.m7rows); HB(bp.winrows); HB(bp.subrows); HB(bp.mathrows); HB(bp.mathpal);
                for (int i = 0; i < bp.n && i < DSP_MAX_WORDS; i++) HB(bp.w[i]);
                for (int i = 0; i < DSP_VPAGES; i++) { HB(bp.need[i]); HB(bp.fmt[i]); HB(bp.sprneed[i]); }
                for (int i = 0; i < PPU_H; i++) HB(bp.rowkind[i]);
            }
            hm[mode] = h;
            all ^= h * (uint64_t)(mode + 1);
        }
        memcpy(prev, vram, 65536);
        have = 1;
        printf("PLANHASH %s %016llx%s%s\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, (unsigned long long)all,
               hm[1] != hm[0] ? " TRUST-MISMATCH" : "", hm[3] != hm[2] ? " TRUST-MISMATCH-BANDS" : "");
        free(snap);
        return hm[1] != hm[0] || hm[3] != hm[2];
    }
    ppu_render_565(&in, ref);
    /* A true-colour run's snapshot (CTX_TC=1): its frame against the
     * reference too. */
    int fdiff = -1;
    if (planes == 16) {
        fdiff = 0;
        for (int i = 0; i < PPU_W * PPU_H; i++) fdiff += ref[i] != be16(falcon + i * 2);
        falcon_frames++;
        falcon_bad += fdiff != 0;
    }
    if (dsp_plan(&in, &plan)) {
        int run[3][2], nr = plan.fail == 6 ? dsp_tc_runs(plan.rowkind, run, 3) : 0;
        if (!nr) {
            printf("%s: no plan, fail %d (the CPU composes it)", name, plan.fail);
            if (fdiff >= 0) printf("  Falcon frame: %d pixels differ", fdiff);
            printf("\n");
            free(snap);
            return 0;
        }
        /* Mode 7 and tile rows: the runs the runtime draws (smkx_tc.c),
         * each planned for its rows and drawn by the model for them */
        static DspPlan bp;
        int d = 0, out = 0, worst = 0, fail = 0;
        for (int k = 0; k < nr && !fail; k++) {
            memset(&bp, 0, sizeof bp);
            bp.tc = 1;
            bp.band0 = run[k][0];
            bp.band1 = run[k][1];
            uint8_t bf[DSP_VPAGES];
            memset(bf, 0, sizeof bf);
            tc_model_band0 = bp.band0;
            tc_model_band1 = bp.band1;
            fail = dsp_plan(&in, &bp) || tc_model_frame(bp.w, bp.n, bp.kernel, vram, tc, bf, NULL);
            tc_model_band0 = tc_model_band1 = 0;
            int sl = 0;
            for (int pg = 0; pg < DSP_VPAGES; pg++) {
                out += (bf[pg] & ~bp.fmt[pg]) != 0;
                for (int b = 0; b < 5; b++) sl += (bp.fmt[pg] >> b) & 1;
            }
            if (sl > worst) worst = sl;
        }
        for (int i = 0; i < PPU_W * PPU_H; i++) d += ref[i] != tc[i];
        printf("%s: Mode 7 and tile rows, %d runs (rows %d-%d first), at most %d page slots a run: diff %d pixels%s%s",
               name, nr, run[0][0], run[0][1], worst, d, out ? "  READ OUTSIDE PLAN" : "", fail ? "  NO PLAN" : "");
        if (fdiff >= 0) printf("  Falcon frame: %d pixels differ", fdiff);
        printf("\n");
        free(snap);
        return d || out || fail;
    }
    uint8_t fmt[DSP_VPAGES];
    memset(fmt, 0, sizeof fmt);
    TcStats s;
    memset(&s, 0, sizeof s);
    if (tc_model_frame(plan.w, plan.n, plan.kernel, vram, tc, fmt, &s)) {
        printf("%s: bad stream\n", name);
        free(snap);
        return 1;
    }
    int diff = 0, rows = 0, outside = 0, slots = 0, read = 0;
    for (int r = 0; r < PPU_H; r++) {
        int d = 0;
        for (int x = 0; x < PPU_W; x++) d += ref[r * PPU_W + x] != tc[r * PPU_W + x];
        diff += d;
        rows += d != 0;
    }
    for (int pg = 0; pg < DSP_VPAGES; pg++) {
        outside += (fmt[pg] & ~plan.fmt[pg]) != 0;
        for (int b = 0; b < 5; b++) {
            slots += (plan.fmt[pg] >> b) & 1;
            read += (fmt[pg] >> b) & 1;
        }
    }
    printf("%s mode %d TM $%02X TS $%02X: words %4d  changes %3d  sprites %3d  page slots %3d  "
           "read %3d  diff %d pixels in %d rows%s", name, io[0x105] & 7, io[0x12C], io[0x12D], plan.n,
           plan.changes, plan.sprites, slots, read, diff, rows, outside ? "  READ OUTSIDE PLAN" : "");
    if (fdiff >= 0) printf("  Falcon frame: %d pixels differ", fdiff);
    printf("\n");
    if (bands > 1) {
        /* The frame as N BAND commands draw it: each band planned for its
         * rows only, the model reading nothing outside that plan. */
        static uint16_t cmb[PPU_W * PPU_H];
        static DspPlan bp;
        int worst = 0, bout = 0, bfail = 0;
        for (int b = 0; b < bands && !bfail; b++) {
            memset(&bp, 0, sizeof bp);
            bp.tc = 1;
            bp.band0 = b * PPU_H / bands;
            bp.band1 = (b + 1) * PPU_H / bands;
            uint8_t bf[DSP_VPAGES];
            memset(bf, 0, sizeof bf);
            tc_model_band0 = bp.band0;
            tc_model_band1 = bp.band1;
            bfail = dsp_plan(&in, &bp) || tc_model_frame(bp.w, bp.n, bp.kernel, vram, cmb, bf, NULL);
            tc_model_band0 = tc_model_band1 = 0;
            int sl = 0;
            for (int pg = 0; pg < DSP_VPAGES; pg++) {
                bout += (bf[pg] & ~bp.fmt[pg]) != 0;
                for (int k = 0; k < 5; k++) sl += (bp.fmt[pg] >> k) & 1;
            }
            if (sl > worst) worst = sl;
        }
        int d = 0;
        for (int i = 0; i < PPU_W * PPU_H; i++) d += cmb[i] != ref[i];
        printf("  in %d bands: at most %d page slots a band, %d pixels differ%s%s\n", bands, worst, d,
               bout ? ", READ OUTSIDE A BAND'S PLAN" : "", bfail ? ", NO PLAN" : "");
        if (worst > band_max) band_max = worst;
        band_bad += d || bout || bfail;
    }
    if (pack && !diff && !outside) {
        tc_pack(pack, &plan, vram, ref);
        packed++;
    }
    total.tilerows += s.tilerows; total.clear += s.clear; total.opaque += s.opaque;
    total.objslivers += s.objslivers; total.objclear += s.objclear; total.objopaque += s.objopaque;
    total.mathpix += s.mathpix; total.pix += s.pix;
    for (int i = 0; i < 5; i++) total.layerpix[i] += s.layerpix[i];
    free(snap);
    return diff || outside;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s rom.sfc SNAP.BIN...\n", argv[0]);
        return 2;
    }
    long rn;
    uint8_t *rom = load(argv[1], &rn);
    int bad = 0;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-bands") && i + 1 < argc) bands = atoi(argv[i + 1]);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-planhash")) planhash = 1;
        if (!strcmp(argv[i], "-rowclean")) rowclean = 1;
        if (!strcmp(argv[i], "-rowbuf")) rowbuf = 1;
        if (!strcmp(argv[i], "-rowfuzz") && i + 1 < argc) rowfuzz = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "-rowgap") && i + 1 < argc) rowgap = atoi(argv[i + 1]);
    }
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-pack") && i + 1 < argc) {
            pack = fopen(argv[i + 1], "wb");
            if (!pack) { perror(argv[i + 1]); return 2; }
            fwrite("TCP1\0\0\0\0", 1, 8, pack);
        }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-planhash")) { planhash = 1; continue; }
        if (!strcmp(argv[i], "-pack") || !strcmp(argv[i], "-bands") || !strcmp(argv[i], "-rowgap") || !strcmp(argv[i], "-rowfuzz")) { i++; continue; }
        if (!strcmp(argv[i], "-rowclean") || !strcmp(argv[i], "-rowbuf")) continue;
        bad |= check(argv[i], rom);
    }
    if (pack) {
        tc_pack_count(pack, packed);
        fclose(pack);
    }
    if (bands > 1) {
        printf("in %d bands: at most %d page slots a band; %d frames wrong\n", bands, band_max, band_bad);
        bad |= band_bad != 0;
    }
    if (rowbuf)
        printf("rowbuf: %ld pictures, %ld skipped, %ld sync-presented, %ld rows drawn (%.1f%% of all): WRONG %ld rows in %ld pictures\n", rb_pics, rb_skip, rb_sync, rb_rows, 100.0 * rb_rows / (rb_pics ? rb_pics * 224.0 : 1), rb_bad, rb_badpics);
    if (rowfuzz)
        printf("rowfuzz: %ld mutations, rows %ld: pixel-identical %ld, called unchanged %ld, WRONG %ld\n", fz_n, fz_rows, fz_pix, fz_clean, fz_bad);
    if (rowclean)
        printf("rowclean: %ld compared pictures, rows %ld: called unchanged %ld (%.1f%%), pixel-identical %ld (%.1f%%), "
               "WRONG %ld rows in %ld pictures; pictures with no row to draw %ld\n", rc_frames, rc_rows, rc_clean,
               100.0 * rc_clean / (rc_rows ? rc_rows : 1), rc_pixclean, 100.0 * rc_pixclean / (rc_rows ? rc_rows : 1),
               rc_bad, rc_badframes, rc_static);
    if (falcon_frames) {
        printf("Falcon true-colour frames %d: %d differ from the reference\n", falcon_frames, falcon_bad);
        bad |= falcon_bad != 0;
    }
    printf("tile rows %u: all transparent %u, all opaque %u; sprite slivers %u: transparent %u, opaque %u; "
           "pixels %u, with colour math %u; layer lines BG1-4/OBJ %u %u %u %u %u\n",
           total.tilerows, total.clear, total.opaque, total.objslivers, total.objclear, total.objopaque,
           total.pix, total.mathpix, total.layerpix[0], total.layerpix[1], total.layerpix[2], total.layerpix[3],
           total.layerpix[4]);
    return bad;
}

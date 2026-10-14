/* What a frame's screen is made of, line by line: the census for a
 * race-only DSP kernel. Reads a snapshot (SNAP.BIN format, src/snes_ppu.h),
 * runs the reference frame loop (ppu_render_rows: HDMA, IRQ-logged writes,
 * OAM segments) with a row function that records each row's register
 * state instead of drawing, and prints:
 *
 *   - the enabled HDMA channels (all of them, also those the renderer
 *     ignores: windows, colour math) with their target and table;
 *   - the IRQ-logged writes, grouped by line;
 *   - the rows grouped by layout (mode, layers, tile maps, OBSEL, M7SEL,
 *     windows, colour math), and per group which values change inside it
 *     (scroll, Mode 7 matrix and centre) and on how many rows;
 *   - sprites per row (entries in range, before the 32/34 limits) and the
 *     OAM segment in use;
 *   - rows where CGRAM differs from the row before.
 *
 *   build/race_census SNAP.BIN rom.sfc [frame.ppm]
 *
 * With a third argument it also draws the frame with the reference row
 * composer in SNES colours (CGRAM, INIDISP brightness), to see which
 * screen a snapshot shows.
 *
 * Host tool; nothing here runs on the Falcon. */
#include "snes_ppu.h"
#include "ppu_internal.h"
#include "palette.h"

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

/* Layout registers ($2100 + n): a change starts a new group. */
static const uint8_t layout_regs[] = {
    0x00, 0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x1A,
    0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B,
    0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33,
};
#define NLAYOUT (int)sizeof layout_regs

typedef struct {
    uint8_t lay[NLAYOUT];
    uint16_t bgofs[8];
    int16_t m7[6];
    uint16_t m7ofs[2];
    const uint8_t *oam;
    int nspr;
    uint8_t inidisp;
    uint8_t cgram[512];
} Row;

static Row rows[PPU_H];

static const uint8_t obj_sz[8][2][2] = {
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

static int sprites_on(const uint8_t *oam, uint8_t obsel, int row)
{
    int n = 0;
    for (int i = 0; i < 128; i++) {
        const uint8_t *e = oam + i * 4;
        uint8_t hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int w = obj_sz[obsel >> 5][hi >> 1][0], h = obj_sz[obsel >> 5][hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        if (x <= -w || x >= 256) continue;
        int dy = (row - e[1]) & 0xFF;
        if (dy < h) n++;
    }
    return n;
}

static void record(const PpuInput *in, const Regs *s, int row, uint8_t *out)
{
    if (out) ppu_ref_row(in, s, row, out);
    Row *r = &rows[row];
    for (int i = 0; i < NLAYOUT; i++) r->lay[i] = s->r[layout_regs[i]];
    memcpy(r->bgofs, s->bgofs, sizeof r->bgofs);
    memcpy(r->m7, s->m7, sizeof r->m7);
    memcpy(r->m7ofs, s->m7ofs, sizeof r->m7ofs);
    r->oam = s->oam;
    r->inidisp = s->r[0x00];
    r->nspr = sprites_on(s->oam, s->r[0x01], row);
    memcpy(r->cgram, s->cgram, 512);
}

static const char *mode_name(uint8_t bgmode)
{
    static char b[16];
    snprintf(b, sizeof b, "mode %d%s", bgmode & 7, (bgmode & 8) ? " BG3pri" : "");
    return b;
}

static void layers(uint8_t tm, char *b)
{
    static const char *nm[5] = {"BG1", "BG2", "BG3", "BG4", "OBJ"};
    b[0] = 0;
    for (int i = 0; i < 5; i++)
        if (tm & (1 << i)) { strcat(b, nm[i]); strcat(b, " "); }
    if (!b[0]) strcpy(b, "none ");
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s SNAP.BIN rom.sfc [frame.ppm]\n", argv[0]);
        return 2;
    }
    long n, romsize;
    uint8_t *snap = load(argv[1], &n);
    uint8_t *rom = load(argv[2], &romsize);
    if (n < 12 || memcmp(snap, "SMKXSNAP", 8) || be32(snap + 8) != PPU_SNAP_VERSION) {
        fprintf(stderr, "%s: not a version %d SMKX snapshot\n", argv[1], PPU_SNAP_VERSION);
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
    const uint8_t *sram = p; p += 2048;
    p += 28672 + 2 * PAL_SNAP_BYTES;
    uint32_t noamseg = be32(p); p += 4;
    static PpuOamSeg oamseg[PPU_OAMSEG_MAX];
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
        oamseg[i].line = be16(p);
        memcpy(oamseg[i].oam, p + 4, PPU_OAM_BYTES);
        p += 4 + PPU_OAM_BYTES;
    }
    if (noamseg > PPU_OAMSEG_MAX) noamseg = PPU_OAMSEG_MAX;
    if (p - snap != n) {
        fprintf(stderr, "%s: %ld bytes, expected %ld\n", argv[1], n, (long)(p - snap));
        return 2;
    }
    if (nlog > 256) nlog = 256;

    printf("%s: INIDISP $%02X, NMITIMEN $%02X, HDMAEN $%02X, %u IRQ writes, %u OAM segments\n",
           argv[1], io[0x100], io[0x2200], io[0x220C], nlog, noamseg);

    /* HDMA channels as the frame starts */
    static const int unit[8] = {1, 2, 2, 4, 4, 4, 2, 4};
    for (int ch = 0; ch < 8; ch++) {
        if (!((io[0x220C] >> ch) & 1)) continue;
        const uint8_t *r = io + 0x2300 + ch * 16;
        printf("  HDMA %d: $21%02X mode %d (%d byte%s)%s, table $%02X%02X%02X%s\n", ch, r[1], r[0] & 7,
               unit[r[0] & 7], unit[r[0] & 7] > 1 ? "s" : "", (r[0] & 0x40) ? " indirect" : "",
               r[4], r[3], r[2], (r[0] & 0x40) ? "" : "");
    }

    /* IRQ-logged writes, one line per scanline */
    for (uint32_t i = 0; i < nlog;) {
        uint16_t line = log[i].line;
        printf("  IRQ writes at line %3u:", line);
        for (; i < nlog && log[i].line == line; i++) printf(" $%04X=%02X", log[i].reg, log[i].value);
        printf("\n");
    }
    for (uint32_t i = 0; i < noamseg; i++) printf("  OAM segment %u from line %u\n", i, oamseg[i].line);

    PpuBus bus = {wram, sram, rom};
    static uint8_t map[256];
    static uint32_t hist[256];
    PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_hirom, &bus,
                   log, (int)nlog, map, hist, NULL, oamseg, (int)noamseg, ppu_aptr_hirom};
    for (int i = 0; i < 256; i++) map[i] = (uint8_t)i;     /* out: CGRAM indices */
    static uint8_t idx[PPU_W * PPU_H];
    ppu_init();
    ppu_render_rows(&in, argc == 4 ? idx : NULL, record);
    if (argc == 4) {
        FILE *f = fopen(argv[3], "wb");
        if (!f) { perror(argv[3]); return 2; }
        fprintf(f, "P6\n%d %d\n255\n", PPU_W, PPU_H);
        for (int y = 0; y < PPU_H; y++) {
            const Row *r = &rows[y];
            int b = (r->inidisp & 0x80) ? 0 : (r->inidisp & 15);
            for (int x = 0; x < PPU_W; x++) {
                const uint8_t *c = r->cgram + idx[y * PPU_W + x] * 2;
                uint16_t v = (uint16_t)(c[0] | c[1] << 8);
                int rgb[3] = {v & 31, (v >> 5) & 31, (v >> 10) & 31};
                for (int k = 0; k < 3; k++) fputc(rgb[k] * 255 * b / (31 * 15), f);
            }
        }
        fclose(f);
    }

    /* groups of rows with the same layout */
    for (int a = 0; a < PPU_H;) {
        int b = a;
        while (b + 1 < PPU_H && !memcmp(rows[b + 1].lay, rows[a].lay, NLAYOUT)) b++;
        const Row *r = &rows[a];
        char tm[32], ts[32];
        layers(r->lay[20], tm);
        layers(r->lay[21], ts);
        printf("rows %3d-%3d (%3d): %s, main %ssub %sINIDISP $%02X OBSEL $%02X M7SEL $%02X\n",
               a, b, b - a + 1, mode_name(r->lay[2]), tm, ts, r->lay[0], r->lay[1], r->lay[10]);
        printf("    SC $%02X $%02X $%02X $%02X NBA $%02X $%02X, mosaic $%02X, windows $%02X $%02X $%02X"
               " [%02X %02X %02X %02X] log $%02X $%02X TMW $%02X TSW $%02X, CGWSEL $%02X CGADSUB $%02X"
               " COLDATA $%02X SETINI $%02X\n",
               r->lay[4], r->lay[5], r->lay[6], r->lay[7], r->lay[8], r->lay[9], r->lay[3],
               r->lay[11], r->lay[12], r->lay[13], r->lay[14], r->lay[15], r->lay[16], r->lay[17],
               r->lay[18], r->lay[19], r->lay[22], r->lay[23], r->lay[24], r->lay[25], r->lay[26],
               r->lay[27]);
        /* values that change inside the group */
        static const char *ofsn[8] = {"BG1H", "BG1V", "BG2H", "BG2V", "BG3H", "BG3V", "BG4H", "BG4V"};
        static const char *m7n[6] = {"M7A", "M7B", "M7C", "M7D", "M7X", "M7Y"};
        printf("    values:");
        for (int k = 0; k < 8; k++) {
            int ch = 0;
            for (int y = a + 1; y <= b; y++) ch += rows[y].bgofs[k] != rows[y - 1].bgofs[k];
            if (ch) printf(" %s(%d changes)", ofsn[k], ch);
            else printf(" %s=%u", ofsn[k], r->bgofs[k] & 0x3FF);
        }
        printf("\n            ");
        for (int k = 0; k < 6; k++) {
            int ch = 0, lo = r->m7[k], hi = r->m7[k];
            for (int y = a + 1; y <= b; y++) {
                ch += rows[y].m7[k] != rows[y - 1].m7[k];
                if (rows[y].m7[k] < lo) lo = rows[y].m7[k];
                if (rows[y].m7[k] > hi) hi = rows[y].m7[k];
            }
            if (ch) printf(" %s(%d changes, %d..%d)", m7n[k], ch, lo, hi);
            else printf(" %s=%d", m7n[k], r->m7[k]);
        }
        for (int k = 0; k < 2; k++) {
            int ch = 0;
            for (int y = a + 1; y <= b; y++) ch += rows[y].m7ofs[k] != rows[y - 1].m7ofs[k];
            if (ch) printf(" M7%cOFS(%d changes)", "HV"[k], ch);
            else printf(" M7%cOFS=%d", "HV"[k], r->m7ofs[k] & 0x1FFF);
        }
        int smax = 0, stot = 0;
        for (int y = a; y <= b; y++) { stot += rows[y].nspr; if (rows[y].nspr > smax) smax = rows[y].nspr; }
        printf("\n    sprites per row: max %d, mean %.1f\n", smax, (double)stot / (b - a + 1));
        a = b + 1;
    }
    int cgch = 0;
    for (int y = 1; y < PPU_H; y++)
        if (memcmp(rows[y].cgram, rows[y - 1].cgram, 512)) {
            if (!cgch) printf("CGRAM changes at rows:");
            printf(" %d", y);
            cgch++;
        }
    printf(cgch ? "\n" : "CGRAM: no change within the frame\n");
    return 0;
}

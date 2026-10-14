/* Exact sampling reuse in frozen Mode 7 frames; no timing model.
 * Build with src/snes_ppu.c and src/m7_plan.c, -Isrc.
 * Usage: m7_sampling_audit SNAP.BIN ROM.sfc [SNAP.BIN ...]
 * Replays HDMA/OAM/IRQ state, checks the segment sampler against the
 * reference, and counts opportunities without assuming a cache is free. */
#include "ppu_internal.h"
#include "m7_plan.h"
#include "palette.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned long rows, pixels, inside, tile0, fill, segments;
    unsigned long same_cell, same_address, same_uv, same_colour;
    unsigned long constant_v, front_obj, no_obj_groups, checked, bad;
} Counts;
static Counts counts[2];

static unsigned be16(const unsigned char *p) { return p[0] * 256u + p[1]; }
static unsigned be32(const unsigned char *p) { return be16(p) * 65536u + be16(p + 2); }

static unsigned char *load(const char *name, size_t *size)
{
    FILE *f = fopen(name, "rb");
    if (!f) { perror(name); exit(2); }
    if (fseek(f, 0, SEEK_END)) exit(2);
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET)) exit(2);
    unsigned char *p = malloc((size_t)n + 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *size = (size_t)n;
    return p;
}

static void sample(const PpuInput *in, const Regs *s, int row, uint8_t *out)
{
    (void)out;
    if ((s->r[0] & 0x80) || (s->r[5] & 7) != 7 || !(s->r[0x2c] & 1)) return;
    Counts *c = &counts[row >= 114];
    PpuM7Line l;
    M7Seg seg[M7_MAX_SEGS];
    uint8_t ref[PPU_W];
    uint16_t obj[PPU_W] = {0}, win[3][2];
    ppu_mode7_setup(s, row, &l);
    int ns = m7_plan(&l, seg);
    ppu_m7_pixels(in->vram, &l, ref);
    if (s->r[0x2c] & 16) ppu_obj_line(in, s, row, obj);
    int nw = ppu_win_spans(s, 4, win);
    for (int w = 0; w < nw; w++)
        for (int x = win[w][0]; x < win[w][0] + win[w][1]; x++) obj[x] = 0;
    c->rows++;
    c->constant_v += l.c == 0;
    c->segments += (unsigned)ns;
    int x = 0, prev_cell = -1, prev_addr = -1, prev_uv = -1, prev_col = -1;
    for (int j = 0; j < ns; j++) {
        M7Seg *g = &seg[j];
        for (int k = 0; k < g->count; k++, x++) {
            if (x >= PPU_W) { c->bad++; continue; }
            int u = (g->u + k * g->du) >> 8;
            int v = (g->v + k * g->dv) >> 8;
            int cell = -1, addr = -1, uv = -1, colour = 0;
            if (g->kind == M7_FILL) c->fill++;
            else {
                int tile = 0;
                if (g->kind == M7_INSIDE) {
                    cell = (v >> 3) * 128 + (u >> 3);
                    tile = in->vram[cell * 2];
                    uv = v * 1024 + u;
                    c->inside++;
                } else c->tile0++;
                addr = tile * 64 + (v & 7) * 8 + (u & 7);
                colour = in->vram[addr * 2 + 1];
            }
            c->pixels++;
            c->checked++;
            c->bad += colour != ref[x];
            c->same_cell += cell >= 0 && cell == prev_cell;
            c->same_address += addr >= 0 && addr == prev_addr;
            c->same_uv += uv >= 0 && uv == prev_uv;
            c->same_colour += colour == prev_col;
            c->front_obj += (obj[x] & 0x400) && (obj[x] & 0x300);
            prev_cell = cell; prev_addr = addr; prev_uv = uv; prev_col = colour;
        }
    }
    if (x != PPU_W) c->bad++;
    for (int g = 0; g < 16; g++) {
        int any = 0;
        for (int k = 0; k < 16; k++) any |= obj[g * 16 + k] & 0x400;
        c->no_obj_groups += !any;
    }
}

static int audit(const char *name, const uint8_t *rom)
{
    size_t size;
    uint8_t *snap = load(name, &size);
    const size_t expected = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 +
        256 * 6 + 131072 + 2048 + 28672 + 2 * PAL_SNAP_BYTES + 4 +
        PPU_OAMSEG_MAX * (4 + PPU_OAM_BYTES);
    if (size != expected || memcmp(snap, "SMKXSNAP", 8) || be32(snap + 8) != PPU_SNAP_VERSION) {
        fprintf(stderr, "%s: unsupported snapshot\n", name); free(snap); return 1;
    }
    const uint8_t *p = snap + 12;
    PpuInput in = {0};
    in.vram = p; p += 65536;
    in.cgram = p; p += 512;
    in.oam = p; p += 544;
    in.io = p; p += 0x6000;
    uint16_t bg[8], m7[6], ofs[2];
    for (int i = 0; i < 8; i++, p += 2) bg[i] = (uint16_t)be16(p);
    for (int i = 0; i < 6; i++, p += 2) m7[i] = (uint16_t)be16(p);
    for (int i = 0; i < 2; i++, p += 2) ofs[i] = (uint16_t)be16(p);
    in.bgofs = bg; in.m7 = m7; in.m7ofs = ofs;
    unsigned nl = be32(p); p += 4;
    PpuWrite log[256];
    for (int i = 0; i < 256; i++, p += 6)
        log[i] = (PpuWrite){(uint16_t)be16(p), (uint16_t)be16(p + 2), p[4], 0};
    const uint8_t *wram = p; p += 131072;
    const uint8_t *sram = p; p += 2048 + 28672 + 2 * PAL_SNAP_BYTES;
    unsigned no = be32(p); p += 4;
    PpuOamSeg oam[PPU_OAMSEG_MAX];
    for (int i = 0; i < PPU_OAMSEG_MAX; i++, p += 4 + PPU_OAM_BYTES) {
        oam[i].line = (uint16_t)be16(p);
        memcpy(oam[i].oam, p + 4, PPU_OAM_BYTES);
    }
    if (nl > 256 || no > PPU_OAMSEG_MAX) { free(snap); return 1; }
    PpuBus bus = {wram, sram, rom};
    uint8_t map[256];
    uint32_t hist[256] = {0};
    for (int i = 0; i < 256; i++) map[i] = (uint8_t)i;
    in.abus = ppu_abus_hirom; in.aptr = ppu_aptr_hirom; in.ctx = &bus;
    in.log = log; in.nlog = (int)nl; in.map = map; in.hist = hist;
    in.oamseg = oam; in.noamseg = (int)no;
    memset(counts, 0, sizeof counts);
    ppu_render_rows(&in, NULL, sample);
    for (int v = 0; v < 2; v++) {
        Counts *c = &counts[v];
        printf("%s,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
               name, v ? "lower" : "upper", c->rows, c->pixels, c->inside,
               c->tile0, c->fill, c->segments, c->same_cell, c->same_address,
               c->same_uv, c->same_colour, c->constant_v, c->front_obj,
               c->no_obj_groups, c->checked, c->bad);
    }
    int bad = counts[0].bad || counts[1].bad;
    free(snap);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s SNAP.BIN ROM.sfc [SNAP.BIN ...]\n", argv[0]); return 2; }
    size_t n;
    uint8_t *rom = load(argv[2], &n);
    if (n != 0x80000) { fprintf(stderr, "expected 512 KiB SMK ROM\n"); free(rom); return 2; }
    puts("snapshot,view,rows,pixels,inside,tile0,fill,segments,same_cell,same_address,same_uv,same_colour,constant_v_rows,front_obj_pixels,no_obj_groups,checked,bad");
    ppu_init();
    int bad = audit(argv[1], rom);
    for (int i = 3; i < argc; i++) bad |= audit(argv[i], rom);
    free(rom);
    return bad;
}

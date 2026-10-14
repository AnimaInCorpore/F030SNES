/* Host check of src/dsp1_hle.c against the firmware: every vector holds the
 * chip's state where a hot loop starts and where the firmware leaves it
 * (tools/smkx/dsp1_hle.py --vectors); the C routine must reproduce the
 * second from the first exactly.
 *
 *   build/dsp1_hle_test raster build/smkx/raster_vectors.bin assets/dsp1.bin
 *
 * Regions: raster (hand-written, src/dsp1_hle.c), and the generated ones
 * (tools/smkx/upd2c.py): region_raster, region_project. */
#include "dsp1_hle.h"
extern const uint16_t dsp1_flags_live[2048];

unsigned dsp1_region_raster(UpdState *s, uint16_t *ram, const uint16_t *rom);
unsigned dsp1_region_project(UpdState *s, uint16_t *ram, const uint16_t *rom);

static const struct {
    const char *name;
    unsigned (*fn)(UpdState *, uint16_t *, const uint16_t *);
} regions[] = {
    {"raster", dsp1_hle_raster},
    {"region_raster", dsp1_region_raster},
    {"region_project", dsp1_region_project},
};

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NW (27 + 256)

static void load(const uint16_t *w, UpdState *s, uint16_t *ram)
{
    s->tr = w[0]; s->trb = w[1]; s->dr = w[2]; s->sr = w[3]; s->pc = w[4];
    for (int i = 0; i < 4; i++) s->stack[i] = w[5 + i];
    for (int i = 0; i < 6; i++) { s->fa[i] = (uint8_t)w[9 + i]; s->fb[i] = (uint8_t)w[15 + i]; }
    s->a = w[21]; s->b = w[22]; s->k = w[23]; s->l = w[24]; s->dp = w[25]; s->rp = w[26];
    memcpy(ram, w + 27, 256 * 2);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s region vectors.bin dsp1.bin\n", argv[0]);
        return 2;
    }
    unsigned (*fn)(UpdState *, uint16_t *, const uint16_t *) = 0;
    for (unsigned i = 0; i < sizeof regions / sizeof regions[0]; i++)
        if (!strcmp(regions[i].name, argv[1])) fn = regions[i].fn;
    if (!fn) { fprintf(stderr, "unknown region %s\n", argv[1]); return 2; }
    argv++;
    FILE *f = fopen(argv[2], "rb");
    static uint8_t fw[8192];
    if (!f || fread(fw, 1, sizeof fw, f) != sizeof fw) { perror(argv[2]); return 2; }
    fclose(f);
    static uint16_t rom[1024];
    for (int i = 0; i < 1024; i++) rom[i] = (uint16_t)(fw[6144 + i * 2] << 8 | fw[6144 + i * 2 + 1]);
    f = fopen(argv[1], "rb");
    uint32_t n;
    if (!f || fread(&n, 4, 1, f) != 1) { perror(argv[1]); return 2; }
    static uint16_t w[2 * NW];
    int bad = 0;
    for (uint32_t v = 0; v < n; v++) {
        if (fread(w, 2, 2 * NW, f) != 2 * NW) { fprintf(stderr, "short file\n"); return 2; }
        UpdState s, want;
        static uint16_t ram[256], want_ram[256];
        load(w, &s, ram);
        load(w + NW, &want, want_ram);
        unsigned pc = fn(&s, ram, rom);
        s.pc = (uint16_t)pc;
        /* flags dead where the region hands back hold anything */
        uint16_t lv = dsp1_flags_live[want.pc & 0x7FF];
        for (int i = 0; i < 6; i++) {
            if (!(lv >> i & 1)) s.fa[i] = want.fa[i] = 0;
            if (!(lv >> (6 + i) & 1)) s.fb[i] = want.fb[i] = 0;
        }
        int diff = memcmp(&s.tr, &want.tr, sizeof s) != 0 || memcmp(ram, want_ram, sizeof ram) != 0;
        if (diff && bad++ < 5) {
            printf("vector %u differs: pc %03X/%03X a %04X/%04X b %04X/%04X k %04X/%04X l %04X/%04X "
                   "dp %02X/%02X rp %03X/%03X tr %04X/%04X trb %04X/%04X\n", v, s.pc, want.pc, s.a, want.a,
                   s.b, want.b, s.k, want.k, s.l, want.l, s.dp, want.dp, s.rp, want.rp, s.tr, want.tr, s.trb, want.trb);
            for (int i = 0; i < 6; i++)
                if (s.fa[i] != want.fa[i] || s.fb[i] != want.fb[i])
                    printf("  flag %d: fa %02X/%02X fb %02X/%02X\n", i, s.fa[i], want.fa[i], s.fb[i], want.fb[i]);
            for (int i = 0; i < 4; i++)
                if (s.stack[i] != want.stack[i]) printf("  stack[%d] %03X/%03X\n", i, s.stack[i], want.stack[i]);
        }
    }
    printf("%s: %u vectors, differing: %d\n", argv[0], n, bad);
    return bad != 0;
}

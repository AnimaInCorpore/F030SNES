/* Host check of ppu_dsp_frame_slots (src/ppu_dsp.c): the slots it writes,
 * fast path included, against ppu_dsp_frame's words turned into slots, for
 * every snapshot given. Prints the snapshots whose streams differ and the
 * share of lines the fast path took.
 *
 *   build/stream_check rom.sfc SNAP.BIN ... */
#include "ppu_internal.h"
#include "ppu_dsp.h"
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

extern uint32_t ppu_dsp_fast_lines;
static uint16_t ref[80000], got[80000];
static int nref;
static void put(uint32_t w) { ref[nref++] = (uint16_t)(w >> 8); ref[nref++] = (uint16_t)(w & 0xFF); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s rom.sfc SNAP.BIN ...\n", argv[0]); return 2; }
    long rs;
    uint8_t *rom = load(argv[1], &rs);
    int bad = 0, total = 0;
    long words_ref = 0;
    for (int a = 2; a < argc; a++) {
        long n;
        uint8_t *snap = load(argv[a], &n);
        const uint8_t *p = snap + 12, *vram = p; p += 65536;
        const uint8_t *cgram = p; p += 512;
        const uint8_t *oam = p; p += 544;
        const uint8_t *io = p; p += 0x6000;
        uint16_t bgofs[8], m7[6], m7ofs[2];
        for (int i = 0; i < 8; i++) { bgofs[i] = be16(p); p += 2; }
        for (int i = 0; i < 6; i++) { m7[i] = be16(p); p += 2; }
        for (int i = 0; i < 2; i++) { m7ofs[i] = be16(p); p += 2; }
        uint32_t nlog = be32(p); p += 4;
        static PpuWrite log[256];
        for (int i = 0; i < 256; i++) { log[i].line = be16(p); log[i].reg = be16(p + 2); log[i].value = p[4]; p += 6; }
        const uint8_t *wram = p; p += 131072;
        const uint8_t *sram = p; p += 2048;
        p += 28672 + 2 * PAL_SNAP_BYTES;
        uint32_t nseg = be32(p); p += 4;
        static PpuOamSeg seg[PPU_OAMSEG_MAX];
        for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
            seg[i].line = be16(p);
            memcpy(seg[i].oam, p + 4, PPU_OAM_BYTES);
            p += 4 + PPU_OAM_BYTES;
        }
        PpuBus bus = {wram, sram, rom};
        static uint8_t map[256];
        static uint32_t hist[256];
        PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_hirom, &bus, log,
                       (int)(nlog > 256 ? 256 : nlog), map, hist, NULL, seg,
                       (int)(nseg > PPU_OAMSEG_MAX ? PPU_OAMSEG_MAX : nseg), ppu_aptr_hirom};
        nref = 0;
        ppu_dsp_frame(&in, put);
        uint16_t *e = ppu_dsp_frame_slots(&in, got, got + 80000);
        int ng = e ? (int)(e - got) : -1;
        total++;
        words_ref += nref / 2;
        if (ng != nref || memcmp(ref, got, (size_t)nref * 2)) {
            int i = 0;
            while (i < nref && i < ng && ref[i] == got[i]) i++;
            printf("%s: slots %d vs %d, first difference at %d\n", argv[a], nref, ng, i);
            bad++;
        }
        free(snap);
    }
    printf("%d snapshots, %d differ, %ld words on average, %.1f of 225 lines a frame on the fast path\n",
           total, bad, total ? words_ref / total : 0, total ? (double)ppu_dsp_fast_lines / total : 0.0);
    return bad != 0;
}

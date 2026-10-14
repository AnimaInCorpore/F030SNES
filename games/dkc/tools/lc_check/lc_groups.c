#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define H 224
#define W 256
static uint32_t hp[H][256], hc[H][256];
int main(int argc, char **argv)
{
    int have = 0, prevn = 0;
    double sd = 0, sgrp = 0, sruns = 0, sopx = 0, sall = 0; int n = 0, ng = 0;
    int maxd = 0;
    printf("%-14s gap  scrollx  overlayPx  ovGroups  dirtyGrp  runsInDirty\n", "frame");
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb"); uint16_t sc[2]; static uint8_t F[H*W], B[H*W];
        if (!f || fread(sc, 2, 2, f) != 2 || fread(F, 1, H*W, f) != H*W || fread(B, 1, H*W, f) != H*W) return 2;
        fclose(f);
        const char *b = strrchr(argv[a], '/'); int num = (int)strtol(b + 2, 0, 16);
        memset(hc, 0, sizeof hc);
        int opx = 0, ovg = 0;
        for (int y = 0; y < H; y++) {
            int wy = (y + sc[1]) & 511; (void)wy;
            for (int x = 0; x < W; x++) {
                if (F[y*W+x] == B[y*W+x]) continue;
                opx++;
                int wx = (x + sc[0]) & 4095, g = wx >> 4;
                uint32_t h = hc[y][g] * 2654435761u + (uint32_t)(F[y*W+x] + 1) * 40503u + (uint32_t)(wx & 15) * 7919u + 1;
                hc[y][g] = h | 1;
            }
            for (int g = 0; g < 256; g++) if (hc[y][g]) ovg++;
        }
        int dirty = 0, runs = 0;
        if (have) {
            for (int y = 0; y < H; y++) for (int g = 0; g < 256; g++)
                if (hc[y][g] != hp[y][g]) dirty++;
            /* runs of overlay pixels lying in groups that are dirty now */
            for (int y = 0; y < H; y++) {
                int in = 0;
                for (int x = 0; x < W; x++) {
                    int ov = F[y*W+x] != B[y*W+x];
                    int g = ((x + sc[0]) & 4095) >> 4;
                    int d = ov && hc[y][g] != hp[y][g];
                    if (d && !in) runs++;
                    in = d;
                }
            }
            int gap = num - prevn;
            if (gap == 1) { sd += dirty; sgrp += ovg; sruns += runs; sopx += opx; n++; if (dirty > maxd) maxd = dirty; }
            ng++;
            if (a < 12 || a % 25 == 0) printf("%-14s %3d  %7d  %9d  %8d  %8d  %8d\n", b + 1, gap, sc[0], opx, ovg, dirty, runs);
        }
        memcpy(hp, hc, sizeof hp); have = 1; prevn = num;
        sall += opx;
    }
    printf("\nconsecutive pairs (gap 1): %d of %d; per update, BG1 on the hardware scroll:\n"
           "  overlay pixels %.0f (%.1f%% of 57344), groups with overlay %.0f of 4032,\n"
           "  dirty groups %.0f (%.1f%%; max %d), restore+redraw runs %.0f\n",
           n, ng, sopx / n, 100 * sopx / n / 57344, sgrp / n, sd / n, 100 * sd / n / 4032, maxd, sruns / n);
    return 0;
}

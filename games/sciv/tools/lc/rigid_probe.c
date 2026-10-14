/* Research probe: how much of a scrolling frame could reuse the previous frame by a hardware (whole-picture) scroll.
 *
 *   rigid_probe rom.sfc SNAP.BIN...      consecutive frames, oldest first
 *
 * Per frame pair (reference renderer, colour math off): the best global shift d (-16..16 pixels) of the previous picture, and
 * the 16-pixel groups (aligned on the screen) of the new picture that equal the old picture shifted by d. Groups that do not
 * match still need composing. The same per band (rows 0-30, 31-190, 191-223: the HDMA splits of stage 1-1, so a band of its own
 * could be placed by the walker at its own whole-group offset and its own shift). Pixel-exact equality: sprites that moved,
 * parallax and animated tiles break groups. Not a plan, an upper bound on what the Videl scroll can relieve. */
#define LCD_NO_MAIN
#include "lcd_model.c"

static const int band_y[4] = {0, 31, 191, 224};

static int group_match(const uint8_t *cur, const uint8_t *prev, int y, int g, int d)
{
    for (int k = 0; k < 16; k++) {
        int x = g * 16 + k, px = x - d;
        if (px < 0 || px >= PPU_W || cur[y * PPU_W + x] != prev[y * PPU_W + px]) return 0;
    }
    return 1;
}

static int best_shift(const uint8_t *cur, const uint8_t *prev, int y0, int y1, int *groups)
{
    int bd = 0, bg = -1;
    for (int d = -16; d <= 16; d++) {
        int n = 0;
        for (int y = y0; y < y1; y++)
            for (int g = 0; g < 16; g++) n += group_match(cur, prev, y, g, d);
        if (n > bg || (n == bg && abs(d) < abs(bd))) { bg = n; bd = d; }
    }
    *groups = bg;
    return bd;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s rom.sfc SNAP.BIN...\n", argv[0]); return 2; }
    long n;
    const uint8_t *rom = load(argv[1], &n);
    ppu_init();
    static uint8_t a[PPU_H * PPU_W], b[PPU_H * PPU_W];
    uint8_t *prev = a, *cur = b;
    int have = 0, pairs = 0, dh[33] = {0};
    long tot = 0, gmatch = 0, bmatch = 0, same0 = 0;
    for (int i = 2; i < argc; i++) {
        Snap *s = snap_load(argv[i], rom);
        render_nomath(s, 0, cur);
        free(s);
        if (have) {
            int g0, gm = 0, gb = 0;
            int d = best_shift(cur, prev, 0, PPU_H, &gm);
            best_shift(cur, prev, 0, PPU_H, &gm);
            for (int bnd = 0; bnd < 3; bnd++) { int gg; best_shift(cur, prev, band_y[bnd], band_y[bnd + 1], &gg); gb += gg; }
            g0 = 0;
            for (int y = 0; y < PPU_H; y++) for (int g = 0; g < 16; g++) g0 += group_match(cur, prev, y, g, 0);
            pairs++; dh[d + 16]++;
            tot += PPU_H * 16; gmatch += gm; bmatch += gb; same0 += g0;
            if (getenv("RIGID_V")) printf("%s d %3d  reuse: none-shift %4d global %4d per-band %4d of %d\n", argv[i], d, g0, gm, gb, PPU_H * 16);
        }
        uint8_t *t = prev; prev = cur; cur = t; have = 1;
    }
    printf("%d frame pairs; mean groups reusable of %ld: unshifted %ld, one global shift %ld, one shift per band %ld\n",
           pairs, pairs ? tot / pairs : 0L, pairs ? same0 / pairs : 0L, pairs ? gmatch / pairs : 0L, pairs ? bmatch / pairs : 0L);
    printf("best global shift histogram (pixels: count):");
    for (int d = -16; d <= 16; d++) if (dh[d + 16]) printf(" %d:%d", d, dh[d + 16]);
    printf("\n");
    return 0;
}

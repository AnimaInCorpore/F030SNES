/* Tile conversion for the true-colour renderer: see ppu_tc.h. Freestanding. */
#include "ppu_tc.h"

static uint16_t word(const uint8_t *page, int w)
{
    return (uint16_t)(page[w * 2] | page[w * 2 + 1] << 8);
}

/* The colour number of pixel x (0 left) of a row from its plane words. */
static int pixel(const uint16_t *planes, int n, int x)
{
    int bit = 7 - x, c = 0;
    for (int p = 0; p < n; p++) {
        c |= ((planes[p] >> bit) & 1) << (2 * p);
        c |= ((planes[p] >> (bit + 8)) & 1) << (2 * p + 1);
    }
    return c;
}

static uint32_t flags(const int *c)
{
    int n = 0;
    for (int x = 0; x < 8; x++) n += c[x] != 0;
    return n == 0 ? TC_CLEAR : n == 8 ? TC_SOLID : 0;
}

void tc_convert(const uint8_t *page, int fmt, uint32_t *out)
{
    for (int w = 0; w < 256; w++) out[w] = 0;
    if (fmt == TC_RAW) {
        for (int w = 0; w < 256; w++) out[w] = word(page, w);
        return;
    }
    if (fmt == TC_M7) {
        for (int w = 0; w < 256; w++) out[w] = tc_m7_p(64u * page[w * 2]) << 8 | page[w * 2 + 1];
        return;
    }
    if (fmt == TC_F2) {
        for (int w = 0; w < 256; w++) {
            uint16_t pl[1] = {word(page, w)};
            int c[8];
            uint32_t v = 0;
            for (int x = 0; x < 8; x++) {
                c[x] = pixel(pl, 1, x);
                v |= (uint32_t)c[x] << (21 - 3 * x);
            }
            out[w] = v;
        }
        return;
    }
    if (fmt == TC_F4) {
        for (int t = 0; t < 256; t += 16)
            for (int fy = 0; fy < 8; fy++) {
                uint16_t pl[2] = {word(page, t + fy), word(page, t + 8 + fy)};
                int c[8];
                uint32_t a = 0, b = 0;
                for (int x = 0; x < 8; x++) c[x] = pixel(pl, 2, x);
                for (int x = 0; x < 4; x++) {
                    a |= (uint32_t)c[x] << (19 - 5 * x);
                    b |= (uint32_t)c[x + 4] << (19 - 5 * x);
                }
                out[t + fy] = a | flags(c);
                out[t + 8 + fy] = b;
            }
        return;
    }
    if (fmt == TC_F8) {
        for (int t = 0; t < 256; t += 32)
            for (int fy = 0; fy < 8; fy++) {
                uint16_t pl[4] = {word(page, t + fy), word(page, t + 8 + fy), word(page, t + 16 + fy),
                                  word(page, t + 24 + fy)};
                int c[8];
                for (int x = 0; x < 8; x++) c[x] = pixel(pl, 4, x);
                for (int q = 0; q < 4; q++)
                    out[t + 8 * q + fy] = (uint32_t)c[2 * q] << 15 | (uint32_t)c[2 * q + 1] << 6;
                out[t + fy] |= flags(c);
            }
    }
}

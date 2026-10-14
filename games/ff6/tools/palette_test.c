/* Host check of the adaptive palette with synthetic colour sets:
 * exactness for <= 16 colours, determinism, slot stability across small
 * weight changes, and every shown colour mapped to its nearest slot. */
#include "palette.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t oklab[32768 * 3];

static int32_t d2(const int16_t *a, const int16_t *b)
{
    int32_t x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "build/smkx/oklab555.bin", "rb");
    uint8_t raw[32768 * 6];
    if (!f || fread(raw, 1, sizeof raw, f) != sizeof raw) { perror("oklab555.bin"); return 2; }
    fclose(f);
    for (int i = 0; i < 32768 * 3; i++) oklab[i] = (int16_t)(raw[i * 2] << 8 | raw[i * 2 + 1]);

    uint8_t cgram[512];
    uint32_t hist[256];
    int fails = 0;
    srand(7);

    /* 1: twelve colours are reproduced exactly */
    Palette p;
    memset(&p, 0, sizeof p);
    memset(hist, 0, sizeof hist);
    for (int i = 0; i < 256; i++) { uint16_t c = (uint16_t)(rand() & 0x7FFF); cgram[2 * i] = (uint8_t)c; cgram[2 * i + 1] = (uint8_t)(c >> 8); }
    for (int i = 0; i < 12; i++) hist[i * 7] = 100 + (uint32_t)i;
    palette_update(&p, hist, cgram, oklab);
    for (int i = 0; i < 12; i++) {
        int k = i * 7;
        uint16_t c = (uint16_t)(cgram[2 * k] | cgram[2 * k + 1] << 8);
        if (d2(p.lab[p.map[k]], &oklab[c * 3]) != 0) { fails++; printf("colour %d not exact\n", k); }
    }

    /* 2: 120 colours with skewed weights; each shown colour's slot is nearest */
    memset(hist, 0, sizeof hist);
    for (int i = 0; i < 120; i++) hist[i + 20] = (uint32_t)(1 + (rand() % 2000) * (i % 5 == 0 ? 20 : 1));
    Palette q = p;
    palette_update(&q, hist, cgram, oklab);
    for (int i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(cgram[2 * i] | cgram[2 * i + 1] << 8);
        const int16_t *lab = &oklab[(c & 0x7FFF) * 3];
        int s = q.map[i];
        for (int t = 0; t < PAL_COLOURS; t++)
            if (d2(q.lab[t], lab) < d2(q.lab[s], lab)) { fails++; printf("index %d not nearest\n", i); break; }
    }
    /* 3: determinism */
    Palette r = p;
    palette_update(&r, hist, cgram, oklab);
    if (memcmp(&q, &r, sizeof q)) { fails++; printf("not deterministic\n"); }
    /* 4: small weight jitter keeps the palette */
    for (int i = 20; i < 140; i++) if (hist[i] > 10) hist[i] += 1;
    Palette s = q;
    int changed = palette_update(&s, hist, cgram, oklab);
    if (changed || memcmp(s.lab, q.lab, sizeof s.lab)) { fails++; printf("jitter changed the palette\n"); }

    printf("palette checks: %d failures\n", fails);
    return fails != 0;
}

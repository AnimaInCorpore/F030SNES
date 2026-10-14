/* Adaptive 16-colour palette in OKLab: see palette.h. Freestanding. */
#include "palette.h"

#define MAXC 256

typedef struct {
    uint16_t c;          /* BGR555 */
    uint32_t w;          /* pixels */
    int16_t lab[3];
} Col;

static int32_t dist(const int16_t *a, const int16_t *b)
{
    int32_t d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return d0 * d0 + d1 * d1 + d2 * d2;
}

static void lab_of(const int16_t *oklab, uint16_t c, int16_t *out)
{
    const int16_t *e = oklab + (uint32_t)(c & 0x7FFF) * 3;
    out[0] = e[0];
    out[1] = e[1];
    out[2] = e[2];
}

static void rgb8(uint16_t c, int *r, int *g, int *b)
{
    int r5 = c & 31, g5 = (c >> 5) & 31, b5 = (c >> 10) & 31;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g5 << 3) | (g5 >> 2);
    *b = (b5 << 3) | (b5 >> 2);
}

/* Coarse weight class: colour sets whose weights only jitter keep the palette. */
static uint32_t weight_class(uint32_t w)
{
    uint32_t k = 0;
    while (w) { k++; w >>= 1; }
    return k;
}

static int nearest(const Palette *p, int n, const int16_t *lab)
{
    int best = 0;
    int32_t bd = 0x7FFFFFFF;
    for (int s = 0; s < n; s++) {
        int32_t d = dist(p->lab[s], lab);
        if (d < bd) { bd = d; best = s; }
    }
    return best;
}

int palette_update(Palette *p, const uint32_t hist[256], const uint8_t *cgram,
                   const int16_t *oklab)
{
    static Col cols[MAXC];
    int n = 0;
    uint32_t sig = 2166136261u;

    /* Distinct colours on screen with their pixel counts, in CGRAM order. */
    for (int i = 0; i < 256; i++) {
        if (!hist[i]) continue;
        uint16_t c = (uint16_t)((cgram[i * 2] | (cgram[i * 2 + 1] << 8)) & 0x7FFF);
        int k;
        for (k = 0; k < n; k++)
            if (cols[k].c == c) break;
        if (k == n) {
            cols[n].c = c;
            cols[n].w = 0;
            lab_of(oklab, c, cols[n].lab);
            n++;
        }
        cols[k].w += hist[i];
    }
    for (int k = 0; k < n; k++) {
        sig = (sig ^ cols[k].c) * 16777619u;
        sig = (sig ^ weight_class(cols[k].w)) * 16777619u;
    }

    int changed = 0;
    if (!p->valid || sig != p->signature) {
        changed = 1;
        int slots = n < PAL_COLOURS ? n : PAL_COLOURS;
        if (n <= PAL_COLOURS) {
            /* Few enough colours: each is its own slot, kept where it was. */
            uint8_t used[PAL_COLOURS] = {0};
            int8_t slot_of[MAXC];
            for (int k = 0; k < n; k++) {
                slot_of[k] = -1;
                if (!p->valid) continue;
                for (int s = 0; s < PAL_COLOURS; s++)
                    if (!used[s] && dist(p->lab[s], cols[k].lab) == 0) {
                        slot_of[k] = (int8_t)s;
                        used[s] = 1;
                        break;
                    }
            }
            for (int k = 0; k < n; k++) {
                if (slot_of[k] >= 0) continue;
                int s = 0;
                while (used[s]) s++;
                slot_of[k] = (int8_t)s;
                used[s] = 1;
            }
            for (int k = 0; k < n; k++) {
                int s = slot_of[k], r, g, b;
                p->lab[s][0] = cols[k].lab[0];
                p->lab[s][1] = cols[k].lab[1];
                p->lab[s][2] = cols[k].lab[2];
                rgb8(cols[k].c, &r, &g, &b);
                p->rgb[s][0] = (uint8_t)r;
                p->rgb[s][1] = (uint8_t)g;
                p->rgb[s][2] = (uint8_t)b;
            }
            if (!p->valid)
                for (int s = n; s < PAL_COLOURS; s++) {
                    p->lab[s][0] = p->lab[s][1] = p->lab[s][2] = 0;
                    p->rgb[s][0] = p->rgb[s][1] = p->rgb[s][2] = 0;
                }
        } else {
            /* Weighted k-means in OKLab, seeded from the previous palette
             * (stable slots) or, the first time, by weighted farthest point. */
            if (!p->valid) {
                int first = 0;
                for (int k = 1; k < n; k++)
                    if (cols[k].w > cols[first].w) first = k;
                for (int d = 0; d < 3; d++) p->lab[0][d] = cols[first].lab[d];
                for (int s = 1; s < PAL_COLOURS; s++) {
                    int pick = 0;
                    uint32_t best = 0;
                    for (int k = 0; k < n; k++) {
                        int32_t md = dist(p->lab[nearest(p, s, cols[k].lab)], cols[k].lab);
                        uint32_t score = (uint32_t)(md >> 8) * (weight_class(cols[k].w) + 1);
                        if (score > best) { best = score; pick = k; }
                    }
                    for (int d = 0; d < 3; d++) p->lab[s][d] = cols[pick].lab[d];
                }
            }
            static uint8_t member[MAXC];
            for (int iter = 0; iter < 4; iter++) {
                int32_t sum[PAL_COLOURS][3];
                uint32_t wsum[PAL_COLOURS];
                for (int s = 0; s < PAL_COLOURS; s++) {
                    sum[s][0] = sum[s][1] = sum[s][2] = 0;
                    wsum[s] = 0;
                }
                for (int k = 0; k < n; k++) {
                    int s = nearest(p, PAL_COLOURS, cols[k].lab);
                    member[k] = (uint8_t)s;
                    for (int d = 0; d < 3; d++) sum[s][d] += (int32_t)cols[k].w * cols[k].lab[d];
                    wsum[s] += cols[k].w;
                }
                for (int s = 0; s < PAL_COLOURS; s++) {
                    if (wsum[s]) {
                        for (int d = 0; d < 3; d++)
                            p->lab[s][d] = (int16_t)(sum[s][d] / (int32_t)wsum[s]);
                        continue;
                    }
                    /* An empty slot takes the colour its cluster serves worst. */
                    int pick = 0;
                    uint32_t best = 0;
                    for (int k = 0; k < n; k++) {
                        uint32_t e = (uint32_t)(dist(p->lab[member[k]], cols[k].lab) >> 8) *
                                     (weight_class(cols[k].w) + 1);
                        if (e > best) { best = e; pick = k; }
                    }
                    for (int d = 0; d < 3; d++) p->lab[s][d] = cols[pick].lab[d];
                }
            }
            /* Output colours: the weighted sRGB mean of each slot's members. */
            for (int s = 0; s < PAL_COLOURS; s++) {
                uint32_t r = 0, g = 0, b = 0, w = 0;
                for (int k = 0; k < n; k++) {
                    if (member[k] != s) continue;
                    int cr, cg, cb;
                    rgb8(cols[k].c, &cr, &cg, &cb);
                    r += cols[k].w * (uint32_t)cr;
                    g += cols[k].w * (uint32_t)cg;
                    b += cols[k].w * (uint32_t)cb;
                    w += cols[k].w;
                }
                if (w) {
                    p->rgb[s][0] = (uint8_t)(r / w);
                    p->rgb[s][1] = (uint8_t)(g / w);
                    p->rgb[s][2] = (uint8_t)(b / w);
                }
            }
        }
        (void)slots;
        p->signature = sig;
        p->valid = 1;
    }

    /* Every CGRAM index, shown or not, maps to its nearest slot. */
    int live = n < PAL_COLOURS ? (n ? n : 1) : PAL_COLOURS;
    if (n < PAL_COLOURS && p->valid) live = PAL_COLOURS;
    for (int i = 0; i < 256; i++) {
        int16_t lab[3];
        lab_of(oklab, (uint16_t)(cgram[i * 2] | (cgram[i * 2 + 1] << 8)), lab);
        p->map[i] = (uint8_t)nearest(p, live, lab);
    }
    return changed;
}

void palette_scaled(const Palette *p, int brightness, uint32_t out[PAL_COLOURS])
{
    for (int s = 0; s < PAL_COLOURS; s++) {
        uint32_t r = p->rgb[s][0] * (uint32_t)(brightness + 1) >> 4;
        uint32_t g = p->rgb[s][1] * (uint32_t)(brightness + 1) >> 4;
        uint32_t b = p->rgb[s][2] * (uint32_t)(brightness + 1) >> 4;
        out[s] = (r << 16) | (g << 8) | b;
    }
}

void palette_save(const Palette *p, uint8_t *o)
{
    for (int s = 0; s < PAL_COLOURS; s++)
        for (int d = 0; d < 3; d++) {
            *o++ = (uint8_t)((uint16_t)p->lab[s][d] >> 8);
            *o++ = (uint8_t)p->lab[s][d];
        }
    for (int s = 0; s < PAL_COLOURS; s++)
        for (int d = 0; d < 3; d++) *o++ = p->rgb[s][d];
    for (int i = 0; i < 256; i++) *o++ = p->map[i];
    *o++ = (uint8_t)(p->signature >> 24);
    *o++ = (uint8_t)(p->signature >> 16);
    *o++ = (uint8_t)(p->signature >> 8);
    *o++ = (uint8_t)p->signature;
    *o++ = p->valid;
    *o = 0;
}

void palette_load(Palette *p, const uint8_t *in)
{
    for (int s = 0; s < PAL_COLOURS; s++)
        for (int d = 0; d < 3; d++, in += 2)
            p->lab[s][d] = (int16_t)(in[0] << 8 | in[1]);
    for (int s = 0; s < PAL_COLOURS; s++)
        for (int d = 0; d < 3; d++) p->rgb[s][d] = *in++;
    for (int i = 0; i < 256; i++) p->map[i] = *in++;
    p->signature = (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
    p->valid = in[4];
}

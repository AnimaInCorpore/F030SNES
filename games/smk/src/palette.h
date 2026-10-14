/* Adaptive 16-colour palette in OKLab for the translated SMK program.
 *
 * Each frame the renderer counts how many pixels use each CGRAM index. From
 * those counts and CGRAM the quantizer picks 16 colours (pixel-weighted
 * k-means in OKLab, seeded from the previous palette, slots kept stable)
 * and maps every CGRAM index to its nearest colour. Screen brightness
 * scales the 16 output colours, not the quantizer's input, so fades cost
 * nothing. Integer-only and deterministic: the Falcon and the host produce
 * identical palettes from identical inputs.
 *
 * The OKLab table (32768 entries: L, a, b as int16, L in 0..4096) comes from
 * tools/smkx/oklab.py; the Falcon links it, the host loads it. */
#ifndef PALETTE_H
#define PALETTE_H

#include <stdint.h>

#define PAL_COLOURS 16

typedef struct {
    int16_t lab[PAL_COLOURS][3];      /* slot centroids in OKLab */
    uint8_t rgb[PAL_COLOURS][3];      /* slot colours, 8 bits per channel */
    uint8_t map[256];                 /* CGRAM index -> slot */
    uint32_t signature;               /* of the weighted colour set last used */
    uint8_t valid;                    /* 0 until the first update */
} Palette;

/* Recomputes the palette when the weighted colour set changed.
 * hist[i] = pixels that showed CGRAM index i; cgram = 512 bytes BGR555.
 * oklab = the 32768 x 3 table. Returns 1 when the palette changed. */
int palette_update(Palette *p, const uint32_t hist[256], const uint8_t *cgram,
                   const int16_t *oklab);

/* Serialised Palette in snapshots: lab int16[16][3], rgb[16][3], map[256],
 * signature u32, valid u8, pad u8 -- big-endian. */
#define PAL_SNAP_BYTES (96 + 48 + 256 + 4 + 2)
void palette_save(const Palette *p, uint8_t *out);
void palette_load(Palette *p, const uint8_t *in);

/* The 16 colours scaled by SNES brightness 0-15, as 0xRRGGBB. */
void palette_scaled(const Palette *p, int brightness, uint32_t out[PAL_COLOURS]);

#endif

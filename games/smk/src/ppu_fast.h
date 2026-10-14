/* The fast row composer (src/ppu_fast.c): the same palette slots and
 * histogram counts as the reference renderer, composed a character row at
 * a time. */
#ifndef PPU_FAST_H
#define PPU_FAST_H

#include "snes_ppu.h"

/* Composes one frame like ppu_render(), byte for byte. */
void ppu_fast_render(const PpuInput *in, uint8_t *out);

/* The tile-row cache (decoded character rows) holds on to VRAM contents:
 * drop a 256-word page after writing it, or everything. */
void ppu_fast_invalidate_page(int page);
void ppu_fast_invalidate_all(void);

/* Palette slots (0-15) to 4-plane words like ppu_planar(), 16 pixels at a
 * time through a merge network; groups = pixels / 16. */
void ppu_c2p4(const uint8_t *chunky, uint16_t *planar, int groups);

#endif

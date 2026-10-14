/* Offline investigation only: provenance from the unmodified reference PPU. */
#ifndef OCCLUSION_REF_H
#define OCCLUSION_REF_H
#include "snes_ppu.h"
typedef struct {
    uint8_t pixels[PPU_H][PPU_W];
    uint8_t main_used[PPU_H][PPU_W];
    uint8_t both_used[PPU_H][PPU_W]; /* conservatively retain both half-blend inputs */
    uint8_t layers[PPU_H], blend[PPU_H];
    uint8_t mode[PPU_H];
    uint16_t opaque[4][PPU_H][16];
} OcclusionFrame;
void occlusion_reference(const PpuInput *in, OcclusionFrame *out);
#endif

/* Internals of the reference renderer (snes_ppu.c) shared with the fast
 * row composer (ppu_fast.c): the per-line register state and the pieces
 * both use, so the two agree on everything but how a row is drawn. */
#ifndef PPU_INTERNAL_H
#define PPU_INTERNAL_H

#include "snes_ppu.h"

typedef PpuRegs Regs;

/* Composes one visible row (0-223) from the line's register state. */
typedef void (*PpuComposeFn)(const PpuInput *in, const Regs *s, int row, uint8_t *out);

/* The frame loop (HDMA, IRQ-logged writes, OAM segments) with a given row
 * composer; ppu_render uses the reference one, ppu_ref_row. */
void ppu_render_rows(const PpuInput *in, uint8_t *out, PpuComposeFn row);
void ppu_ref_row(const PpuInput *in, const Regs *s, int row, uint8_t *out);

/* Sprites of one row: CGRAM index in bits 0-7, priority 0-3 in bits 8-9,
 * bit 10 = opaque; 32-per-line and 34-sliver limits applied. */
void ppu_obj_line(const PpuInput *in, const Regs *s, int row, uint16_t *out);

#endif

/* The DSP's true-colour renderer (falcon/dsp/tc.asm): RGB565 pixels for
 * the Videl's true-colour mode, every SNES colour exact (colour math per
 * pixel, the sub screen, windows, brightness, CGRAM changed mid-frame).
 *
 * Reference: ppu_render_565() (snes_ppu.h). The host model
 * (tools/tc_model.c) is the DSP program's executable description.
 *
 * Tiles are converted once, when a VRAM page goes to the DSP, into rows of
 * pixel fields the DSP reads without bit-plane work. A converted page keeps
 * every character row at its VRAM word address, so a character's rows are
 * found as in VRAM (chrbase + chr * words + fy):
 * Every field has a zero top bit (the DSP's multiply that shifts it out is
 * signed):
 *   TC_F2  2 bpp: one word per row (VRAM word w: planes 0/1 of a row):
 *          8 fields of 3 bits, pixel i's value << (21 - 3i); no flags (a
 *          row is clear when its word is 0).
 *   TC_F4  4 bpp: the row's planes 0/1 word (+fy) becomes pixels 0-3, its
 *          planes 2/3 word (+8 + fy) pixels 4-7, as 5-bit fields (pixel 0
 *          in bits 23-19 ... pixel 3 in 8-4); bits 3-0 of the first word
 *          the row's flags (of the second 0).
 *   TC_F8  8 bpp: word +8q + fy holds pixels 2q and 2q + 1 as 9-bit
 *          fields (<< 15, << 6); bits 5-0 of the first word (q = 0) the
 *          row's flags.
 *   TC_RAW the VRAM words (tile maps).
 *   TC_M7  the Mode 7 plane (pages 0-63, DSP_KERNEL_TC7 only): plane
 *          page p always goes into slot p (tc_slot_p), so plane word w
 *          sits at tc_m7_p(w); word w becomes tc_m7_p(64 * t) << 8 | texel
 *          byte (t its map byte): a map entry gives its character's P
 *          address at once.
 * Flags (TC_F4, TC_F8): TC_CLEAR every pixel of the row is 0 (it draws
 * nothing: half of all tile rows in CT's demo), TC_SOLID none is.
 * Pixel values in the fields are the character's colour numbers (0 is
 * transparent), before the palette.
 *
 * Stream (dsp_plan with DspPlan.tc set; DSP_KERNEL_TC, or DSP_KERNEL_TC7
 * for frames whose visible rows are all Mode 7):
 *   DSP_CMD_FRAME
 *   DSP_NSLOT_TC slots at row 0 (ppu_dsp.h: the tile slots, the window
 *     slots, SUB (unused), TS, TSW, MATH = CGWSEL | CGADSUB << 8,
 *     FIXED = r | g << 5 | b << 10)
 *   nchange, per change row << 8 | slot, value; slot DSP_SLOT_CG: CGRAM
 *     entry index << 16 | its BGR555 colour (mid-frame CGRAM writes)
 *   nseg and the OAM segments (ppu_dsp.h)
 *   the offset-per-tile table: 0, or 66 and the words (BG modes 2, 4, 6)
 *   256 words: CGRAM at row 0, BGR555
 *   TC7 only: the Mode 7 section (ppu_dsp.h)
 * and DspPlan.fmt[page] says which conversions of each needed page the
 * frame reads (bit per TC_F2, TC_F4, TC_F8, TC_RAW).
 */
#ifndef PPU_TC_H
#define PPU_TC_H

#include <stdint.h>

enum { TC_F2 = 1, TC_F4 = 2, TC_F8 = 4, TC_RAW = 8, TC_M7 = 16 };
enum { TC_CLEAR = 1, TC_SOLID = 2 };

/* The programs' converted page slots (falcon/dsp/tc.asm): 26 in
 * P:$2600-$3FFF, 48 in P:$5000-$7FFF (TC7 all 74, the tile program 72);
 * the stream holds 2048 words. */
#define TC_SLOTS 74
/* The tile program keeps its last two slots (P:$7E00-$7FFF) for its own code. */
#define TC_SLOTS_TILE 72
#define TC_STREAM_MAX 2048
static inline uint32_t tc_slot_p(int k)
{
    return k < 26 ? 0x2600u + (uint32_t)k * 256 : 0x5000u + (uint32_t)(k - 26) * 256;
}

/* The P address of Mode 7 plane word w (0-$3FFF): pages 0-25 in the first
 * slots, the rest from P:$5000. */
static inline uint32_t tc_m7_p(uint32_t w)
{
    return w < 0x1A00 ? 0x2600u + w : 0x3600u + w;
}

/* One 256-word VRAM page (VRAM order, little-endian words as the SNES
 * keeps them: vram + page * 512 bytes) converted to fmt: 256 words. */
void tc_convert(const uint8_t *page, int fmt, uint32_t *out);

#endif

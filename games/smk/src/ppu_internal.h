/* Internals of the reference renderer (snes_ppu.c) shared with the fast
 * row composer (ppu_fast.c): the per-line register state and the pieces
 * both use, so the two agree on everything but how a row is drawn. (The
 * split follows F030SMW's.) */
#ifndef PPU_INTERNAL_H
#define PPU_INTERNAL_H

#include "snes_ppu.h"

typedef struct {
    uint8_t r[0x100];          /* $2100-$21FF as last written */
    uint16_t bgofs[8];
    int16_t m7[6];
    uint16_t m7ofs[2];
    uint8_t bgold, m7old;
    uint8_t cgram[512];
    uint16_t cgaddr;
    uint8_t cglatch, cgflip;
    const uint8_t *oam;        /* OAM for the current screen segment */
    uint32_t dirty;            /* ppu_dsp.c's fields written (bit per field,
                                  bit 31: OBSEL); all set at the frame start */
} Regs;

/* One HDMA channel as the frame loop runs it. */
typedef struct {
    uint8_t active, dotransfer, repeat, count;
    uint8_t n, ind, breg;      /* bytes per unit, indirect, B-bus register */
    int8_t m7i;                /* mode 2 into $211B-$2120: that m7[] index */
    const uint8_t *off;        /* the unit's register offsets */
    uint32_t table;            /* A-bus address of the table pointer */
    uint32_t indirect;         /* A-bus address of indirect data */
    const uint8_t *tp, *ip;    /* host pointers at table / indirect, */
    uint32_t tlen, ilen;       /* valid for this many more bytes */
} Hdma;

/* The frame loop's state: registers, HDMA channels, the next IRQ write and
 * OAM segment. ppu_frame_line(line) applies what takes effect at that line
 * (IRQ writes, OAM segment, HDMA); row line - 1 is then composed from s.
 * ppu_render_rows is start + lines 0..PPU_H. */
typedef struct {
    Regs s;
    Hdma h[8];
    int logi, segi;
} PpuFrame;
void ppu_frame_start(const PpuInput *in, PpuFrame *f);
void ppu_frame_line(const PpuInput *in, PpuFrame *f, int line);

/* Composes one visible row (0-223) from the line's register state. */
typedef void (*PpuRowFn)(const PpuInput *in, const Regs *s, int row, uint8_t *out);

/* The frame loop (HDMA, IRQ-logged writes, OAM segments) with a given row
 * composer; ppu_render uses the reference one, ppu_ref_row. */
void ppu_render_rows(const PpuInput *in, uint8_t *out, PpuRowFn row);
void ppu_ref_row(const PpuInput *in, const Regs *s, int row, uint8_t *out);

/* Sprites of one row: CGRAM index in bits 0-7, priority 0-3 in bits 8-9,
 * bit 10 = opaque; 32-per-line and 34-sliver limits applied. */
void ppu_obj_line(const PpuInput *in, const Regs *s, int row, uint16_t *out);

/* Windows: where layer (0-3 BG1-4, 4 OBJ) is hidden on the main screen,
 * as up to three spans [x, x + len); ppu_win_layers: the layers (bit i)
 * TM shows that have spans on this row. */
int ppu_win_spans(const Regs *s, int layer, uint16_t span[3][2]);
int ppu_win_layers(const Regs *s);

/* Mode 7: the row's start coordinates and steps, and one texel. */
void ppu_mode7_setup(const Regs *s, int row, PpuM7Line *l);

static inline uint8_t ppu_m7_texel(const uint8_t *vram, uint8_t sel, int32_t vx, int32_t vy)
{
    int tx = vx >> 8, ty = vy >> 8;
    uint8_t tile;
    if (((tx | ty) & ~1023) && (sel & 0x80)) {
        if (!(sel & 0x40)) return 0;
        tile = 0;
    } else {
        tile = vram[(((ty & 1023) >> 3) * 128 + ((tx & 1023) >> 3)) * 2];
    }
    return vram[(tile * 64 + (ty & 7) * 8 + (tx & 7)) * 2 + 1];
}

#ifdef PPU_PROFILE
#define PROF_START() uint32_t prof_t = ppu_prof_clock()
#define PROF_MARK(i) do { uint32_t prof_u = ppu_prof_clock(); \
                          ppu_prof[i] += prof_u - prof_t; prof_t = prof_u; } while (0)
#else
#define PROF_START() do {} while (0)
#define PROF_MARK(i) do {} while (0)
#endif

#endif

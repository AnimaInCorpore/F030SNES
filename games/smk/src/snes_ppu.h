/* SNES picture composition for the translated SMK program.
 *
 * One source serves two builds: the host reference (tests, snapshot
 * comparison) and the Falcon runtime (m68k-atari-mintelf-gcc, freestanding).
 * The output is a 256x224 frame of palette slots (PpuInput.map); ppu_planar packs
 * it into the Videl's four interleaved bitplanes.
 *
 * Covered: BG modes 0, 1 and 7; sprites with the 32-per-line and 34-sliver
 * limits; HDMA per scanline; register writes a mid-frame IRQ made, applied
 * at their line. Colour: every pixel is its CGRAM index mapped through the
 * adaptive palette's slot table (palette.h); brightness and forced blank act
 * on the 16 palette colours, not on pixels. Not yet: colour math, windows,
 * mosaic, modes 2-6, hi-res and interlace.
 */
#ifndef SNES_PPU_H
#define SNES_PPU_H

#include <stdint.h>

#define PPU_W 256
#define PPU_H 224

/* OAM as a mid-frame IRQ left it, used from scanline `line` on. */
#define PPU_OAM_BYTES 544
#define PPU_OAMSEG_MAX 9
typedef struct {
    uint16_t line;
    uint16_t pad;
    uint8_t oam[PPU_OAM_BYTES];
} PpuOamSeg;

/* A register write the runtime logged during a mid-frame IRQ. */
typedef struct {
    uint16_t line;   /* scanline the write takes effect from */
    uint16_t reg;    /* B-bus register, $2100-$21FF */
    uint8_t value;
    uint8_t pad;
} PpuWrite;

typedef struct {
    const uint8_t *vram;      /* 64 KiB, little-endian words */
    const uint8_t *cgram;     /* 512 bytes, BGR555 little-endian */
    const uint8_t *oam;       /* 544 bytes, when there are no segments */
    const uint8_t *io;        /* shadow of $2000-$7FFF: io[addr - 0x2000] */
    const uint16_t *bgofs;    /* BG1-4 H/V scroll as latched: [bg*2 + v] */
    const uint16_t *m7;       /* Mode 7 A, B, C, D, X, Y as latched */
    const uint16_t *m7ofs;    /* Mode 7 H/V scroll as latched */
    /* A-bus byte read for HDMA tables (ROM or WRAM). */
    uint8_t (*abus)(uint32_t address, void *ctx);
    void *ctx;
    const PpuWrite *log;      /* IRQ writes, ascending line */
    int nlog;
    const uint8_t *map;       /* CGRAM index -> palette slot (0-15) */
    uint32_t *hist;           /* out: pixels per CGRAM index (added to):
                                 every 16th pixel of a row counts 16 (the
                                 DSP renderer sends those samples) */
    const struct PpuM7Sink *m7sink;   /* NULL: every row composed here */
    const PpuOamSeg *oamseg;  /* OAM per screen segment, ascending line */
    int noamseg;              /* 0: oam for the whole frame */
    /* Optional: the host address of A-bus byte a and how many bytes follow
     * it contiguously (to the end of its region and bank), or NULL where
     * abus must be asked byte by byte. HDMA reads its tables through it. */
    const uint8_t *(*aptr)(uint32_t address, void *ctx, uint32_t *len);
} PpuInput;

/* HiROM A-bus reads for HDMA tables: WRAM, SRAM and the 512 KiB ROM. */
typedef struct {
    const uint8_t *wram;      /* 128 KiB */
    const uint8_t *sram;      /* 2 KiB */
    const uint8_t *rom;       /* 512 KiB */
} PpuBus;
uint8_t ppu_abus_hirom(uint32_t address, void *bus);
const uint8_t *ppu_aptr_hirom(uint32_t address, void *bus, uint32_t *len);

/* Snapshot file ("SMKXSNAP", version 3), written by the Falcon runtime at
 * its frame limit; all multi-byte fields big-endian:
 *   magic[8] version u32
 *   vram[65536] cgram[512] (as the frame started) oam[544] io[0x6000]
 *   bgofs u16[8] m7 u16[6] m7ofs u16[2]
 *   nlog u32, then 256 x {line u16, reg u16, value u8, pad u8}
 *   wram[131072] sram[2048]
 *   planar[28672]    the frame the Falcon composed from these inputs
 *   palette before   (PAL_SNAP_BYTES) the state the frame was mapped with
 *   palette after    the state palette_update() produced from its counts
 *   noamseg u32, then PPU_OAMSEG_MAX x {line u16, pad u16, oam[544]} */
#define PPU_SNAP_VERSION 3

/* Kept for callers; the palette lives in palette.h. */
void ppu_init(void);

/* Composes one frame into out[PPU_H][PPU_W] (palette slots 0-15). */
void ppu_render(const PpuInput *in, uint8_t *out);

/* Packs a chunky frame into Videl four-plane words: for each 16-pixel
 * group, plane 0..3, most significant bit leftmost. */
void ppu_planar(const uint8_t *chunky, uint16_t *planar);

/* One Mode 7 scanline, as the frame planner hands it to the pixel stage
 * (the DSP kernel on the Falcon): texel coordinates in 1/256 units at the
 * line's first pixel, their per-pixel steps, and M7SEL. */
typedef struct {
    int32_t vx, vy;       /* at x = 0 (after the screen flip) */
    int32_t a, c;         /* per-pixel steps; negated when M7SEL flips x */
    uint8_t sel;          /* M7SEL: bits 7-6 outside handling */
} PpuM7Line;

/* The pixel stage: 256 CGRAM indices for the line, 0 = transparent. This
 * is the reference the DSP kernel must reproduce byte for byte. */
void ppu_m7_pixels(const uint8_t *vram, const PpuM7Line *l, uint8_t *cgidx);

/* Stage profile, compiled with -DPPU_PROFILE only: ppu_render adds the
 * time of each stage (in the units of ppu_prof_clock) to ppu_prof[]. */
enum { PPU_PROF_HDMA, PPU_PROF_BG, PPU_PROF_M7, PPU_PROF_OBJ, PPU_PROF_COMPOSE,
       PPU_PROF_M7FEED, PPU_PROF_STAGES };
#ifdef PPU_PROFILE
extern uint32_t (*ppu_prof_clock)(void);
extern uint32_t ppu_prof[PPU_PROF_STAGES];
#endif

/* An external pixel stage for Mode 7 (the DSP kernel on the Falcon). For a
 * Mode 7 row with BG1 on, ppu_render hands the stage the row's line and
 * writes PPU_EXT wherever the stage's pixel shows (BG1 or, where BG1 is
 * transparent, the backdrop) and a palette slot where a sprite wins. Every
 * other row is composed here and reported through cpu_row. Rows arrive in
 * order, 0 to PPU_H - 1. */
#define PPU_EXT 0xFF
typedef struct PpuM7Sink {
    void (*m7_row)(void *ctx, int row, const PpuM7Line *l);
    void (*cpu_row)(void *ctx, int row);
    void *ctx;
} PpuM7Sink;


#endif

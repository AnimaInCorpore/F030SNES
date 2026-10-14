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
    uint32_t *hist;           /* out: pixels per CGRAM index (added to);
                                 Mode 7 rows count every 8th pixel x 8 */
    const struct PpuM7Sink *m7sink;   /* NULL: every row composed here */
    const PpuOamSeg *oamseg;  /* OAM per screen segment, ascending line */
    int noamseg;              /* 0: oam for the whole frame */
    /* Optional: the host address of A-bus byte a and how many bytes follow
     * it contiguously (to the end of its region and bank), or NULL where
     * abus must be asked byte by byte. HDMA reads its tables through it. */
    const uint8_t *(*aptr)(uint32_t address, void *ctx, uint32_t *len);
    /* Optional: per 16-entry CGRAM block, the block a sub-screen pixel of
     * it is shown from on rows that add the sub screen (0xFF: its own),
     * from the planner (DspPlan.subreloc; dsp_sub_palette fills those
     * blocks with lightened copies). NULL: none. */
    const uint8_t *subreloc;
} PpuInput;

/* HiROM A-bus reads for HDMA tables: WRAM, SRAM and the ROM (CT: 4 MiB,
 * 8 KiB SRAM). */
typedef struct {
    const uint8_t *wram;      /* 128 KiB */
    const uint8_t *sram;      /* CT: 8 KiB; SMW: 2 KiB; YI: the GSU's RAM, 32 KiB */
    const uint8_t *rom;       /* CT: 4 MiB; SMW: 512 KiB; YI: 2 MiB */
} PpuBus;
uint8_t ppu_abus_hirom(uint32_t address, void *bus);
const uint8_t *ppu_aptr_hirom(uint32_t address, void *bus, uint32_t *len);
uint8_t ppu_abus_lorom(uint32_t address, void *bus);                           /* SMW */
const uint8_t *ppu_aptr_lorom(uint32_t address, void *bus, uint32_t *len);
uint8_t ppu_abus_yi(uint32_t address, void *bus);                              /* YI: SuperFX cartridge */
const uint8_t *ppu_aptr_yi(uint32_t address, void *bus, uint32_t *len);

/* Snapshot file ("SMKXSNAP", version 5: YI's own layout), written by the Falcon runtime; all multi-byte
 * fields big-endian:
 *   magic[8] version u32
 *   vram[65536] cgram[512] (as the frame started) oam[544] io[0x6000]
 *   bgofs u16[8] m7 u16[6] m7ofs u16[2]
 *   nlog u32, then 256 x {line u16, reg u16, value u8, pad u8}
 *   wram[131072] gsuram[32768]
 *   planar[57344]    the frame the Falcon showed (eight planes: CGRAM indices)
 *   noamseg u32, then PPU_OAMSEG_MAX x {line u16, pad u16, oam[544]}
 * (F030CT, SMK, SMW and DKC have sram[8192] or [2048], a `planes` field and two palette blocks.) */
#define PPU_SNAP_VERSION 5
#define PPU_SNAP_PLANAR 57344
/* The size of a snapshot's frame by its planes field. */
#define ppu_snap_frame(planes) ((planes) == 16 ? 2 * PPU_SNAP_PLANAR : PPU_SNAP_PLANAR)

/* The PPU registers as the frame's HDMA and IRQ writes leave them at one
 * row (ppu_replay). */
typedef struct {
    uint8_t r[0x100];          /* $2100-$21FF as last written */
    uint16_t bgofs[8];
    int16_t m7[6];
    uint16_t m7ofs[2];
    uint8_t bgold, m7old;
    uint8_t cgram[512] __attribute__((aligned(4)));
    uint16_t cgaddr;
    uint8_t cglatch, cgflip;
    uint8_t fixed[3];          /* COLDATA's red, green, blue (0-31) */
    uint16_t cgwrites;         /* CGRAM entries changed so far this frame */
    const uint8_t *oam;        /* OAM for the current screen segment */
    uint8_t changed;           /* in ppu_replay's callback: 1 when a register
                                  or the OAM segment changed since the last
                                  row (always on row 0) */
} PpuRegs;

/* Runs the frame's register replay (HDMA, IRQ log, OAM segments) and calls
 * fn for rows 0 to PPU_H - 1 with the registers that row is composed with.
 * ppu_render is this plus the composition. */
typedef void (*PpuRowFn)(void *ctx, int row, const PpuRegs *s);
void ppu_replay(const PpuInput *in, PpuRowFn fn, void *ctx);
/* The same, but fn only for row 0 and the rows where a register or the OAM
 * segment changed (the rows between keep the registers of the last call). */
void ppu_replay_changes(const PpuInput *in, PpuRowFn fn, void *ctx);
/* Set by ppu_replay_changes when an HDMA table or its indirect data came from RAM (conservatively: WRAM, SRAM, the
 * low half of the system banks); otherwise the replay depends on the registers and the IRQ log alone. */
extern int ppu_replay_ram;
/* For the list compositor: leave CGRAM writes out of the replay; subtract this from every BG horizontal scroll the callback sees. */
extern int ppu_replay_nocg, ppu_replay_dx;

/* Host tools (tools/win_census.c): 1 walks HDMA channels on every
 * register up to $2133 (windows, colour math), not only those the
 * renderer reads. */
extern int ppu_walk_all;

/* The frame as the SNES shows it: main and sub screen, colour math
 * (CGWSEL, CGADSUB, the fixed colour), clip to black, the colour window,
 * main- and sub-screen windows, brightness; out gets 0xRRGGBB per pixel.
 * The reference for what the Falcon approximates. The runtime keeps the
 * fixed colour at $21F0-$21F2 (io[0x1F0..0x1F2]). */
void ppu_render_rgb(const PpuInput *in, uint32_t *out);

/* The same picture as the Videl's true colour shows it (ppu_tc, the DSP's
 * true-colour renderer): RGB565 per pixel, brightness applied to the 5-bit
 * components as c * (brightness + 1) >> 4, green widened to 6 bits as
 * g << 1 | g >> 4. */
void ppu_render_565(const PpuInput *in, uint16_t *out);

/* Kept for callers; the palette lives in palette.h. */
void ppu_init(void);

/* Composes one frame into out[PPU_H][PPU_W] (palette slots 0-15). */
void ppu_render(const PpuInput *in, uint8_t *out);

/* Packs a chunky frame into Videl four-plane words: for each 16-pixel
 * group, plane 0..3, most significant bit leftmost. */
void ppu_planar(const uint8_t *chunky, uint16_t *planar);

/* Packs a chunky frame of CGRAM indices into Videl eight-plane words: for
 * each 16-pixel group, plane 0..7. */
void ppu_planar8(const uint8_t *chunky, uint16_t *planar);

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

/* The line a Mode 7 row samples, from the registers at that row. */
void ppu_m7_setup(const PpuRegs *s, int row, PpuM7Line *l);

/* Main-screen windows at a row: where layer (0-3 BG1-4, 4 OBJ) is hidden,
 * at most three spans [x, x + len), ascending (0: nowhere); and the TM
 * layers that have spans (bit per layer). Colour windows are not
 * modelled. */
int ppu_win_spans(const PpuRegs *s, int layer, uint16_t span[3][2]);
int ppu_win_layers(const PpuRegs *s);
/* The same for any mask register (on: TMW or TSW), and layer 5, the colour
 * window (on unused): its spans are where it is "inside". */
int ppu_win_spans_on(const PpuRegs *s, int layer, uint8_t on, uint16_t span[3][2]);

/* Offset-per-tile (BG modes 2, 4 and 6) for a strip of 33 eight-pixel
 * slivers of layer bg (0 or 1) from the 8-pixel boundary at or left of
 * pixel 0, as the DSP renderer builds its rows: per sliver the word
 * H on << 23 | V on << 22 | V << 10 | H (10 bits each, the scroll values
 * that replace the registers'); 0 where the registers apply, as in
 * sliver 0 (the leftmost column is never offset). */
void ppu_opt_slivers(const PpuInput *in, const PpuRegs *s, int bg, uint32_t out[33]);

/* The sub screen as the Falcon approximates it, per row: 0 (also where
 * no shown main layer nor the backdrop takes part), or TS | the
 * colour math layers (CGADSUB 5-0) << 8 | 1 << 16, 1 << 17 where the
 * backdrop is black and not halved (backdrop + sub = sub, exact), 1 << 18
 * where it adds without halving: the checkerboard then takes only sub
 * pixels of the CGRAM blocks PpuInput.subreloc relocates, shown from
 * their lightened copies (dim blocks add little: the main pixel stays).
 * Only for
 * tile-mode rows that add the sub screen with colour math always on and
 * nothing clipped. Such a row shows the sub screen's pixel wherever the
 * main pixel's layer takes part in colour math (OBJ: palettes 4-7), the
 * sub pixel is opaque and x + row is even (a checkerboard: translucency,
 * where the SNES adds or averages); on every such backdrop pixel with bit
 * 17. The main screen's windows apply to both screens. cgram: the palette
 * the frame is shown with (its entry 0, the backdrop). */
uint32_t ppu_sub_cfg(const PpuRegs *s, const uint8_t *cgram);

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

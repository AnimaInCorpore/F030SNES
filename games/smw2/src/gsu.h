/* SuperFX (GSU-2) core: the reference for YI's SuperFX program.
 *
 * Runs synchronously: the runtime starts it when the 65816 writes R15's
 * high byte and it runs until STOP. Instruction semantics, the one-byte
 * pipeline (the instruction after every write to R15 executes before the
 * jump takes effect), the ROM/RAM buffers and the pixel caches follow the
 * documented GSU behaviour as bsnes's SuperFX core models it; timing is not
 * modelled. Portable C: the host checks and the Falcon runtime use the
 * same file.
 */
#ifndef GSU_H
#define GSU_H

#include <stdint.h>

#define GSU_RAM_BYTES 0x8000u        /* YI: 32 KiB, mirrored in $70-$71 */
#define GSU_RAM_MASK (GSU_RAM_BYTES - 1u)

typedef struct {
    uint8_t data[8];
    uint16_t offset;
    uint8_t bitpend;
} GsuPixelCache;

typedef struct Gsu {
    uint16_t r[16];
    /* SFR as separate flags */
    uint8_t z, cy, s, ov, g, alt1, alt2, b, irq;
    uint8_t sreg, dreg;
    uint8_t pbr, rombr, rambr;
    uint8_t cfgr, scbr, clsr, scmr, por, colr, bramr;
    uint16_t cbr;
    uint8_t pipeline;
    uint8_t r15_written, r14_written;
    uint16_t ramaddr;
    GsuPixelCache pc[2];
    const uint8_t *rom;          /* 2 MiB */
    uint8_t *ram;                /* GSU_RAM_BYTES */
    uint8_t cache[512];
    uint32_t steps;              /* instructions executed, all runs */
    uint32_t last_steps;         /* instructions of the last run */
    uint16_t start_pc;
    uint8_t start_pbr;
    uint8_t fault;               /* 1: step limit hit; translated code: 2 unsupported
                                    node, 3 target not translated, 4 far jump */
} Gsu;

void gsu_init(Gsu *g, const uint8_t *rom, uint8_t *ram);
/* The 65816 side: $3000-$34FF (the caller passes addr & $FFFF). */
uint8_t gsu_read_reg(Gsu *g, uint16_t addr);
/* Returns 1 when the write started the GSU (R15 high byte). */
int gsu_write_reg(Gsu *g, uint16_t addr, uint8_t data);
/* Run until STOP (or the step limit, 0 = none). */
void gsu_run(Gsu *g, uint32_t limit);

/* For the translated program (tools/smkx/gsu2c.py): */
uint8_t gsu_color(Gsu *g, uint8_t src);
void gsu_plot(Gsu *g, uint8_t x, uint8_t y);
uint8_t gsu_rpix(Gsu *g, uint8_t x, uint8_t y);
/* host bytes of a ROM (or RAM) bank as the GSU sees it: p[addr & *mask] */
const uint8_t *gsu_rom_view(Gsu *g, uint8_t bank, uint16_t *mask);
/* Continue in the interpreter with the opcode at pc next. */
void gsu_resume_at(Gsu *g, uint16_t pc);
/* One interpreted instruction. */
void gsu_step1(Gsu *g);

#endif

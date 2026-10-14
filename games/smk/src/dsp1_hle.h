/* High-level DSP-1 hot paths, exact against the chip's program.
 *
 * The translated firmware (tools/smkx/upd2m68k.py) calls these at the head
 * of its hottest loops; they leave the chip in exactly the state the
 * firmware would reach at the loop's exit point: registers, the twelve
 * flags, the return stack and the data RAM. tools/smkx/dsp1_hle.py is the
 * reference, proved against the interpreter on SMK's own traffic;
 * tools/dsp1_hle_test.c checks this code against vectors from it.
 * Freestanding. */
#ifndef DSP1_HLE_H
#define DSP1_HLE_H

#include <stdint.h>

/* The translated firmware's state block (upd_equ.i): big-endian on the
 * Falcon, native on the host test. Flags are 0 or 0xFF. */
typedef struct {
    uint16_t tr, trb, dr, sr, pc;
    uint16_t stack[4];
    uint8_t fa[6], fb[6];              /* ov0 ov1 z c s0 s1 */
    uint16_t a, b, k, l, dp, rp;       /* upd_*_saved: live in registers */
} UpdState;

/* Raster ($0A) line, uPD $364 to $384. Returns the program counter the
 * firmware continues at: $384 (outputs) or $392 (the other output path,
 * taken when RAM[$46] says so). */
unsigned dsp1_hle_raster(UpdState *s, uint16_t *ram, const uint16_t *rom);

#endif

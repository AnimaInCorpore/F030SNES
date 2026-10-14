/* Native DSP-1 commands, exact against the chip's program.
 *
 * SMK's race sends a few commands many times a frame (project, parameter,
 * triangle, distance, multiply). Instead of running the translated
 * firmware (tools/smkx/upd2m68k.py) on every host access, the runtime
 * (falcon/smkx_upd.s) collects a native command's parameter words, calls
 * dsp1n_exec once and serves the results. The functions leave the data RAM
 * exactly as the firmware would and produce the same results; the
 * registers are dead at the command poll (every host read of a 29,212-
 * command race log is unchanged when they are scrambled there), so the
 * runtime only restores what $000-$003 set: B, SR, DR.
 * tools/smkx/dsp1_native.py checks this code against the interpreter
 * (tools/smkx/upd7725.py): subroutines and commands on random inputs and
 * the recorded race traffic. Freestanding. */
#ifndef DSP1_NATIVE_H
#define DSP1_NATIVE_H

#include <stdint.h>

/* Parameter words of a native command, or -1: the firmware runs it. */
int dsp1n_params(unsigned cmd);

/* Runs a native command on the data RAM (256 words) with the data ROM
 * (1024 words); returns the number of result words written to out. */
int dsp1n_exec(unsigned cmd, const uint16_t *in, uint16_t *out, uint16_t *ram, const uint16_t *rom);

#ifdef DSP1N_TEST
/* The firmware's subroutines, for tools/smkx/dsp1_native.py. */
typedef struct {
    uint16_t a, b, k, l, tr, trb, dp, rp, ov1, s1;
} Dsp1nRegs;
void dsp1n_test_sub(unsigned entry, Dsp1nRegs *r, uint16_t *ram, const uint16_t *rom);
#endif

#endif

/* The sound CPU: an SPC700 interpreter with its 64 KiB of RAM, the three
 * timers, the four CPU ports and the S-DSP register file (no audio yet).
 * The boot ROM is not included: its upload protocol is modelled directly
 * (spc_cpu_write), so the game's own driver is what runs.
 * Freestanding C: the same file builds for the Falcon and the host. */
#ifndef SPC700_H
#define SPC700_H

#include <stdint.h>

/* The state first and the RAM after a few bytes: the 68030 reaches the fields with a 16-bit displacement
 * and the RAM with an 8-bit one (a RAM first put every field at a 32-bit displacement, an extra
 * instruction word to fetch on each access). */
typedef struct {
    uint8_t in[4];          /* written by the 65816 */
    uint8_t out[4];         /* written by the SPC700 */
    uint8_t a, x, y, sp, psw;
    uint16_t pc;
    uint8_t dspaddr, control;
    uint8_t target[3], counter[3];
    uint32_t next_tick[3];  /* cycle of each timer's next counter step */
    uint32_t tick_due;      /* the earliest next_tick of the enabled timers (timers() does nothing before) */
    uint32_t cycles;        /* SPC700 clock, 1.024 MHz */
    uint32_t until;         /* run() stops here */
    uint8_t mode;           /* SPC_IPL_*, SPC_RUN */
    uint8_t ipl_y;          /* the IPL's transfer index */
    uint16_t ipl_addr;
    uint8_t stopped;
    uint32_t steps;         /* instructions executed (diagnostics) */
    uint16_t bad_pc;        /* an opcode the interpreter stopped at */
    uint8_t ram[0x10000];
    uint8_t dsp[128];
} Spc;

enum { SPC_IPL_WAIT, SPC_IPL_FIRST, SPC_IPL_XFER, SPC_RUN };

void spc_reset(Spc *s);
/* The 65816 side: port writes ($2140-$2143) and reads. */
void spc_cpu_write(Spc *s, int port, uint8_t v);
uint8_t spc_cpu_read(Spc *s, int port);
/* Run the SPC700 up to the given clock (cycles), skipping ahead while it
 * waits for a timer or for the 65816. */
void spc_run(Spc *s, uint32_t until);

#endif

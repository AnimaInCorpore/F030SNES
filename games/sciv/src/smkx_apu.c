/* The APU for the translated program: the SPC700 (src/spc700.c) running
 * the game's own sound driver. Its clock advances with the 65816's work,
 * not with the Falcon's clock (interpreting a busy driver runs several
 * times slower than real time, and a clock-driven catch-up then fell
 * further behind with every port poll): each port access gives it about
 * what one 65816 polling iteration takes on the SNES (4 SPC700 cycles),
 * each frame 1/60 s (17,067 cycles). No audio output yet: the S-DSP
 * registers are only stored.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "spc700.h"

extern void rt_cprint(const char *text);
extern void rt_cprint_hex(uint32_t value);

/* The last port writes: port << 24 | value << 16 | SPC700 PC. */
#define LOG 32
static uint32_t wlog[LOG];
static unsigned wlog_at;

static Spc spc;
static volatile uint8_t busy;

#define PER_ACCESS 4u
/* SMKX_APU_FRAME=0 (Makefile SCIVX_APU_FRAME): no SPC700 time per frame; the
 * driver then only runs when the 65816 polls its ports (sound deferred,
 * handshakes and the intro's beat wait still work). */
#ifndef SMKX_APU_FRAME
#define SMKX_APU_FRAME 1
#endif
#define PER_FRAME (SMKX_APU_FRAME ? 1024000u / 60u : 0u)

void smkx_apu_init(void)
{
    spc_reset(&spc);
}

/* SMKX_APU_ECHO=1 (Makefile SCIVX_APU_ECHO): the SPC700 is left out once the
 * game's driver would start: the game's sound upload (the IPL protocol) is
 * still modelled, but afterwards every port write is echoed on the same
 * port and nothing runs. FF6 loads a song through its driver's own transfer
 * protocol (a handshake per byte, ~20 KB for the title theme) and its
 * SPC700 costs the 68030 several times what the SNES's takes: the title
 * screen took minutes to appear. A game that waits for the port to equal
 * what it wrote goes on; one that waits for a driver status (a song's end)
 * does not. `$FF` on port 0 with `$F0` on port 1 resets the SPC700 to its
 * boot state (InitSound waits for $BBAA after it). */
#ifndef SMKX_APU_ECHO
#define SMKX_APU_ECHO 0
#endif
#define ECHOING (SMKX_APU_ECHO && spc.mode == SPC_RUN)

static void advance(uint32_t cycles)
{
    if (busy || ECHOING)
        return;             /* a VBL inside a run: the outer run continues */
    busy = 1;
    spc_run(&spc, spc.cycles + cycles);
    busy = 0;
}

/* A 65816 poll loop reads one port until it changes (FF6's sound commands:
 * `CMP $2140 : BNE`). Each read gives the SPC700 what one pass takes, and
 * the driver's own waits (WaitEchoBuf: 16 ms per step of the echo delay,
 * counted by timer 2) then take real time: 20 passes per timer tick, the
 * game seemed stuck for minutes. A read that returns what the previous read
 * of the same port did, with no write between, is a wait, and the SPC700
 * is given twice the time of the one before, up to POLL_MAX cycles: the
 * 65816 still sees the port change only when the driver has changed it,
 * only sooner in its own count of passes. */
#define POLL_MAX (4u << 10)
static unsigned poll_cycles = PER_ACCESS;
static int poll_port = -1;
static uint8_t poll_val;

unsigned smkx_apu_read(unsigned port)
{
    advance(poll_cycles);
    uint8_t v = spc_cpu_read(&spc, (int)port);
    if ((int)port == poll_port && v == poll_val) {
        if (poll_cycles < POLL_MAX)
            poll_cycles <<= 1;
    } else {
        poll_cycles = PER_ACCESS;
        poll_port = (int)port;
        poll_val = v;
    }
    return v;
}

void smkx_apu_write(unsigned port, unsigned v)
{
    wlog[wlog_at++ % LOG] = (uint32_t)port << 24 | (uint32_t)(v & 0xFF) << 16 | spc.pc;
    poll_cycles = PER_ACCESS;
    poll_port = -1;
    advance(PER_ACCESS);
    spc_cpu_write(&spc, (int)port, (uint8_t)v);
    if (ECHOING) {
        spc.out[port & 3] = (uint8_t)v;
        if ((port & 3) == 1 && spc.in[0] == 0xFF && spc.in[1] == 0xF0)
            spc_reset(&spc);
    }
}

/* The ports 1-3 as bytes (spc_cpu_write/read only store and load them):
 * the 65816's WRAM port trick at $217F (falcon/smkx_rt.s) passes port 3
 * inline, a side effect and not a poll, so without SPC700 time. */
uint8_t *const smkx_apu_in = spc.in;
uint8_t *const smkx_apu_out = spc.out;

/* Once per frame, so the music advances even while the 65816 leaves the
 * ports alone. */
void smkx_apu_frame(void)
{
    if (PER_FRAME)
        advance(PER_FRAME);
}

uint32_t smkx_apu_steps(void) { return spc.steps | (spc.stopped ? 0x80000000u : 0); }
uint32_t smkx_apu_pc(void) { return (uint32_t)spc.pc << 16 | spc.bad_pc; }

/* At the stop: the last writes, the ports both ways and the SPC700 state. */
void smkx_apu_dump(void)
{
    rt_cprint("SMKX apu writes (port<<24|value<<16|spc pc), oldest first:");
    for (unsigned i = 0; i < LOG; i++) {
        rt_cprint(" ");
        rt_cprint_hex(wlog[(wlog_at + i) % LOG]);
    }
    rt_cprint("\nSMKX apu in/out ");
    rt_cprint_hex((uint32_t)spc.in[0] << 24 | spc.in[1] << 16 | spc.in[2] << 8 | spc.in[3]);
    rt_cprint(" ");
    rt_cprint_hex((uint32_t)spc.out[0] << 24 | spc.out[1] << 16 | spc.out[2] << 8 | spc.out[3]);
    rt_cprint(" pc/a/x/y/sp/psw ");
    rt_cprint_hex((uint32_t)spc.pc << 16 | spc.a << 8 | spc.x);
    rt_cprint_hex((uint32_t)spc.y << 24 | spc.sp << 16 | spc.psw << 8 | spc.mode);
    rt_cprint(" control/targets ");
    rt_cprint_hex((uint32_t)spc.control << 24 | spc.target[0] << 16 | spc.target[1] << 8 | spc.target[2]);
    rt_cprint("\n");
}

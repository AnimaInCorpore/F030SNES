/* Differential test of the SPC700 interpreter: build/spc_ref.c (a copy of an earlier src/spc700.c with its
 * entry points renamed ref_*) and src/spc700.c run the same random programs (random RAM, registers, ports,
 * timers) in slices of random length; the whole state must be equal after every slice.
 *   build/spc_diff [SEED [PROGRAMS [SLICES]]] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spc700.h"

void ref_reset(Spc *s);
void ref_run(Spc *s, uint32_t until);
void ref_cpu_write(Spc *s, int port, uint8_t v);
uint8_t ref_cpu_read(Spc *s, int port);

static uint64_t rs;
static uint32_t rnd(void)
{
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rs >> 33);
}

int main(int argc, char **argv)
{
    rs = argc > 1 ? strtoull(argv[1], 0, 0) : 1;
    int progs = argc > 2 ? atoi(argv[2]) : 2000, slices = argc > 3 ? atoi(argv[3]) : 40;
    static Spc a, b;
    long steps = 0;
    for (int p = 0; p < progs; p++) {
        ref_reset(&a);
        spc_reset(&b);
        /* random memory: mostly random bytes, some runs of a single opcode's likely operands */
        for (int i = 0; i < 0x10000; i++) a.ram[i] = (uint8_t)rnd();
        for (int k = 0; k < 64; k++) {           /* low, common operand values */
            uint32_t at = rnd() & 0xFFFF, n = rnd() & 63;
            for (uint32_t i = 0; i < n; i++) a.ram[(at + i) & 0xFFFF] = (uint8_t)(rnd() & 3 ? rnd() & 0xF0 : 0xF0 | (rnd() & 15));
        }
        for (int i = 0; i < 128; i++) a.dsp[i] = (uint8_t)rnd();
        a.a = (uint8_t)rnd(); a.x = (uint8_t)rnd(); a.y = (uint8_t)rnd(); a.sp = (uint8_t)rnd();
        a.psw = (uint8_t)rnd();
        a.pc = (uint16_t)(rnd() & 0xFFFF);
        a.mode = SPC_RUN;
        a.control = (uint8_t)(rnd() & 7);
        for (int t = 0; t < 3; t++) { a.target[t] = (uint8_t)rnd(); a.next_tick[t] = 100 + (rnd() & 1023); }
        for (int i = 0; i < 4; i++) a.in[i] = (uint8_t)rnd();
        memcpy(b.ram, a.ram, sizeof a.ram);
        memcpy(b.dsp, a.dsp, sizeof a.dsp);
        b.a = a.a; b.x = a.x; b.y = a.y; b.sp = a.sp; b.psw = a.psw; b.pc = a.pc; b.mode = a.mode;
        b.control = a.control;
        for (int t = 0; t < 3; t++) { b.target[t] = a.target[t]; b.next_tick[t] = a.next_tick[t]; }
        memcpy(b.in, a.in, 4);
        for (int sl = 0; sl < slices; sl++) {
            uint32_t n = 4 + (rnd() % (rnd() & 1 ? 40 : 2000));
            if (rnd() % 5 == 0) {                /* the 65816 writes a port */
                int port = (int)(rnd() & 3);
                uint8_t v = (uint8_t)rnd();
                ref_cpu_write(&a, port, v);
                spc_cpu_write(&b, port, v);
            }
            ref_run(&a, a.cycles + n);
            spc_run(&b, b.cycles + n);
            steps += (long)(a.steps);
            a.tick_due = b.tick_due = 0;               /* (a cache of the new interpreter) */
            if (memcmp(&a, &b, sizeof a)) {
                printf("MISMATCH program %d slice %d: pc %04X/%04X cycles %u/%u steps %u/%u a %02X/%02X psw %02X/%02X\n", p, sl,
                       a.pc, b.pc, a.cycles, b.cycles, a.steps, b.steps, a.a, b.a, a.psw, b.psw);
                for (int i = 0; i < 0x10000; i++)
                    if (a.ram[i] != b.ram[i]) { printf("  ram[%04X] %02X/%02X\n", i, a.ram[i], b.ram[i]); break; }
                return 1;
            }
            if (a.stopped) break;
            for (int i = 0; i < 4; i++)
                if (ref_cpu_read(&a, i) != spc_cpu_read(&b, i)) { printf("port %d differs\n", i); return 1; }
        }
    }
    printf("spc_diff: %d programs, %ld instructions, equal\n", progs, steps);
    return 0;
}

/* Host check of the SPC700 interpreter (src/spc700.c) on CT's own driver:
 * every opcode but SLEEP/STOP decodes; the six driver blocks go up through
 * the modelled boot-ROM protocol exactly as Audio_UploadDriver ($C70038)
 * sends them; then the driver runs, takes a play-song command the way
 * Audio_SendPlaySongCmd does and must echo it. (Port 3, the beat the
 * intro waits for at $7E5AAA, only moves once the game has uploaded the
 * song and its instruments; the test just reports it.)
 *     build/spc_test build/ctx/ct.sfc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spc700.h"

static uint8_t *rom;
static uint8_t rb(uint32_t a) { return rom[a & 0x3FFFFF]; }

static Spc s;

/* One 65816 port write, with the SPC700 running a little around it, and a
 * wait for the echo the 65816 code waits for. */
static void put(int port, uint8_t v) { spc_cpu_write(&s, port, v); spc_run(&s, s.cycles + 40); }
static int wait0(uint8_t v)
{
    for (int i = 0; i < 20000; i++) {
        if (spc_cpu_read(&s, 0) == v) return 1;
        spc_run(&s, s.cycles + 20);
    }
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "build/ctx/ct.sfc", "rb");
    if (!f) { perror("rom"); return 1; }
    rom = malloc(0x400000);
    if (fread(rom, 1, 0x400000, f) != 0x400000) { fprintf(stderr, "short ROM\n"); return 1; }
    fclose(f);

    /* every opcode decodes (SLEEP $EF and STOP $FF stop on purpose) */
    int bad = 0;
    for (int op = 0; op < 256; op++) {
        spc_reset(&s);
        s.mode = SPC_RUN;
        s.pc = 0x1000;
        s.ram[0x1000] = (uint8_t)op;
        spc_run(&s, 30);
        if (s.stopped && op != 0xEF && op != 0xFF) { printf("opcode $%02X unhandled\n", op); bad++; }
    }

    /* upload: blocks at $C70010 (pointers), $C7001C (SPC destinations) */
    spc_reset(&s);
    if (spc_cpu_read(&s, 0) != 0xAA || spc_cpu_read(&s, 1) != 0xBB) { puts("no IPL ready"); return 1; }
    uint8_t cnt = 0xCC;
    for (int blk = 0; blk < 6; blk++) {
        uint32_t ptr = 0xC70000 | rb(0xC70010 + 2 * blk) | rb(0xC70011 + 2 * blk) << 8;
        uint16_t dst = (uint16_t)(rb(0xC7001C + 2 * blk) | rb(0xC7001D + 2 * blk) << 8);
        uint16_t len = (uint16_t)(rb(ptr) | rb(ptr + 1) << 8);
        put(2, (uint8_t)dst); put(3, (uint8_t)(dst >> 8)); put(1, 1); put(0, cnt);
        if (!wait0(cnt)) { printf("block %d: no echo of $%02X\n", blk, cnt); return 1; }
        for (unsigned i = 0; i < len; i++) {
            put(1, rb(ptr + 2 + i));
            put(0, (uint8_t)i);
            if (!wait0((uint8_t)i)) { printf("block %d byte %u: no echo\n", blk, i); return 1; }
        }
        cnt = (uint8_t)(len + 3);
        if (!cnt) cnt++;
    }
    put(2, 0x00); put(3, 0x02); put(1, 0); put(0, cnt);
    if (s.mode != SPC_RUN) { puts("driver not started"); return 1; }
    if (!wait0(cnt)) { puts("no echo of the execute command"); return 1; }
    spc_run(&s, s.cycles + 1024000 / 10);
    printf("driver: pc $%04X, %u steps, ports %02X %02X %02X %02X%s\n", s.pc, (unsigned)s.steps,
           s.out[0], s.out[1], s.out[2], s.out[3], s.stopped ? " STOPPED" : "");
    if (s.stopped) { printf("stopped at $%04X op $%02X\n", s.bad_pc, s.ram[s.bad_pc]); return 1; }

    /* play song $30 the way Audio_SendPlaySongCmd sends it: ports 3,2,1,0 */
    put(3, 0x00); put(2, 0xFF); put(1, 0x30); put(0, 0x10);
    int ok = wait0(0x10);
    printf("play command echoed: %s\n", ok ? "yes" : "no");
    uint8_t p3 = spc_cpu_read(&s, 3);
    int changes = 0;
    for (int ms = 0; ms < 3000; ms += 10) {
        spc_run(&s, s.cycles + 10240);
        if (spc_cpu_read(&s, 3) != p3) { changes++; p3 = spc_cpu_read(&s, 3); if (changes <= 8) printf("  t=%dms port3=$%02X\n", ms, p3); }
    }
    printf("port 3 changed %d times in 3 s; %u steps; dsp KON $%02X FLG $%02X%s\n", changes, (unsigned)s.steps,
           s.dsp[0x4C], s.dsp[0x6C], s.stopped ? " STOPPED" : "");
    if (getenv("SPC_OPHIST")) {              /* the driver's opcode mix (instructions in 3 s of play) */
        static unsigned oph[256];
        unsigned total = 0;
        for (int i = 0; i < 2000000; i++) { oph[s.ram[s.pc]]++; total++; spc_run(&s, s.cycles + 1); }
        for (int k = 0; k < 24; k++) {
            unsigned best = 0, bop = 0;
            for (unsigned o = 0; o < 256; o++) if (oph[o] > best) { best = oph[o]; bop = o; }
            printf("  op $%02X %5.1f%%\n", bop, 100.0 * best / total);
            oph[bop] = 0;
        }
    }
    if (getenv("SPC_HIST")) {
        static unsigned hist[65536];
        for (int i = 0; i < 200000; i++) { hist[s.pc]++; spc_run(&s, s.cycles + 1); }
        for (int k = 0; k < 12; k++) {
            unsigned best = 0, bpc = 0;
            for (unsigned pc = 0; pc < 65536; pc++) if (hist[pc] > best) { best = hist[pc]; bpc = pc; }
            printf("  pc $%04X %u  %02X %02X %02X\n", bpc, best, s.ram[bpc], s.ram[bpc + 1], s.ram[bpc + 2]);
            hist[bpc] = 0;
        }
    }
    return bad || !ok;
}

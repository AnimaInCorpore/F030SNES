/* Host check of the GSU core: run one SuperFX routine from the ROM.
 *
 *   build/gsu_run ROM PBR:PC [RAM.BIN]
 *
 * Prints the steps, the registers at STOP and a checksum of the GSU RAM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gsu.h"

int main(int argc, char **argv)
{
    static uint8_t rom[0x200000], ram[GSU_RAM_BYTES];
    Gsu g;
    FILE *f;
    unsigned pbr, pc, i;
    uint32_t sum = 0;
    if (argc < 3 || sscanf(argv[2], "%x:%x", &pbr, &pc) != 2) {
        fprintf(stderr, "usage: gsu_run ROM PBR:PC [RAM.BIN]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f || fread(rom, 1, sizeof rom, f) != sizeof rom) {
        fprintf(stderr, "%s: cannot read 2 MiB\n", argv[1]);
        return 1;
    }
    fclose(f);
    if (argc > 3) {
        f = fopen(argv[3], "rb");
        if (!f || fread(ram, 1, sizeof ram, f) != sizeof ram) {
            fprintf(stderr, "%s: cannot read RAM\n", argv[3]);
            return 1;
        }
        fclose(f);
    }
    gsu_init(&g, rom, ram);
    gsu_write_reg(&g, 0x3034, (uint8_t)pbr);
    gsu_write_reg(&g, 0x301E, (uint8_t)pc);
    if (gsu_write_reg(&g, 0x301F, (uint8_t)(pc >> 8)))
        gsu_run(&g, 50000000);
    for (i = 0; i < GSU_RAM_BYTES; i++)
        sum = sum * 31 + ram[i];
    printf("%s steps %u, stop at %02X:%04X\n", g.fault ? "LIMIT" : "stopped", (unsigned)g.last_steps,
           g.pbr, g.r[15]);
    for (i = 0; i < 16; i++)
        printf("R%-2u %04X%s", i, g.r[i], (i & 7) == 7 ? "\n" : "  ");
    printf("RAM sum %08X\n", (unsigned)sum);
    return g.fault;
}

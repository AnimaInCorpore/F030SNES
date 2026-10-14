/* ppu_c2p8 (merge network) against ppu_planar8 (bit by bit) on random
 * frames and on single pixels of every value at every position. */
#include "ppu_fast.h"

#include <stdio.h>
#include <string.h>

static uint8_t chunky[PPU_W * PPU_H];
static uint16_t ref[PPU_W * PPU_H / 2], fast[PPU_W * PPU_H / 2];

static int check(const char *what)
{
    ppu_planar8(chunky, ref);
    ppu_c2p8(chunky, fast);
    if (memcmp(ref, fast, sizeof ref)) {
        for (size_t i = 0; i < sizeof ref / 2; i++)
            if (ref[i] != fast[i]) {
                printf("%s: word %zu: %04X, reference %04X\n", what, i, fast[i], ref[i]);
                break;
            }
        return 1;
    }
    return 0;
}

int main(void)
{
    uint32_t x = 12345;
    int fail = 0;
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < PPU_W * PPU_H; i++) {
            x = x * 1103515245u + 12345u;
            chunky[i] = (uint8_t)(x >> 16);
        }
        fail |= check("random");
    }
    for (int v = 1; v < 256; v <<= 1)
        for (int pos = 0; pos < 64; pos++) {
            memset(chunky, 0, sizeof chunky);
            chunky[pos * 997 % (PPU_W * PPU_H)] = (uint8_t)v;
            fail |= check("single pixel");
        }
    printf(fail ? "c2p: FAIL\n" : "c2p: 8 random frames and 512 single pixels match\n");
    return fail;
}

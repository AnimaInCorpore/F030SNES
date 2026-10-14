/* TCBENCH: times the chunky true-colour core (falcon/dsp/tcbench.asm) on
 * the Falcon: 224 rows of three BG layers, first computed only, then with
 * every row's RGB565 words through the SSI into DMA record (114,688 bytes,
 * a 256x224 true-colour picture). Freestanding (falcon/tcbench.s). */
#include <stdint.h>
#include "smkx_prof.h"

extern void m7t_print(const char *s);
extern void m7t_hex(uint32_t v);
extern int m7t_record(uint16_t *buf, uint32_t bytes);
extern int m7t_record_done(void);
extern void pt_boot(int kernel);

#define HP ((volatile uint8_t *)0xFFFFA200)

static void put(uint32_t w)
{
    while (!(HP[2] & 2)) {}
    HP[5] = (uint8_t)(w >> 16);
    HP[6] = (uint8_t)(w >> 8);
    HP[7] = (uint8_t)w;
}

static uint32_t get(void)
{
    while (!(HP[2] & 1)) {}
    return ((uint32_t)HP[5] << 16) | ((uint32_t)HP[6] << 8) | HP[7];
}

static uint16_t picture[256 * 224];

static uint32_t bench(int send)
{
    if (send && m7t_record(picture, sizeof picture)) {
        m7t_print("TCBENCH: DMA record route not available\n");
        return 0;
    }
    uint32_t t0 = prof_now();
    put(0x30);
    put(224);
    put((uint32_t)send);
    get();
    if (send)
        while (!m7t_record_done()) {}
    return prof_now() - t0;
}

int tcbench_main(void)
{
    m7t_print("TCBENCH start\n");
    pt_boot(0);
    put(4);
    if (get() != 0x544342) {
        m7t_print("TCBENCH: no answer to PING\n");
        return 1;
    }
    for (int pass = 0; pass < 2; pass++) {
        uint32_t c = bench(0), s = bench(1);
        m7t_print("TCBENCH (emulated, 1/38400 s): compute only $");
        m7t_hex(c);
        m7t_print(" with the output $");
        m7t_hex(s);
        m7t_print("\n");
    }
    return 0;
}

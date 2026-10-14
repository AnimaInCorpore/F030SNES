/* Render profiling on the Falcon: a clock in 1/38400 s (26 us) from
 * _hz_200 and MFP Timer C, which TOS runs at 200 Hz (2.4576 MHz / 64,
 * reload 192). Timer C interrupts above the VBL, so the clock runs while
 * the VBL handler renders. Freestanding. */
#ifndef SMKX_PROF_H
#define SMKX_PROF_H

#include <stdint.h>

#define PROF_HZ 38400u

static inline uint32_t prof_now(void)
{
    volatile uint32_t *hz200 = (volatile uint32_t *)0x4BA;
    volatile uint8_t *tcdr = (volatile uint8_t *)0xFFFFFA23;
    static uint32_t last;
    uint32_t a, b, t;
    uint8_t c;
    do {
        a = *hz200;
        c = *tcdr;
        b = *hz200;
    } while (a != b);
    t = a * 192u + (uint32_t)(192u - c);
    /* The counter reloads before the interrupt counts _hz_200 up: never
     * let the clock step back (differences are unsigned). */
    if (t < last) t = last;
    last = t;
    return t;
}

#endif

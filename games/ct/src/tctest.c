/* TCTEST: the true-colour renderer (falcon/dsp/tc.asm; Mode 7 frames its
 * TC7 build) against the reference (ppu_render_565), on the frames build/tc_snap -pack and
 * build/tc_rand write (tc_tests.bin). Per frame the VRAM pages go up over
 * the host port with the conversion each needs (TPAGE: the DSP converts),
 * the stream follows, and the DSP's rows come back through SSI -> DMA
 * record as RGB565 words; the words that differ are counted.
 * Freestanding: runs on the Falcon (falcon/tctest.s provides the glue). */
#include "ppu_dsp.h"
#include "ppu_tc.h"
#include "smkx_prof.h"

extern void m7t_print(const char *s);
extern void m7t_hex(uint32_t v);
extern int m7t_record(uint16_t *buf, uint32_t bytes);
extern int m7t_record_done(void);
extern int m7t_save(const char *name, const void *buf, long bytes);
extern const uint8_t tc_tests[];
extern void pt_boot(int tc7);

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

static uint32_t rd32(const uint8_t **p)
{
    const uint8_t *q = *p;
    *p += 4;
    return (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
}

static uint16_t rd16(const uint8_t **p)
{
    const uint8_t *q = *p;
    *p += 2;
    return (uint16_t)(q[0] << 8 | q[1]);
}

static void say(const char *s, uint32_t v)
{
    m7t_print(s);
    m7t_hex(v);
}

static uint16_t captured[PPU_W * PPU_H];

int tctest_main(void)
{
    m7t_print("TCTEST start\n");
    int booted = -1;
    const uint8_t *p = tc_tests + 4;
    uint32_t frames = rd32(&p);
    uint32_t bad_total = 0, tested = 0, banded = 0;
    for (uint32_t f = 0; f < frames; f++) {
        int kernel = (int)rd32(&p);
        uint32_t n = rd32(&p);
        const uint8_t *words = p;
        p += n * 4;
        uint32_t nslots = rd32(&p);
        const uint8_t *pages = p;
        p += nslots * (2 + 512);
        const uint8_t *expect = p;
        p += PPU_W * PPU_H * 2;
        if ((kernel != DSP_KERNEL_TC && kernel != DSP_KERNEL_TC7) || nslots > (uint32_t)(kernel == DSP_KERNEL_TC7 ? TC_SLOTS : TC_SLOTS_TILE) || n - 1 > TC_STREAM_MAX) {
            say("TCTEST frame $", f);
            m7t_print(" left out (too big)\n");
            continue;
        }
        if (booted != (kernel == DSP_KERNEL_TC7)) {    /* the frame's program */
            booted = kernel == DSP_KERNEL_TC7;
            pt_boot(booted);
            put(4);
            if (get() != 0x544353) {
                m7t_print("TCTEST: the program did not answer PING\n");
                return 1;
            }
        }
        uint32_t t0 = prof_now();
        const uint8_t *q = pages;
        for (uint32_t k = 0; k < nslots; k++) {
            uint16_t pf = rd16(&q);
            put(0x22);
            put(pf & 0xFF);                 /* page */
            put(pf >> 8);                   /* format */
            put(tc_slot_p((int)k));
            for (int i = 0; i < 256; i++) put(rd16(&q));
        }
        uint32_t t1 = prof_now();
        for (int i = 0; i < PPU_W * PPU_H; i++) captured[i] = 0x5A5A;
        if (m7t_record(captured, PPU_W * PPU_H * 2)) {
            m7t_print("TCTEST: DMA record route not available\n");
            return 1;
        }
        const uint8_t *w = words + 4;       /* after DSP_CMD_FRAME */
        put(0x10);
        put(n - 1);
        for (uint32_t i = 1; i < n; i++) put(rd32(&w));
        uint32_t t2 = prof_now();
        uint32_t spin;
        for (spin = 0; spin < 50000000 && !m7t_record_done(); spin++) {}
        uint32_t t3 = prof_now();
        uint32_t bad = 0, first = 0xFFFFFFFF, rows = 0;
        int lastrow = -1;
        const uint8_t *e = expect;
        for (int i = 0; i < PPU_W * PPU_H; i++) {
            uint16_t x = rd16(&e);
            if (x != captured[i]) {
                bad++;
                if (first == 0xFFFFFFFF) first = (uint32_t)i;
                if (i / PPU_W != lastrow) { rows++; lastrow = i / PPU_W; }
            }
        }
        bad_total += bad;
        tested++;
        say("TCTEST frame $", f);
        say(" kernel $", (uint32_t)kernel);
        say(" slots $", nslots);
        say(" stream $", n);
        say(" differing words $", bad);
        say(" in rows $", rows);
        if (bad) {
            static char name[] = "TC00.BIN";
            name[2] = (char)('0' + (f / 10) % 10);
            name[3] = (char)('0' + f % 10);
            m7t_save(name, captured, PPU_W * PPU_H * 2);
            say(" first at $", first);
            say(" got $", captured[first]);
            e = expect + first * 2;
            say(" want $", rd16(&e));
        }
        if (spin == 50000000) m7t_print(" TIMEOUT");
        m7t_print("\n");
        {
            /* The same frame again, whole, straight after: what a picture drawn twice looks like (the
             * program keeps state between frames) */
            for (int i = 0; i < PPU_W * PPU_H; i++) captured[i] = 0x5A5A;
            m7t_record(captured, PPU_W * PPU_H * 2);
            put(0x10);
            put(n - 1);
            w = words + 4;
            for (uint32_t i = 1; i < n; i++) put(rd32(&w));
            for (spin = 0; spin < 50000000 && !m7t_record_done(); spin++) {}
            uint32_t bb = 0;
            e = expect;
            for (int i = 0; i < PPU_W * PPU_H; i++) bb += rd16(&e) != captured[i];
            say("TCTEST again frame $", f);
            say(" differing words $", bb);
            if (spin == 50000000) m7t_print(" TIMEOUT");
            m7t_print("\n");
            bad_total += bb;
        }
        if (f % 3 == 0) {
            /* Again in two BAND commands (the pages are the whole frame's):
             * rows 0-111, then 112-223, into the same record window. */
            for (int i = 0; i < PPU_W * PPU_H; i++) captured[i] = 0x5A5A;
            m7t_record(captured, PPU_W * PPU_H * 2);
            for (int b = 0; b < 2; b++) {
                put(0x11);
                put((uint32_t)b * 112);
                put((uint32_t)b * 112 + 112);
                w = words + 4;
                put(0x10);
                put(n - 1);
                for (uint32_t i = 1; i < n; i++) put(rd32(&w));
            }
            for (spin = 0; spin < 50000000 && !m7t_record_done(); spin++) {}
            uint32_t bb = 0;
            e = expect;
            for (int i = 0; i < PPU_W * PPU_H; i++) bb += rd16(&e) != captured[i];
            say("TCTEST banded frame $", f);
            say(" differing words $", bb);
            if (spin == 50000000) m7t_print(" TIMEOUT");
            m7t_print("\n");
            bad_total += bb;
            banded++;
        }
        say("TCTEST timing (emulated, 1/38400 s): upload $", t1 - t0);
        say(" frame send $", t2 - t1);
        say(" render+record $", t3 - t2);
        say(" ms $", (t3 - t2) * 1000u / PROF_HZ);
        m7t_print("\n");
    }
    say("TCTEST frames $", tested);
    say(" (banded $", banded);
    m7t_print(")");
    say(" differing words $", bad_total);
    m7t_print("\n");
    return bad_total != 0;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memset(void *d, int v, unsigned long n)
{
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)v;
    return d;
}

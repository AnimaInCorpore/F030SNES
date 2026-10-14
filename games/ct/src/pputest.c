/* PPUTEST: the DSP renderer (falcon/dsp/ppu.asm) on real frames.
 *
 * build/dsp_snap -pack writes each frame's stream, the VRAM pages it reads
 * and the planes the host model (= ppu_render + ppu_planar8) composes. The
 * pages go up over the host port, the stream follows, and the DSP's rows
 * come back through SSI -> DMA record; the words that differ are counted.
 * Freestanding: runs on the Falcon (falcon/pputest.s provides the glue). */
#include "ppu_dsp.h"
#include "smkx_prof.h"

extern void m7t_print(const char *s);
extern void m7t_hex(uint32_t v);
extern int m7t_record(uint16_t *buf, uint32_t bytes);
extern int m7t_record_done(void);
extern const uint8_t ppu_tests[];
extern void pt_boot(int kernel);        /* ppu.bin, ppu7.bin or ppuw.bin (DSP_KERNEL_*) */

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

/* Cache slot k of the tile program (ppu_dsp.h). */
static uint32_t slot_addr(int k)
{
    return dsp_tile_slot_p(k);
}

static uint16_t captured[PPU_H * 128];

/* Boots the program and checks that it answers PING. */
static int boot(int kernel)
{
    pt_boot(kernel);
    put(4);
    return get() == 0x505055;
}

static void say(const char *s, uint32_t v)
{
    m7t_print(s);
    m7t_hex(v);
}

int pputest_main(void)
{
    m7t_print("PPUTEST start\n");
    int prog = -1;
    const uint8_t *p = ppu_tests + 4;
    uint32_t frames = rd32(&p);
    int bad_total = 0;
    for (uint32_t f = 0; f < frames; f++) {
        int kernel = (int)rd32(&p);
        if (kernel != prog) {
            if (!boot(kernel)) {
                say("PPUTEST: renderer did not answer PING, program $", (uint32_t)kernel);
                m7t_print("\n");
                return 1;
            }
            prog = kernel;
        }
        uint32_t n = rd32(&p);
        const uint8_t *words = p;
        p += n * 4;
        uint32_t npages = rd32(&p);
        uint32_t t0 = prof_now();
        int spr = 0;
        for (uint32_t k = 0; k < npages; k++) {
            uint16_t pg = rd16(&p), raw = pg & 0x8000;
            pg &= 0x7FFF;
            put(0x20);
            put(pg);
            if (kernel == DSP_KERNEL_M7 && pg < 64 && !raw) {
                /* the Mode 7 plane, packed: (tile * 64) << 8 | texel */
                put(DSP_M7_PLANE_P + (uint32_t)pg * 256);
                for (int i = 0; i < 256; i++) {
                    uint16_t w = rd16(&p);
                    put((uint32_t)(w & 0xFF) << 14 | (uint32_t)(w >> 8));
                }
                continue;
            }
            put(kernel == DSP_KERNEL_M7 ? DSP_M7_SPRITE_P + (uint32_t)spr++ * 256 : slot_addr((int)k));
            for (int i = 0; i < 256; i++) put(rd16(&p));
        }
        uint32_t t1 = prof_now();
        const uint8_t *expect = p;
        p += PPU_H * 128 * 2;
        for (int i = 0; i < PPU_H * 128; i++) captured[i] = 0x5A5A;
        if (m7t_record(captured, PPU_H * 256)) {
            m7t_print("PPUTEST: DMA record route not available\n");
            return 1;
        }
        const uint8_t *w = words + 4;           /* after DSP_CMD_FRAME */
        put(0x10);
        put(n - 1);
        for (uint32_t i = 1; i < n; i++) put(rd32(&w));
        uint32_t t2 = prof_now();
        uint32_t spin;
        for (spin = 0; spin < 50000000 && !m7t_record_done(); spin++) {}
        uint32_t t3 = prof_now();
        int bad = 0, first = -1, rows = 0, lastrow = -1;
        const uint8_t *e = expect;
        for (int i = 0; i < PPU_H * 128; i++) {
            uint16_t x = rd16(&e);
            if (x != captured[i]) {
                bad++;
                if (first < 0) first = i;
                if (i / 128 != lastrow) { rows++; lastrow = i / 128; }
            }
        }
        bad_total += bad;
        /* the first bad rows: which of their 128 words differ */
        int shown = 0;
        for (int row = 0; row < PPU_H && shown < 8; row++) {
            const uint8_t *er = expect + row * 256;
            uint32_t m[4] = {0, 0, 0, 0};
            int any = 0;
            for (int i = 0; i < 128; i++) {
                const uint8_t *q = er + i * 2;
                if ((uint16_t)(q[0] << 8 | q[1]) != captured[row * 128 + i]) {
                    m[i >> 5] |= 1u << (31 - (i & 31));
                    any = 1;
                }
            }
            if (!any) continue;
            say("  row $", (uint32_t)row);
            for (int k = 0; k < 4; k++) say(" ", m[k]);
            m7t_print("\n");
            shown++;
        }
        say("PPUTEST frame $", f);
        say(" pages $", npages);
        say(" stream $", n);
        say(" differing words $", (uint32_t)bad);
        say(" in rows $", (uint32_t)rows);
        if (bad) {
            say(" first at word $", (uint32_t)first);
            say(" got $", captured[first]);
            e = expect + first * 2;
            say(" want $", rd16(&e));
        }
        if (spin == 50000000) {
            volatile uint8_t *snd = (volatile uint8_t *)0xFFFF8900;
            snd[1] |= 0x80;                 /* the record registers */
            uint32_t at = (uint32_t)snd[9] << 16 | (uint32_t)snd[11] << 8 | snd[13];
            say(" TIMEOUT, record at byte $", at - (uint32_t)(uintptr_t)captured);
            say(" ctrl $", snd[1]);
        }
        m7t_print("\n");
        say("PPUTEST timing (emulated, 1/38400 s): upload $", t1 - t0);
        say(" frame send $", t2 - t1);
        say(" render+record $", t3 - t2);
        say(" ms $", (t3 - t2) * 1000u / PROF_HZ);
        m7t_print("\n");
    }
    /* Transport alone: a frame in forced blank, all rows zero. */
    {
        for (int i = 0; i < PPU_H * 128; i++) captured[i] = 0x5A5A;
        m7t_record(captured, PPU_H * 256);
        uint32_t t0 = prof_now();
        put(0x10);
        put(DSP_NSLOT + 2);
        put(0x80);                              /* INIDISP: forced blank */
        for (int i = 1; i < DSP_NSLOT; i++) put(0);
        put(0);                                 /* no changes */
        put(0);                                 /* no OAM segments */
        uint32_t spin;
        for (spin = 0; spin < 50000000 && !m7t_record_done(); spin++) {}
        uint32_t t1 = prof_now();
        int bad = 0;
        for (int i = 0; i < PPU_H * 128; i++) bad += captured[i] != 0;
        say("PPUTEST blank frame: differing words $", (uint32_t)bad);
        say(" ms $", (t1 - t0) * 1000u / PROF_HZ);
        m7t_print("\n");
        bad_total += bad;
    }
    say("PPUTEST frames $", frames);
    say(" differing words $", (uint32_t)bad_total);
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

/* LCDTEST: the list compositor's DSP build (falcon/dsp/ppu.asm, LCB=1) on real frames.
 *
 * build/dsp_snap -packlc writes each frame's stream, the VRAM pages it reads, 224 row masks and the list
 * the DSP must send (the eight plane words of each group, in row order). The pages go up over the host port, LCMASK and the stream follow, the list comes back
 * through SSI -> DMA record; the words that differ are counted. Freestanding (falcon/lcdtest.s). */
#include "ppu_dsp.h"
#include "smkx_prof.h"

extern void m7t_print(const char *s);
extern void m7t_hex(uint32_t v);
extern int m7t_record(uint16_t *buf, uint32_t bytes);
extern int m7t_record_done(void);
extern const uint8_t lcd_tests[];
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

#define LIST_MAX (PPU_H * 16 * 9 + 16)
static uint16_t captured[LIST_MAX];

int lcdtest_main(void)
{
    m7t_print("LCDTEST start\n");
    const uint8_t *p = lcd_tests + 4;
    uint32_t frames = rd32(&p);
    int bad_total = 0, booted = -1;
    for (uint32_t f = 0; f < frames; f++) {
        int kernel = (int)rd32(&p);                 /* DSP_KERNEL_LC or DSP_KERNEL_LCW */
        if (booted != kernel) {
            pt_boot(kernel);
            put(4);
            if (get() != 0x505055) {
                m7t_print("LCDTEST: renderer did not answer PING\n");
                return 1;
            }
            booted = kernel;
        }
        uint32_t n = rd32(&p);
        const uint8_t *words = p;
        p += n * 4;
        uint32_t npages = rd32(&p);
        uint32_t t0 = prof_now();
        int sprslot = 0;
        for (uint32_t k = 0; k < npages; k++) {
            uint16_t pg = rd16(&p);
            int raw = pg & 0x8000;
            pg &= 0x7FFF;
            put(0x20);
            put(pg);
            put(kernel == DSP_KERNEL_LC7 ? raw ? DSP_M7_SPRITE_P + (uint32_t)sprslot++ * 256 : DSP_M7_PLANE_P + (uint32_t)pg * 256 : dsp_lc_slot_p((int)k));
            for (int i = 0; i < 256; i++) {
                uint32_t w = rd16(&p);
                put(kernel == DSP_KERNEL_LC7 && !raw ? (w & 255) << 14 | w >> 8 : w);
            }
        }
        uint32_t t1 = prof_now();
        uint16_t masks[PPU_H];
        int nrows = 0, ngroups = 0;
        for (int y = 0; y < PPU_H; y++) {
            masks[y] = rd16(&p);
            nrows += masks[y] != 0;
            for (int g = 0; g < 16; g++) ngroups += masks[y] >> g & 1;
        }
        uint32_t nl = rd32(&p);
        const uint8_t *expect = p;
        p += nl * 2;
        for (uint32_t i = 0; i < nl; i++) captured[i] = 0x5A5A;
        if (m7t_record(captured, nl * 2)) {
            m7t_print("LCDTEST: DMA record route not available\n");
            return 1;
        }
        put(DSP_CMD_LCMASK);
        for (int y = 0; y < PPU_H; y++) put(masks[y]);
        const uint8_t *w = words + 4;               /* after DSP_CMD_FRAME */
        put(0x10);
        put(n - 1);
        for (uint32_t i = 1; i < n; i++) put(rd32(&w));
        uint32_t t2 = prof_now();
        uint32_t spin;
        for (spin = 0; spin < 4000000 && !m7t_record_done(); spin++) {}
        uint32_t t3 = prof_now();
        int bad = 0;
        uint32_t first = 0;
        const uint8_t *e = expect;
        for (uint32_t i = 0; i < nl; i++) {
            uint16_t x = rd16(&e);
            if (x != captured[i]) {
                if (!bad) first = i;
                bad++;
            }
        }
        bad_total += bad;
        say("LCDTEST frame $", f);
        say(" rows $", (uint32_t)nrows);
        say(" groups $", (uint32_t)ngroups);
        say(" list words $", nl);
        say(" differing words $", (uint32_t)bad);
        if (bad) {
            say(" first at word $", first);
            say(" got $", captured[first]);
            e = expect + first * 2;
            say(" want $", rd16(&e));
        }
        if (spin == 4000000) {                      /* the DSP sent fewer words than the list should hold: it is stuck */
            m7t_print(" TIMEOUT\n");
            bad_total++;
            break;
        }
        m7t_print("\n");
        say("LCDTEST timing (emulated, 1/38400 s): upload $", t1 - t0);
        say(" send $", t2 - t1);
        say(" compose+record $", t3 - t2);
        say(" ms $", (t3 - t2) * 1000u / PROF_HZ);
        m7t_print("\n");
    }
    say("LCDTEST frames $", frames);
    say(" differing words $", (uint32_t)bad_total);
    m7t_print("\n");
    return bad_total != 0;
}

/* M7TEST: the DSP Mode 7 kernel against ppu_m7_pixels() + ppu_planar().
 *
 * Random VRAM contents, a random CGRAM -> palette slot table and random
 * lines (every M7SEL outside mode and flip, starts inside and far outside,
 * steps across the 16-bit range) go to the kernel over the host port; its
 * plane words come back through SSI -> DMA record. The 68030 computes the
 * reference from the same inputs and counts differing words.
 * Freestanding: runs on the Falcon (falcon/m7test.s provides the glue). */
#include "m7_plan.h"

#define LINES 64

extern void m7t_print(const char *s);
extern void m7t_hex(uint32_t v);
extern int m7t_record(uint16_t *buf, uint32_t bytes);      /* arm DMA record */
extern int m7t_record_done(void);

static uint8_t vram[65536];
static uint8_t lut[256];
static uint16_t captured[LINES * 64 + 64];
static uint32_t rng = 0x12345678;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

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

static void random_line(PpuM7Line *l)
{
    static const uint8_t sels[3] = {0x00, 0x80, 0xC0};
    uint32_t r = rnd();
    int32_t a = (int16_t)rnd(), c = (int16_t)rnd();
    if ((r & 3) == 0) { a >>= 6; c >>= 6; }          /* near: small steps */
    l->a = a;
    l->c = c;
    /* start inside, just outside, or far outside */
    int32_t spread = (r & 0x30) == 0 ? 0x40000 : ((r & 0x30) == 0x10 ? 0x60000 : 0x1000000);
    l->vx = (int32_t)(rnd() % (uint32_t)spread) - (spread - 0x40000) / 2;
    l->vy = (int32_t)(rnd() % (uint32_t)spread) - (spread - 0x40000) / 2;
    l->sel = sels[(r >> 8) % 3];
}

static void planar_line(const uint8_t *idx, uint16_t *out)
{
    for (int g = 0; g < 16; g++) {
        uint16_t p[4] = {0, 0, 0, 0};
        for (int k = 0; k < 16; k++) {
            uint8_t c = lut[idx[g * 16 + k]];
            for (int b = 0; b < 4; b++)
                if (c & (1 << b)) p[b] |= (uint16_t)(0x8000 >> k);
        }
        for (int b = 0; b < 4; b++) *out++ = p[b];
    }
}

int m7test_main(void)
{
    m7t_print("M7TEST start\n");
    put(4);
    uint32_t ping = get();
    if (ping != 0x4D3743) {
        m7t_print("M7TEST: kernel did not answer PING, got $");
        m7t_hex(ping);
        m7t_print("\n");
        return 1;
    }
    for (int i = 0; i < 65536; i++) vram[i] = (uint8_t)rnd();
    for (int i = 0; i < 256; i++) lut[i] = (uint8_t)(rnd() & 15);
    put(1);
    for (int i = 0; i < 16384; i++)
        put(((uint32_t)vram[2 * i] * 64 << 8) | vram[2 * i + 1]);
    put(2);
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 256; i++)
            put(((lut[i] >> p) & 1) << 8);

    static PpuM7Line lines[LINES];
    for (int i = 0; i < LINES; i++) random_line(&lines[i]);

    if (m7t_record(captured, LINES * 128)) {
        m7t_print("M7TEST: DMA record route not available\n");
        return 1;
    }
    int nsegs = 0;
    for (int i = 0; i < LINES; i++) {
        M7Seg seg[M7_MAX_SEGS];
        int n = m7_plan(&lines[i], seg);
        nsegs += n;
        put(3);
        for (int k = 0; k < n; k++) {
            put(seg[k].kind);
            put(seg[k].count);
            if (seg[k].kind == M7_FILL) {
                put(0);                          /* transparent: CGRAM index 0 */
                continue;
            }
            int32_t du = seg[k].du, dv = seg[k].dv;
            /* 48-bit step (dv << 24) + du: x1 absorbs du's borrow */
            put((uint32_t)seg[k].v & 0xFFFFFF);
            put(((uint32_t)seg[k].u + 0x800000) & 0xFFFFFF);
            put((uint32_t)(dv + (du < 0 ? -1 : 0)) & 0xFFFFFF);
            put((uint32_t)du & 0xFFFFFF);
        }
    }
    while (!m7t_record_done()) {}

    int bad = 0, firstbad = -1, shown = 0;
    for (int i = 0; i < LINES; i++) {
        uint8_t idx[256];
        uint16_t ref[64];
        ppu_m7_pixels(vram, &lines[i], idx);
        planar_line(idx, ref);
        int lb = 0, lfirst = -1;
        for (int w = 0; w < 64; w++)
            if (ref[w] != captured[i * 64 + w]) {
                bad++;
                lb++;
                if (lfirst < 0) lfirst = w;
                if (firstbad < 0) firstbad = i * 64 + w;
            }
        if (lb && shown < 12) {
            M7Seg seg[M7_MAX_SEGS];
            int n = m7_plan(&lines[i], seg), kinds = 0, x = 0, segat = -1;
            for (int k = 0; k < n; k++) {
                kinds |= 1 << seg[k].kind;
                if (segat < 0 && lfirst * 4 < x + seg[k].count) segat = k;
                x += seg[k].count;
            }
            m7t_print("  line $"); m7t_hex((uint32_t)i);
            m7t_print(" sel $"); m7t_hex(lines[i].sel);
            m7t_print(" kinds $"); m7t_hex((uint32_t)kinds);
            m7t_print(" words $"); m7t_hex((uint32_t)lb);
            m7t_print(" first word $"); m7t_hex((uint32_t)lfirst);
            m7t_print(" in segment kind $"); m7t_hex(segat >= 0 ? seg[segat].kind : 0);
            m7t_print(" a $"); m7t_hex((uint32_t)lines[i].a);
            m7t_print(" c $"); m7t_hex((uint32_t)lines[i].c);
            m7t_print("\n");
            shown++;
        }
    }
    m7t_print("M7TEST lines $");
    m7t_hex(LINES);
    m7t_print(" segments $");
    m7t_hex((uint32_t)nsegs);
    m7t_print(" differing words $");
    m7t_hex((uint32_t)bad);
    if (bad) {
        m7t_print(" first at word $");
        m7t_hex((uint32_t)firstbad);
    }
    m7t_print("\n");
    return bad != 0;
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

/* ppu_win_spans against a per-pixel evaluation of the window rules, over
 * random register sets (TMW, the selects, the edges, the logic).
 *
 *   build/win_test [count] */
#include "snes_ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static int hidden(const PpuRegs *s, int layer, int x)
{
    if (!(s->r[0x2E] & (1 << layer))) return 0;
    uint8_t sel = layer == 4 ? s->r[0x25] : (uint8_t)(s->r[0x23 + (layer >> 1)] >> ((layer & 1) * 4));
    int en1 = sel & 2, en2 = sel & 8;
    int a = (s->r[0x26] <= x && x <= s->r[0x27]) ^ (sel & 1);
    int b = (s->r[0x28] <= x && x <= s->r[0x29]) ^ ((sel >> 2) & 1);
    int logic = layer == 4 ? s->r[0x2B] & 3 : (s->r[0x2A] >> (layer * 2)) & 3;
    if (!en1 && !en2) return 0;
    if (!en2) return a;
    if (!en1) return b;
    switch (logic) {
    case 0: return a | b;
    case 1: return a & b;
    case 2: return a ^ b;
    default: return !(a ^ b);
    }
}

int main(int argc, char **argv)
{
    long count = argc > 1 ? atol(argv[1]) : 200000, bad = 0;
    static PpuRegs s;
    for (long t = 0; t < count; t++) {
        memset(s.r, 0, sizeof s.r);
        for (int n = 0x23; n <= 0x2B; n++) s.r[n] = (uint8_t)rnd();
        s.r[0x2C] = (uint8_t)rnd();
        s.r[0x2E] = (uint8_t)rnd();
        /* edges near each other and the ends more often */
        for (int n = 0x26; n <= 0x29; n++)
            if (rnd() & 1) s.r[n] = (uint8_t)((rnd() & 1) ? (rnd() & 7) : 255 - (rnd() & 7));
        int layers = 0;
        for (int layer = 0; layer < 5; layer++) {
            uint16_t sp[3][2];
            int n = ppu_win_spans(&s, layer, sp);
            uint8_t got[256] = {0};
            int prev = -1;
            for (int k = 0; k < n; k++) {
                if (sp[k][1] == 0 || sp[k][0] <= prev || sp[k][0] + sp[k][1] > 256) bad++;
                prev = sp[k][0] + sp[k][1];
                for (int x = sp[k][0]; x < sp[k][0] + sp[k][1] && x < 256; x++) got[x] = 1;
            }
            int any = 0;
            for (int x = 0; x < 256; x++) {
                int h = hidden(&s, layer, x);
                any |= h;
                if (h != got[x]) {
                    if (bad < 5)
                        printf("layer %d x %d: spans %d, rule %d (TMW %02X sel %02X %02X %02X WH %d-%d %d-%d log %02X %02X)\n",
                               layer, x, got[x], h, s.r[0x2E], s.r[0x23], s.r[0x24], s.r[0x25],
                               s.r[0x26], s.r[0x27], s.r[0x28], s.r[0x29], s.r[0x2A], s.r[0x2B]);
                    bad++;
                    break;
                }
            }
            if (any && (s.r[0x2C] & (1 << layer))) layers |= 1 << layer;
        }
        if (ppu_win_layers(&s) != layers) bad++;
    }
    printf("%ld register sets: %ld mismatches\n", count, bad);
    return bad != 0;
}

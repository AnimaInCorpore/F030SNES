/* Private reference functions stay in this research translation unit.
 * Do not link src/snes_ppu.c again, or use this full renderer on the Falcon. */
#include "../../src/snes_ppu.c"
#include "occlusion_ref.h"
#include <string.h>
typedef struct { const PpuInput *in; OcclusionFrame *out; } OcclCtx;
static void occl_row(void *ctx, int y, const PpuRegs *s)
{
    OcclCtx *c = ctx;
    OcclusionFrame *f = c->out;
    int mode = s->r[5] & 7;
    uint32_t sub = ppu_sub_cfg(s, c->in->cgram);
    int tm = s->r[0x2C] & 31, ts = sub ? sub & 31 : 0;
    f->layers[y] = (uint8_t)(tm | ts);
    f->blend[y] = sub != 0;
    f->mode[y] = (uint8_t)mode;
    compose_row(c->in, s, y, f->pixels[y]);
    if (s->r[0] & 0x80) return;
    static Pix bg[4][PPU_W];
    static uint16_t obj[PPU_W];
    static uint8_t hid[5][PPU_W];
    memset(bg, 0, sizeof bg);
    memset(hid, 0, sizeof hid);
    if (mode == 7) { if ((tm | ts) & 1) mode7_line(c->in, s, y, bg[0]); }
    else bg_layers(c->in, s, y, (uint8_t)(tm | ts), bg);
    if ((tm | ts) & 16) obj_line(c->in, s, y, obj);
    else memset(obj, 0, sizeof obj);
    for (int b = 0; b < 5; b++) {
        uint16_t spans[3][2];
        for (int k = 0, n = ppu_win_spans(s, b, spans); k < n; k++)
            memset(hid[b] + spans[k][0], 1, spans[k][1]);
    }
    for (int x = 0; x < PPU_W; x++) {
        for (int b = 0; b < 4; b++) if ((bg[b][x] & 0x200) && !hid[b][x])
            f->opaque[b][y][x >> 4] |= (uint16_t)(1u << (x & 15));
        int ms, ss;
        int mi = front(mode, (s->r[5] & 8) != 0, bg, obj, (uint8_t)tm, hid, x, &ms);
        (void)front(mode, (s->r[5] & 8) != 0, bg, obj, (uint8_t)ts, hid, x, &ss);
        uint8_t used = (uint8_t)(1u << ms);
        f->main_used[y][x] = used;
        if (sub && ss != 5) {
            int en = ms == 5 ? (sub >> 13) & 1 : ms == 4 ? ((sub >> 12) & 1) && mi >= 192 : (sub >> (8 + ms)) & 1;
            if (en || (ms == 5 && (sub & 0x20000))) used |= (uint8_t)(1u << ss);
        }
        f->both_used[y][x] = used;
    }
}
void occlusion_reference(const PpuInput *in, OcclusionFrame *out)
{
    memset(out, 0, sizeof *out);
    OcclCtx c = {in, out};
    ppu_replay(in, occl_row, &c);
}

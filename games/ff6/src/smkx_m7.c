/* Falcon side of the DSP Mode 7 path: the renderer's PpuM7Sink.
 *
 * Mode 7 rows go to the kernel (falcon/dsp/m7c2p.asm) as planned segments;
 * it samples, maps CGRAM indices to palette slots and does the c2p, and its
 * plane words stream over the SSI into DMA record. One record window per
 * frame starts at the first Mode 7 row and runs to the last row of the
 * screen: rows after it that the CPU composes get a cheap FILL line from
 * the kernel and are overwritten once the window is full. The CPU then
 * writes its own rows and lays sprites over the DSP rows.
 *
 * Mode 7 VRAM (words 0-$3FFF) goes up in 256-word pages the runtime marks
 * dirty; the colour tables go up when the slot map changes.
 * Freestanding (m68k-atari-mintelf-gcc -ffreestanding). */
#include "smkx_m7.h"
#include "m7_plan.h"
#include "smkx_prof.h"

extern uint8_t rt_vram_r[];               /* the renderer's copy (rt_vram_sync) */
extern uint8_t rt_m7_dirty[M7_PAGES];      /* per 256-word VRAM page */
extern void rt_m7_record(void *start, uint32_t bytes);
extern int rt_m7_record_done(void);         /* also clears the data cache */

uint8_t smkx_m7_on;                         /* kernel booted, route ours */
uint32_t smkx_m7_rows, smkx_m7_segs, smkx_m7_pages, smkx_m7_timeouts;
uint32_t smkx_m7_spins;                     /* host-port polls that found it full */
uint32_t smkx_m7_wait;                      /* last frame: waiting for the window, 1/38400 s */

#define HSR (*(volatile uint8_t *)0xFFFFA202)
#define HTX (*(volatile uint32_t *)0xFFFFA204)  /* $A204 is unused: a long
                                                   write sends TXH, TXM, TXL */
#define HRX (*(volatile uint32_t *)0xFFFFA204)

static inline void put(uint32_t w)
{
    while (!(HSR & 2)) smkx_m7_spins++;
    HTX = w;
}

static uint8_t sent_map[256];
static int map_valid;
static uint16_t *screen;
static int recording;
static uint8_t rowkind[PPU_H];              /* ROW_CPU, ROW_FILL, ROW_DSP */
enum { ROW_CPU, ROW_FILL, ROW_DSP };
static PpuM7Line rowline[PPU_H];            /* kept for smkx_m7_replay */
static int first_row;

extern const uint8_t m7_image[];
extern const uint32_t m7_image_bytes;
extern void rt_dsp_boot(const uint8_t *image, uint32_t bytes);

int smkx_m7_boot(void)
{
    smkx_m7_on = 0;
    rt_dsp_boot(m7_image, m7_image_bytes);
    return smkx_m7_init();
}

int smkx_m7_init(void)
{
    /* The kernel answers PING once its tables are built. */
    uint32_t spin;
    put(4);
    for (spin = 0; spin < 4000000 && !(HSR & 1); spin++) {}
    if (!(HSR & 1)) return 0;
    if ((HRX & 0xFFFFFF) != 0x4D3743) return 0;
    for (int i = 0; i < M7_PAGES; i++) rt_m7_dirty[i] = 1;
    map_valid = 0;
    smkx_m7_on = 1;
    return 1;
}

static void upload(const uint8_t *map)
{
    for (int p = 0; p < M7_PAGES; p++) {
        if (!rt_m7_dirty[p]) continue;
        rt_m7_dirty[p] = 0;
        smkx_m7_pages++;
        put(5);
        put((uint32_t)p * 256);
        put(256);
        const uint8_t *v = rt_vram_r + p * 512;
        for (int i = 0; i < 256; i++, v += 2)
            put(((uint32_t)v[0] << 14) | v[1]);     /* (tile * 64) << 8 | texel */
    }
    int same = map_valid;
    for (int i = 0; same && i < 256; i++) same = sent_map[i] == map[i];
    if (!same) {
        put(2);
        for (int b = 0; b < 4; b++)
            for (int i = 0; i < 256; i++)
                put(((uint32_t)(map[i] >> b) & 1) << 8);
        for (int i = 0; i < 256; i++) sent_map[i] = map[i];
        map_valid = 1;
    }
}

static void send_line(const PpuM7Line *l)
{
    M7Seg seg[M7_MAX_SEGS];
    int n = m7_plan(l, seg);
    smkx_m7_segs += (uint32_t)n;
    put(3);
    for (int k = 0; k < n; k++) {
        put(seg[k].kind);
        put(seg[k].count);
        if (seg[k].kind == M7_FILL) {
            put(0);                         /* transparent: the backdrop */
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

static void send_fill(void)
{
    put(3);
    put(M7_FILL);
    put(256);
    put(0);
}

static void m7_row(void *ctx, int row, const PpuM7Line *l)
{
    (void)ctx;
    if (!recording) {
        rt_m7_record(screen + row * 64, (uint32_t)(PPU_H - row) * 128);
        recording = 1;
        first_row = row;
    }
    rowkind[row] = ROW_DSP;
    rowline[row] = *l;
    smkx_m7_rows++;
    send_line(l);
}

static void cpu_row(void *ctx, int row)
{
    (void)ctx;
    if (!recording) {
        rowkind[row] = ROW_CPU;
        return;
    }
    rowkind[row] = ROW_FILL;
    send_fill();
}

uint32_t smkx_m7_replay(uint16_t *scratch)
{
    if (!smkx_m7_on || first_row < 0) return 0;
    uint32_t t0 = prof_now();
    rt_m7_record(scratch + first_row * 64, (uint32_t)(PPU_H - first_row) * 128);
    for (int row = first_row; row < PPU_H; row++) {
        if (rowkind[row] == ROW_DSP) send_line(&rowline[row]);
        else send_fill();
    }
    while (!rt_m7_record_done()) {}
    return prof_now() - t0;
}

static const PpuM7Sink sink = {m7_row, cpu_row, 0};

const PpuM7Sink *smkx_m7_begin(uint16_t *planar, const uint8_t *map)
{
    if (!smkx_m7_on) return 0;
    screen = planar;
    recording = 0;
    first_row = -1;
    upload(map);
    return &sink;
}

static void planar_row(const uint8_t *c, uint16_t *w)
{
    for (int g = 0; g < 16; g++, c += 16, w += 4) {
        uint16_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (int k = 0; k < 16; k++) {
            uint16_t bit = (uint16_t)(0x8000 >> k);
            uint8_t v = c[k];
            if (v & 1) p0 |= bit;
            if (v & 2) p1 |= bit;
            if (v & 4) p2 |= bit;
            if (v & 8) p3 |= bit;
        }
        w[0] = p0;
        w[1] = p1;
        w[2] = p2;
        w[3] = p3;
    }
}

/* Sprites (and other CPU pixels) over a DSP row: PPU_EXT keeps the plane. */
static void overlay_row(const uint8_t *c, uint16_t *w)
{
    for (int g = 0; g < 16; g++, c += 16, w += 4) {
        uint16_t m = 0, p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (int k = 0; k < 16; k++) {
            uint8_t v = c[k];
            if (v == PPU_EXT) continue;
            uint16_t bit = (uint16_t)(0x8000 >> k);
            m |= bit;
            if (v & 1) p0 |= bit;
            if (v & 2) p1 |= bit;
            if (v & 4) p2 |= bit;
            if (v & 8) p3 |= bit;
        }
        if (!m) continue;
        w[0] = (uint16_t)((w[0] & ~m) | p0);
        w[1] = (uint16_t)((w[1] & ~m) | p1);
        w[2] = (uint16_t)((w[2] & ~m) | p2);
        w[3] = (uint16_t)((w[3] & ~m) | p3);
    }
}

void smkx_m7_finish(const uint8_t *chunky)
{
    smkx_m7_wait = 0;
    if (recording) {
        uint32_t spin, t0 = prof_now();
        for (spin = 0; spin < 20000000 && !rt_m7_record_done(); spin++) {}
        if (spin == 20000000) {
            /* The kernel or the route stalled: the CPU renders from now on. */
            smkx_m7_timeouts++;
            smkx_m7_on = 0;
        }
        smkx_m7_wait = prof_now() - t0;
    }
    for (int row = 0; row < PPU_H; row++) {
        const uint8_t *c = chunky + row * PPU_W;
        uint16_t *w = screen + row * 64;
        if (recording && rowkind[row] == ROW_DSP) overlay_row(c, w);
        else planar_row(c, w);
    }
    recording = 0;
}

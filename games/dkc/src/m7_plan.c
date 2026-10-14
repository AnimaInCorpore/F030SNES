/* Mode 7 line planner: splits one PpuM7Line into the segments the DSP
 * kernel (falcon/dsp/m7c2p.asm) renders, so that its inner loop never
 * checks ranges and its 24-bit coordinates never wrap.
 *
 *   INSIDE  u, v stay in 0..$3FFFF (texels 0..1023) over the segment
 *   TILE0   outside, M7SEL = $C0: tile 0's texels, only fine bits matter
 *   FILL    outside, M7SEL = $80: transparent (CGRAM index 0)
 * With M7SEL bit 7 clear the plane wraps: the line is cut wherever u or v
 * crosses a multiple of 1024 texels and each piece is an INSIDE segment
 * with its coordinates reduced.
 *
 * The result equals ppu_m7_pixels() pixel for pixel; M7TEST checks it. */
#include "m7_plan.h"

#define SPAN 0x40000L          /* 1024 texels in 1/256 units */

/* floor(n / d) and ceil(n / d) for d > 0 */
static int32_t fdiv(int32_t n, int32_t d) { return n >= 0 ? n / d : -((-n + d - 1) / d); }
static int32_t cdiv(int32_t n, int32_t d) { return n >= 0 ? (n + d - 1) / d : -((-n) / d); }

/* Pixels x in [lo, hi] with 0 <= w0 + d*x < SPAN, clipped to 0..255. */
static void inside_range(int32_t w0, int32_t d, int *lo, int *hi)
{
    int32_t a, b;
    if (d == 0) {
        if (w0 >= 0 && w0 < SPAN) { a = 0; b = 255; }
        else { a = 1; b = 0; }
    } else if (d > 0) {
        a = cdiv(-w0, d);
        b = fdiv(SPAN - 1 - w0, d);
    } else {
        a = cdiv(w0 - (SPAN - 1), -d);
        b = fdiv(w0, -d);
    }
    if (a < 0) a = 0;
    if (b > 255) b = 255;
    *lo = (int)a;
    *hi = (int)b;
}

/* Floor of w / SPAN, as the wrap period index. */
static int32_t period(int32_t w) { return fdiv(w, SPAN); }

static int add(M7Seg *s, int n, int kind, int count, int32_t u, int32_t v, int32_t a, int32_t c)
{
    if (count <= 0) return n;
    s[n].kind = (uint8_t)kind;
    s[n].count = (uint16_t)count;
    s[n].u = u;
    s[n].v = v;
    s[n].du = a;
    s[n].dv = c;
    return n + 1;
}

static int outside(const PpuM7Line *l, M7Seg *s, int n, int x0, int x1)
{
    if (x1 <= x0) return n;
    int32_t u = l->vx + l->a * x0, v = l->vy + l->c * x0;
    if ((l->sel & 0x40) == 0)
        return add(s, n, M7_FILL, x1 - x0, 0, 0, 0, 0);
    /* tile 0: only u, v mod 2048 matter, so start and steps are reduced
     * mod 2048 as well; over 256 pixels u, v then stay in 0..$7FFFF (fine
     * tables of 2048 entries) and the packed coordinates never carry */
    return add(s, n, M7_TILE0, x1 - x0, u & 2047, v & 2047, l->a & 2047, l->c & 2047);
}

int m7_plan(const PpuM7Line *l, M7Seg *s)
{
    int n = 0;
    if (l->sel & 0x80) {
        int ulo, uhi, vlo, vhi;
        inside_range(l->vx, l->a, &ulo, &uhi);
        inside_range(l->vy, l->c, &vlo, &vhi);
        int lo = ulo > vlo ? ulo : vlo, hi = uhi < vhi ? uhi : vhi;
        if (lo > hi) return outside(l, s, 0, 0, 256);
        n = outside(l, s, n, 0, lo);
        n = add(s, n, M7_INSIDE, hi - lo + 1, l->vx + l->a * lo, l->vy + l->c * lo, l->a, l->c);
        return outside(l, s, n, hi + 1, 256);
    }
    /* wrapping plane: cut where either coordinate changes period */
    int x = 0;
    while (x < 256) {
        int32_t u = l->vx + l->a * x, v = l->vy + l->c * x;
        int32_t pu = period(u), pv = period(v);
        int end = 256;
        if (l->a > 0) { int32_t e = cdiv((pu + 1) * SPAN - l->vx, l->a); if (e < end) end = (int)e; }
        if (l->a < 0) { int32_t e = cdiv(l->vx - (pu * SPAN - 1), -l->a); if (e < end) end = (int)e; }
        if (l->c > 0) { int32_t e = cdiv((pv + 1) * SPAN - l->vy, l->c); if (e < end) end = (int)e; }
        if (l->c < 0) { int32_t e = cdiv(l->vy - (pv * SPAN - 1), -l->c); if (e < end) end = (int)e; }
        if (end <= x) end = x + 1;
        n = add(s, n, M7_INSIDE, end - x, u - pu * SPAN, v - pv * SPAN, l->a, l->c);
        x = end;
    }
    return n;
}

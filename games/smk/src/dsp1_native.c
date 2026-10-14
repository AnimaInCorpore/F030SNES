/* Native DSP-1 commands: see dsp1_native.h. Each routine follows the
 * firmware (uPD77C25 program in assets/dsp1.bin) instruction by
 * instruction on values only: flags where a branch or SGN reads them,
 * registers where later code does. Comments give program addresses. */
#include "dsp1_native.h"

typedef struct {
    uint16_t a, b, k, l, tr, trb, dp, rp;
    uint8_t ov1, s1;                    /* flag A's OV1 and S1 (SGN reads S1) */
} Regs;

#define S16(x) ((int16_t)(x))

/* The multiplier: M = K * L >> 15, N = K * L << 1 (16 bits each). */
static inline uint16_t mul_m(uint16_t k, uint16_t l)
{
    return (uint16_t)(((int32_t)S16(k) * S16(l)) >> 15);
}

static inline uint16_t mul_n(uint16_t k, uint16_t l)
{
    return (uint16_t)((uint32_t)((int32_t)S16(k) * S16(l)) << 1);
}

static inline uint16_t sign_of(uint16_t v)
{
    return S16(v) < 0 ? 0xFFFF : 0;
}

/* Flag A after an arithmetic result: on signed overflow S1 takes the true
 * sign (OV1 ^ !sign) and OV1 toggles (upd7725.UPD.alu). Returns OV0. */
static inline int a_flags(Regs *r, int ov0, uint16_t res)
{
    if (ov0) {
        r->s1 = (uint8_t)(r->ov1 ^ ((res & 0x8000) ? 0 : 1));
        r->ov1 ^= 1;
    }
    return ov0;
}

static inline uint16_t a_add(Regs *r, uint16_t q, uint16_t p)
{
    uint16_t s = (uint16_t)(q + p);
    a_flags(r, ((q ^ s) & (p ^ s) & 0x8000) != 0, s);
    return s;
}

static inline uint16_t a_sub(Regs *r, uint16_t q, uint16_t p)
{
    uint16_t s = (uint16_t)(q - p);
    a_flags(r, ((q ^ s) & (q ^ p) & 0x8000) != 0, s);
    return s;
}

/* $03A: A shifted left until the next shift would overflow, B -= shifts;
 * A = 0 gives B = 0. L ends as A; DP cleared. The AND clears OV1; the
 * shift that overflows sets S1 = !(its result's sign) and OV1. */
static inline void norm(Regs *r)
{
    uint16_t a = r->a;
    r->dp = 0;
    r->ov1 = 0;
    if (!a) {
        r->l = 0;
        r->b = 0;
        return;
    }
    uint16_t x = (a & 0x8000) ? (uint16_t)~a : a;
    int n = __builtin_clz(((uint32_t)x << 16) | 0x8000) - 1;
    a = (uint16_t)(a << n);
    r->a = r->l = a;
    r->b = (uint16_t)(r->b - n);
    r->s1 = (uint8_t)(((uint16_t)(a << 1) & 0x8000) ? 0 : 1);
    r->ov1 = 1;
}

/* $042: the 32-bit value A:B (high:low) as mantissa A, exponent B. */
static void norm32(Regs *r, const uint16_t *rom)
{
    uint16_t hi = r->a, lo = r->b;
    r->ov1 = 0;                                             /* $042/$044 CMP */
    if (hi == 0xFFFF || hi == 0) {                          /* $057 / $051 */
        int half = hi ? S16(lo) >= 0 : S16(lo) < 0;
        if (half) {                                         /* $053 */
            r->a = (uint16_t)((uint16_t)(S16(lo) >> 1) ^ 0x8000);
            r->b = 0x10;
            r->k = 0x8000;
        } else {                                            /* $059 */
            r->a = lo;
            r->b = 15;
            norm(r);
        }
        return;
    }
    uint16_t k = (uint16_t)(S16(lo) >> 1) & 0x7FFF;         /* $046-$049 */
    r->k = k;
    r->b = 0x40;
    norm(r);                                                /* $04B */
    r->rp = r->b & 0x3FF;
    r->l = rom[r->rp];
    r->a = a_add(r, r->a, mul_m(k, r->l));                  /* $04E */
    r->rp = 0x21;
    r->b = (uint16_t)(r->b - 0x21);
}

/* $039: exponent 15, normalized. */
static inline void norm15(Regs *r)
{
    r->b = 15;
    norm(r);
}

/* $077: normalize, then scale back to 16 bits (ROM[$31 + B - 15]) or clip. */
static void denorm16(Regs *r, const uint16_t *rom)
{
    norm(r);
    uint16_t b = (uint16_t)(r->b - 0x0F);
    if (S16(b) < 0) {
        r->rp = (uint16_t)(b + 0x31) & 0x3FF;
        r->k = r->a;
        r->l = rom[r->rp];
        r->a = mul_m(r->k, r->l);
        r->ov1 = 0;                                         /* $07E XOR; 0 + M */
    } else if (b) {
        r->a = S16(r->a) < 0 ? 0x8001 : 0x7FFF;
        r->ov1 = 0;                                         /* $081 AND */
    }
    r->b = 0;
}

/* $087: normalize, then the 32-bit value A:B scaled by 2^(B - 15), clipped
 * to $7FFF:$FFFF / $8001:$0000. */
static void denorm32(Regs *r, const uint16_t *rom)
{
    norm(r);
    uint16_t b = (uint16_t)(r->b - 0x0F);
    r->ov1 = 0;                                             /* the next A op is logic */
    if (!b) {                                               /* $09A */
        r->b = r->a;
        r->a = sign_of(r->a);
        return;
    }
    if (S16(b) < 0) {                                       /* $08C */
        r->rp = (uint16_t)(b + 0x31) & 0x3FF;
        r->k = r->a;
        r->l = rom[r->rp];
        r->b = mul_m(r->k, r->l);
        r->a = sign_of(r->b);
        return;
    }
    uint16_t lo = r->a, hi = sign_of(lo);                   /* $094-$098 */
    r->tr = b;
    r->rp = (uint16_t)(0x41 - b) & 0x3FF;                   /* $05B-$062 */
    r->k = hi;
    r->l = rom[r->rp];
    uint16_t a = mul_m(hi, r->l);
    if (S16(a) < 0) {                                       /* $06A */
        if ((uint16_t)~a)
            goto neg;
        a = mul_n(hi, r->l);
        if (S16(a) >= 0)
            goto neg;
    } else {
        if (a)
            goto pos;
        a = mul_n(hi, r->l);
        if (S16(a) < 0)
            goto pos;
    }
    if (S16(lo) < 0) {                                      /* $070-$073 */
        a = a_add(r, a, r->l);                              /* L = ROM[RP] */
        a = a_add(r, a, r->l);
    }
    a = a_add(r, a, mul_m(lo, r->l));                       /* $075 */
    r->k = lo;
    r->a = a;
    r->b = mul_n(lo, r->l);
    return;
pos:
    r->a = 0x7FFF;
    r->b = 0xFFFF;
    return;
neg:
    r->a = 0x8001;
    r->b = 0;
}

/* A doubled, $7FFF on overflow ($0E4-$0E6, $0EC-$0EE). */
static inline uint16_t twice_clip(Regs *r, uint16_t a)
{
    uint16_t s = (uint16_t)(a << 1);
    if (a_flags(r, ((a ^ s) & 0x8000) != 0, s))
        return 0x7FFF;
    return s;
}

/* $0D3: A = the inverse of the mantissa A, B = 1 - B (A = 0: $7FFF, $2F). */
static void inverse(Regs *r, const uint16_t *rom)
{
    uint16_t a = r->a;
    int neg = 0;
    r->dp &= 0xF0;
    r->ov1 = 0;                                             /* $0D3 AND */
    if (!a) {                                               /* $0F2 */
        r->a = 0x7FFF;
        r->b = 0x002F;
        return;
    }
    if (S16(a) < 0) {                                       /* $0D6-$0D9 */
        neg = 1;
        a = a_add(r, (uint16_t)~a, 1);
        if (a == 0x8000)
            a = a_sub(r, a, 1);
    }
    uint16_t k = a, l = rom[0x64];                          /* $0DB */
    uint16_t acc = a_add(r, rom[0x63], mul_m(k, l));        /* $0DC-$0DD */
    uint16_t tr = rom[0x62];
    r->rp = acc & 0x3FF;                                    /* $0DE-$0DF */
    r->ov1 = 0;
    l = rom[r->rp];
    acc = a_sub(r, 0, mul_m(k, l));                         /* $0E0-$0E1 */
    acc = a_sub(r, acc, tr);
    r->ov1 = 0;                                             /* $0E2/$0E3 XOR, OR */
    acc = twice_clip(r, mul_m(acc, l));
    l = acc;                                                /* $0E7 */
    r->ov1 = 0;
    acc = a_sub(r, 0, mul_m(k, l));                         /* $0E8-$0E9 */
    acc = a_sub(r, acc, tr);
    r->ov1 = 0;                                             /* $0EA/$0EB */
    acc = twice_clip(r, mul_m(acc, l));
    if (neg) {                                              /* $0F0-$0F1 */
        r->ov1 = 0;
        acc = a_add(r, (uint16_t)~acc, 1);
    }
    r->a = acc;
    r->b = (uint16_t)(1 - r->b);
    r->k = acc;
    r->l = l;
    r->tr = tr;
    r->trb = k;
}

/* $09F: the vector at RAM[$76/$75/$74] with exponents RAM[$F6/$F5/$F4]
 * brought to the largest exponent. */
static void align3(uint16_t *ram, const uint16_t *rom)
{
    uint16_t ex = ram[0xF6], ey = ram[0xF5], ez = ram[0xF4], emax;
    if (S16((uint16_t)(ex - ey)) >= 0)                      /* $0A2 */
        emax = S16((uint16_t)(ex - ez)) >= 0 ? ex : ez;
    else
        emax = S16((uint16_t)(ey - ez)) < 0 ? ez : ey;
    uint16_t b = (uint16_t)(0x31 + emax);
    for (int i = 0; i < 3; i++) {                           /* $0B0-$0C1 */
        uint16_t rp = (uint16_t)(b - ram[0xF6 - i]) & 0x3FF;
        uint16_t m = mul_m(ram[0x76 - i], rom[rp]);
        ram[0xF6 - i] = emax;
        ram[0x76 - i] = m;
    }
}

/* $132: A = cosine-like, B = sine-like of the angle A (tables at $118+). */
static void sincos(Regs *r, const uint16_t *rom)
{
    uint16_t a = r->a;
    int neg = 0;
    if (S16(a) < 0) {                                       /* $134-$136 */
        neg = 1;
        a = (uint16_t)(~a + 1);
        if (a == 0x8000) {                                  /* $14F */
            r->a = 0x8000;
            r->b = 0;
            return;
        }
    }
    uint16_t k = a;                                         /* $137-$13C */
    uint16_t idx = (uint16_t)(mul_m(k, rom[0x21D]) + rom[0x21B]);
    uint16_t b = rom[0x21A] & mul_n(k, rom[0x21C]);
    uint16_t trb = rom[0x219];
    k = b;                                                  /* $13D-$140 */
    b = mul_m(k, rom[0x218]);
    uint16_t rp = idx & 0x3FF;
    k = b;
    uint16_t tr = rom[rp];
    b = mul_m(k, tr);
    idx = (uint16_t)(idx + trb);                            /* $141-$146 */
    rp = idx & 0x3FF;
    uint16_t l = rom[rp];
    uint16_t s = (uint16_t)(l - b);
    a = ((l ^ s) & (l ^ b) & 0x8000) ? 0x8001 : s;
    uint16_t m = mul_m(k, l), t = (uint16_t)(tr + m);       /* $147-$14A */
    b = ((tr ^ t) & (m ^ t) & 0x8000) ? 0x7FFF : t;
    if (neg)                                                /* $14C-$14D */
        b = (uint16_t)(~b + 1);
    r->a = a;
    r->b = b;
}

/* $123: the square root of mantissa A, exponent B (tables at $116+). */
static void root(Regs *r, const uint16_t *rom)
{
    uint16_t a = r->a, b = r->b;
    int odd = b & 1;
    b = (uint16_t)(S16(b) >> 1);                            /* $124-$127 */
    if (odd) {
        a = (uint16_t)(S16(a) >> 1);
        b = (uint16_t)(b + 1);
    }
    uint16_t trb = b, k = a;                                /* $128-$12C */
    b = (uint16_t)(0xD6 + mul_m(k, rom[0x117]));
    a = mul_n(k, rom[0x116]);
    uint16_t rp = b & 0x3FF;
    k = a;                                                  /* $12D-$131 */
    b = rom[rp];
    rp = (rp - 1) & 0x3FF;
    a = rom[rp];
    b = (uint16_t)(b - a);
    r->a = (uint16_t)(a + mul_m(k, b));
    r->b = trb;
}

/* $06 project ($3A2-$418): the point X, Y, Z relative to the camera (from
 * parameter) projected: results H, V, M. */
static int project(const uint16_t *in, uint16_t *out, uint16_t *ram, const uint16_t *rom)
{
    Regs r = {0};
    ram[0xBC] = in[0];                                      /* $3A2-$3AF */
    ram[0x3C] = sign_of(in[0]);
    ram[0xAC] = in[1];
    ram[0x2C] = sign_of(in[1]);
    ram[0x9C] = in[2];
    ram[0x1C] = sign_of(in[2]);
    static const uint8_t lo_at[3] = {0xBC, 0xAC, 0x9C};
    for (int i = 0; i < 3; i++) {                           /* $3B0-$3CC */
        uint8_t p = lo_at[i];
        uint32_t v = ((uint32_t)ram[p ^ 0x80] << 16 | ram[p]) -
                     ((uint32_t)ram[(p ^ 0x80) - 1] << 16 | ram[p - 1]);
        r.a = (uint16_t)(v >> 16);
        r.b = (uint16_t)v;
        norm32(&r, rom);
        ram[0x76 - i] = (uint16_t)(S16(r.a) >> 1);
        ram[0xF6 - i] = (uint16_t)(r.b + 1);
    }
    align3(ram, rom);                                       /* $3CD */
    uint16_t emax = ram[0xF6];
    ram[0xF3] = ram[0xF2] = ram[0xF1] = emax;               /* $3D0-$3D5 */
    uint16_t mx = ram[0x76], my = ram[0x75], mz = ram[0x74];
    for (int i = 0; i < 3; i++)                             /* $3D6-$3EC */
        ram[0x73 - i] = (uint16_t)(mul_m(ram[0x7C - i], mx) + mul_m(ram[0x6C - i], my) +
                                   mul_m(ram[0x5C - i], mz));
    r.a = (uint16_t)(0 - ram[0x71]);                        /* $3ED-$3F0 */
    r.b = emax;
    denorm32(&r, rom);
    {                                                       /* $3F1-$3F5 */
        uint16_t pb = ram[0xCE], pa = ram[0x4E];
        uint16_t lo = (uint16_t)(r.b + pb);
        int c = lo < r.b;
        uint16_t hi = (uint16_t)(r.a + pa + c);
        int ov0 = ((r.a ^ hi) & (pa ^ hi) & 0x8000) != 0;
        r.b = lo;
        if (a_flags(&r, ov0, hi))
            hi = (uint16_t)(0x8000 - r.s1);
        r.a = hi;
    }
    norm32(&r, rom);                                        /* $3F6 */
    inverse(&r, rom);                                       /* $3F7 */
    ram[0x71] = r.a;
    ram[0xF1] = r.b;
    r.a = ram[0xCE];                                        /* $3FB-$3FD */
    norm15(&r);
    uint16_t tr = mul_m(ram[0x71], r.a);                    /* $3FE-$402 */
    uint16_t trb = (uint16_t)(r.b + ram[0xF1]);
    r.a = tr;                                               /* $403-$407 */
    r.b = (uint16_t)(8 + trb);
    denorm16(&r, rom);
    ram[0x71] = r.a;
    r.a = mul_m(ram[0x72], tr);                             /* $408-$40D */
    r.b = (uint16_t)(trb + ram[0xF2]);
    denorm16(&r, rom);
    ram[0x72] = r.a;
    r.a = mul_m(ram[0x73], tr);                             /* $40E-$411 */
    r.b = (uint16_t)(trb + ram[0xF3]);
    denorm16(&r, rom);
    out[0] = r.a;                                           /* $412-$417 */
    out[1] = ram[0x72];
    out[2] = ram[0x71];
    return 3;
}

/* $178: the azimuth rotation from angle A at RAM[$7F] on. B returns sine. */
static uint16_t rot_azimuth(uint16_t angle, uint16_t *ram, const uint16_t *rom)
{
    Regs r = {0};
    r.a = angle;
    ram[0x00] = 0x7F;
    sincos(&r, rom);
    uint16_t s = r.b, ns = (uint16_t)(~s + 1);              /* $17B-$17F */
    if (s == 0x8000)
        ns = 0x7FFF;
    ram[0x7F] = r.a;
    ram[0x7E] = ns;
    ram[0x7D] = 0;
    ram[0x6D] = 0;
    ram[0x6E] = r.a;
    ram[0x6F] = s;
    ram[0x5F] = 0;
    ram[0x5E] = 0;
    ram[0x5D] = 0x7FFF;
    return s;
}

/* $165: the zenith rotation from angle A at RAM[$76] on. Returns sine. */
static uint16_t rot_zenith(uint16_t angle, uint16_t *ram, const uint16_t *rom)
{
    Regs r = {0};
    ram[0x76] = 0x7FFF;                                     /* $165-$16E */
    ram[0x66] = 0;
    ram[0x56] = 0;
    ram[0x75] = 0;
    ram[0x74] = 0;
    ram[0x00] = 0x65;
    r.a = angle;
    sincos(&r, rom);
    uint16_t s = r.b, ns = (uint16_t)(~s + 1);              /* $170-$177 */
    if (s == 0x8000)
        ns = 0x7FFF;
    ram[0x65] = r.a;
    ram[0x55] = ns;
    ram[0x64] = s;
    ram[0x54] = r.a;
    return s;
}

/* $02 parameter ($27A-$35E): camera position Fx, Fy, Fz, distances Lfe,
 * Les, angles Aas, Azs. Stores the rotation and camera for project, returns
 * Vof, Vva, Cx, Cy. */
static int parameter(const uint16_t *in, uint16_t *out, uint16_t *ram, const uint16_t *rom)
{
    Regs r = {0};
    ram[0x3A] = in[0];                                      /* $27A-$287 */
    ram[0x2A] = in[1];
    ram[0x1A] = in[2];
    ram[0xCF] = in[3];
    ram[0x4F] = 0;
    ram[0xCE] = in[4];
    ram[0x4E] = 0;
    rot_azimuth(in[5], ram, rom);                           /* $288-$28B */
    uint16_t zs = rot_zenith(in[6], ram, rom);              /* $28C-$28F */
    ram[0x49] = zs;                                         /* $290-$291 */
    for (int row = 0; row < 3; row++) {                     /* $292-$2D7 */
        uint8_t az = (uint8_t)(0x7F - 0x10 * row), to = (uint8_t)(0x7C - 0x10 * row);
        uint16_t b = ram[az], tr = ram[az - 1], trb = ram[az - 2];
        for (int j = 0; j < 3; j++)
            ram[to - j] = (uint16_t)(mul_m(ram[0x76 - j], b) + mul_m(ram[0x66 - j], tr) +
                                     mul_m(ram[0x56 - j], trb));
    }
    uint16_t lfe = ram[0xCF], les = ram[0xCE];              /* $2D8-$2F5 */
    static const uint8_t f_at[3] = {0x3A, 0x2A, 0x1A};
    for (int i = 0; i < 3; i++) {
        uint8_t f = f_at[i];
        uint16_t m = ram[f | 0x40];
        uint16_t a = (uint16_t)(ram[f] + mul_m(m, lfe));    /* the eye */
        uint16_t c = (uint16_t)(a - mul_m(m, les));         /* the centre */
        ram[f ^ 0x80] = a;
        ram[f] = mul_m(a, rom[0x32B]);
        ram[(f ^ 0x80) + 1] = c;
        ram[f + 1] = mul_m(c, rom[0x32B]);
    }
    r.a = ram[0x9A];                                        /* $2F6-$2FA */
    norm15(&r);
    ram[0x4C] = r.a;
    ram[0xCC] = r.b;
    r.b = 0xFFF5;                                           /* $2FB-$300 */
    r.ov1 = 0;                                              /* ($2F9 XOR) */
    r.a = a_add(&r, 0x1200, ram[0xCC]);
    norm(&r);
    inverse(&r, rom);
    denorm16(&r, rom);
    uint16_t tr = r.a;                                      /* $301-$30D */
    uint16_t sgn = (uint16_t)(0x8000 - r.s1);
    uint16_t azs = in[6];
    uint16_t b = (uint16_t)(0 - mul_m(azs, sgn) - tr);
    uint16_t a = (uint16_t)(0 - mul_m(tr, sgn));
    if (S16(b) < 0) {
        a = azs;
        b = 0;
    } else {
        b = mul_m(b, sgn);
        b = (uint16_t)(b << 2 | 3);
    }
    ram[0x40] = b;                                          /* $30E-$313 */
    r.a = a;
    sincos(&r, rom);
    ram[0x48] = r.a;
    ram[0xC9] = r.b;
    r.b = 0;                                                /* $314-$318 */
    norm(&r);
    inverse(&r, rom);
    ram[0x4A] = r.a;
    ram[0xCA] = r.b;
    {                                                       /* $319-$322 */
        uint16_t k = ram[0x40];
        uint16_t t = mul_m(k, rom[0x32A]);
        uint16_t u = (uint16_t)(rom[0x329] + mul_m(k, t));
        uint16_t v = mul_m(k, u);
        uint16_t dp = rom[0x328] & 0xFF;
        uint16_t kk = ram[dp];
        dp = (uint16_t)(((dp & 0xF0) | ((dp - 1) & 0x0F)) ^ 0xC0);
        dp = (uint16_t)((dp & 0xF0) | ((dp - 1) & 0x0F));
        ram[dp] = (uint16_t)(0 - mul_m(kk, v));
    }
    {                                                       /* $323-$32F */
        uint16_t k = mul_m(ram[0x40], ram[0x40]);
        uint16_t w = (uint16_t)(rom[0x327] + mul_m(k, rom[0x326]));
        uint16_t x = mul_m(k, w);
        uint16_t dp = rom[0x325] & 0xFF;
        uint16_t kk = ram[(dp | 0x40) & 0xFF];
        a = (uint16_t)(ram[dp] + mul_m(kk, x));
        dp = rom[0x324] & 0xFF;
        uint16_t y = mul_m(ram[dp], a);
        dp = rom[0x323] & 0xFF;
        ram[dp] = y;
        r.a = a;
    }
    r.b = 0;                                                /* $330-$334 */
    norm(&r);
    inverse(&r, rom);
    ram[0x4B] = r.a;
    ram[0xCB] = r.b;
    r.a = ram[0xC9];                                        /* $335-$339 */
    r.b = 0xFFF1;
    norm(&r);
    inverse(&r, rom);
    uint16_t kinv = r.a;                                    /* $33A-$341 */
    r.a = ram[0xC8];
    norm(&r);
    r.a = (uint16_t)(0 - mul_m(kinv, r.a));
    denorm16(&r, rom);
    uint16_t vva = r.a;
    r.a = ram[0x9A];                                        /* $342-$349 */
    norm15(&r);
    r.a = mul_m(ram[0x4A], r.a);
    r.b = (uint16_t)(r.b + ram[0xCA]);
    denorm16(&r, rom);
    out[0] = ram[0x0C];                                     /* $34A-$34D */
    out[1] = vva;
    a = mul_m(ram[0xC9], r.a);                              /* $34E-$35D */
    uint16_t lx = a;
    a = (uint16_t)(mul_m(ram[0x6F], lx) + ram[0xBA]);
    out[2] = a;
    ram[0xB9] = a;
    a = (uint16_t)(0 - mul_m(ram[0x7F], lx) + ram[0xAA]);
    out[3] = a;
    ram[0xA9] = a;
    return 4;
}

/* $04 triangle ($1F3-$1FE): angle, radius; results radius * sine-like,
 * radius * cosine-like. */
static int triangle(const uint16_t *in, uint16_t *out, const uint16_t *rom)
{
    Regs r = {0};
    r.a = in[0];
    sincos(&r, rom);
    out[0] = mul_m(r.b, in[1]);
    out[1] = mul_m(r.a, in[1]);
    return 2;
}

/* $28 distance ($1BC-$1CE): the length of X, Y, Z. */
static int distance(const uint16_t *in, uint16_t *out, const uint16_t *rom)
{
    Regs r = {0};
    uint16_t hi = mul_m(in[0], in[0]), lo = mul_n(in[0], in[0]);
    for (int i = 1; i < 3; i++) {
        uint16_t n = mul_n(in[i], in[i]), s = (uint16_t)(lo + n);
        hi = (uint16_t)(hi + mul_m(in[i], in[i]) + (s < lo));
        lo = s;
    }
    r.a = hi;
    r.b = lo;
    norm32(&r, rom);
    r.b = (uint16_t)(r.b - 1);
    root(&r, rom);
    denorm16(&r, rom);
    out[0] = r.a;
    return 1;
}

int dsp1n_params(unsigned cmd)
{
    switch (cmd) {
    case 0x00: return 2;                /* multiply */
    case 0x02: return 7;                /* parameter */
    case 0x04: return 2;                /* triangle */
    case 0x06: return 3;                /* project */
    case 0x28: return 3;                /* distance */
    default: return -1;
    }
}

int dsp1n_exec(unsigned cmd, const uint16_t *in, uint16_t *out, uint16_t *ram, const uint16_t *rom)
{
    switch (cmd) {
    case 0x00: out[0] = mul_m(in[0], in[1]); return 1;
    case 0x02: return parameter(in, out, ram, rom);
    case 0x04: return triangle(in, out, rom);
    case 0x06: return project(in, out, ram, rom);
    case 0x28: return distance(in, out, rom);
    default: return 0;
    }
}

#ifdef DSP1N_TEST
void dsp1n_test_sub(unsigned entry, Dsp1nRegs *t, uint16_t *ram, const uint16_t *rom)
{
    Regs r = {t->a, t->b, t->k, t->l, t->tr, t->trb, t->dp, t->rp,
              (uint8_t)(t->ov1 & 1), (uint8_t)(t->s1 & 1)};
    switch (entry) {
    case 0x03A: norm(&r); break;
    case 0x039: norm15(&r); break;
    case 0x042: norm32(&r, rom); break;
    case 0x077: denorm16(&r, rom); break;
    case 0x087: denorm32(&r, rom); break;
    case 0x0D3: inverse(&r, rom); break;
    case 0x09F: align3(ram, rom); break;
    case 0x132: sincos(&r, rom); break;
    case 0x123: root(&r, rom); break;
    }
    t->a = r.a; t->b = r.b; t->k = r.k; t->l = r.l;
    t->tr = r.tr; t->trb = r.trb; t->dp = r.dp; t->rp = r.rp;
    t->ov1 = r.ov1;
    t->s1 = r.s1;
}
#endif

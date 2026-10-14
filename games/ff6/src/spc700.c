/* SPC700 interpreter (see spc700.h). Instruction timing follows the usual
 * cycle table; the timers step from the instruction clock. While the
 * program waits (a timer counter reads 0, or a port reads what it read
 * last time), the clock skips ahead to the next timer step, so a driver
 * idling between ticks costs little.
 * Freestanding C (no library calls). */
#include "spc700.h"

enum { FN = 0x80, FV = 0x40, FP = 0x20, FB = 0x10, FH = 0x08, FI = 0x04, FZ = 0x02, FC = 0x01 };

static const uint8_t cycles[256] = {
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,6,8, 2,8,4,5,4,5,5,6,5,5,6,5,2,2,4,6,
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,5,4, 2,8,4,5,4,5,5,6,5,5,6,5,2,2,3,8,
    2,8,4,5,3,4,3,6,2,6,4,4,5,4,6,6, 2,8,4,5,4,5,5,6,5,5,4,5,2,2,4,3,
    2,8,4,5,3,4,3,6,2,6,4,4,5,4,5,5, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,6,
    2,8,4,5,3,4,3,6,2,6,5,4,5,2,4,5, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,12,5,
    3,8,4,5,3,4,3,6,2,6,4,4,5,2,4,4, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,4,
    3,8,4,5,4,5,4,7,2,5,6,4,5,2,4,9, 2,8,4,5,5,6,6,7,4,5,5,5,2,2,6,3,
    2,8,4,5,3,4,3,6,2,4,5,3,4,3,4,3, 2,8,4,5,4,5,5,6,3,4,5,4,2,2,4,3,
};

static uint32_t period(const Spc *s, int t)
{
    return (t == 2 ? 16u : 128u) * (s->target[t] ? s->target[t] : 256u);
}

/* The earliest step of the enabled timers: what timers() compares the clock with. It is set whenever a
 * step or the enable bits change; with no timer enabled it is far ahead. */
static void due(Spc *s)
{
    uint32_t d = s->cycles + 0x40000000u;
    for (int t = 0; t < 3; t++)
        if ((s->control & (1 << t)) && (int32_t)(s->next_tick[t] - d) < 0)
            d = s->next_tick[t];
    s->tick_due = d;
}

static void timers_due(Spc *s)
{
    for (int t = 0; t < 3; t++) {
        if (!(s->control & (1 << t)))
            continue;
        while ((int32_t)(s->cycles - s->next_tick[t]) >= 0) {
            s->counter[t] = (s->counter[t] + 1) & 15;
            s->next_tick[t] += period(s, t);
        }
    }
    due(s);
}

/* (the check inline: most calls find no step due, and a call would save nine registers first) */
static inline void timers(Spc *s)
{
    if ((int32_t)(s->cycles - s->tick_due) >= 0)
        timers_due(s);
}

/* Waiting: a port read twice in a row with nothing else read between
 * cannot change before the 65816 writes, so the run ends there (the
 * timers' 4-bit counters wrap meanwhile, as on the hardware); a timer
 * counter read as 0 twice in a row waits for that timer's next step. */
static uint16_t last_io = 0xFFFF;
static uint8_t last_val;

static void skip_to(Spc *s, uint32_t to)
{
    if ((int32_t)(to - s->until) > 0)
        to = s->until;
    if ((int32_t)(to - s->cycles) > 0)
        s->cycles = to;
}

static uint8_t io_read(Spc *s, uint16_t a)
{
    uint8_t v;
    int again = last_io == a;
    last_io = a;
    switch (a) {
    case 0xF2: return s->dspaddr;
    case 0xF3: return s->dsp[s->dspaddr & 0x7F];
    case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        v = s->in[a - 0xF4];
        if (again && last_val == v)
            skip_to(s, s->until);
        last_val = v;
        return v;
    case 0xF8: case 0xF9: return s->ram[a];
    case 0xFD: case 0xFE: case 0xFF:
        timers(s);
        v = s->counter[a - 0xFD];
        s->counter[a - 0xFD] = 0;
        if (!v && again && (s->control & (1 << (a - 0xFD)))) {
            skip_to(s, s->next_tick[a - 0xFD]);
            timers(s);
        }
        return v;
    default: return 0;
    }
}

static void io_write(Spc *s, uint16_t a, uint8_t v)
{
    s->ram[a] = v;
    switch (a) {
    case 0xF1:
        timers(s);
        for (int t = 0; t < 3; t++)
            if ((v & (1 << t)) && !(s->control & (1 << t))) {
                s->counter[t] = 0;
                s->next_tick[t] = s->cycles + period(s, t);
            }
        if (v & 0x10) s->in[0] = s->in[1] = 0;
        if (v & 0x20) s->in[2] = s->in[3] = 0;
        s->control = v;
        due(s);
        break;
    case 0xF2: s->dspaddr = v; break;
    case 0xF3:
        if (s->dspaddr < 0x80)
            s->dsp[s->dspaddr] = s->dspaddr == 0x7C ? 0 : v;
        break;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: s->out[a - 0xF4] = v; break;
    case 0xFA: case 0xFB: case 0xFC: timers(s); s->target[a - 0xFA] = v; break;
    default: break;
    }
}

static inline uint8_t rd(Spc *s, uint16_t a)
{
    return (a & 0xFFF0) == 0x00F0 ? io_read(s, a) : s->ram[a];
}

static inline void wr(Spc *s, uint16_t a, uint8_t v)
{
    if ((a & 0xFFF0) == 0x00F0) io_write(s, a, v);
    else s->ram[a] = v;
}

#define DP(o)   ((uint16_t)(((s->psw & FP) ? 0x100 : 0) | ((o) & 0xFF)))
#define FETCH() (s->ram[s->pc++])
#define NZ(v)   (s->psw = (uint8_t)((s->psw & ~(FN | FZ)) | ((v) & 0x80) | (((v) & 0xFF) ? 0 : FZ)))
#define SETC(c) (s->psw = (uint8_t)((s->psw & ~FC) | ((c) ? FC : 0)))

static uint16_t fetch16(Spc *s)
{
    uint16_t lo = FETCH();
    return (uint16_t)(lo | FETCH() << 8);
}

static uint16_t rd16dp(Spc *s, uint8_t o)
{
    return (uint16_t)(rd(s, DP(o)) | rd(s, DP(o + 1)) << 8);
}

static void push(Spc *s, uint8_t v) { s->ram[0x100 | s->sp] = v; s->sp--; }
static uint8_t pop(Spc *s) { s->sp++; return s->ram[0x100 | s->sp]; }

static uint8_t adc(Spc *s, uint8_t a, uint8_t b)
{
    unsigned r = a + b + (s->psw & FC);
    s->psw &= ~(FV | FH | FC);
    if (~(a ^ b) & (a ^ r) & 0x80) s->psw |= FV;
    if ((a ^ b ^ r) & 0x10) s->psw |= FH;
    if (r > 0xFF) s->psw |= FC;
    NZ(r);
    return (uint8_t)r;
}

static uint8_t cmp(Spc *s, uint8_t a, uint8_t b)
{
    unsigned r = (unsigned)a - b;
    SETC(a >= b);
    NZ(r);
    return a;
}

/* The ALU operation of rows 0, 2, 4, 6, 8, A (OR AND EOR CMP ADC SBC). */
static uint8_t alu(Spc *s, int row, uint8_t a, uint8_t b)
{
    switch (row) {
    case 0x0: a |= b; NZ(a); return a;
    case 0x2: a &= b; NZ(a); return a;
    case 0x4: a ^= b; NZ(a); return a;
    case 0x6: return cmp(s, a, b);
    case 0x8: return adc(s, a, b);
    default: return adc(s, a, (uint8_t)~b);
    }
}

static uint8_t shift(Spc *s, int op, uint8_t v)
{
    unsigned c = s->psw & FC;
    switch (op) {
    case 0x0: SETC(v & 0x80); v = (uint8_t)(v << 1); break;          /* ASL */
    case 0x2: SETC(v & 0x80); v = (uint8_t)(v << 1 | c); break;      /* ROL */
    case 0x4: SETC(v & 1); v >>= 1; break;                           /* LSR */
    default:  SETC(v & 1); v = (uint8_t)(v >> 1 | c << 7); break;    /* ROR */
    }
    NZ(v);
    return v;
}

static void branch(Spc *s, int take)
{
    int8_t rel = (int8_t)FETCH();
    if (take) {
        s->pc = (uint16_t)(s->pc + rel);
        s->cycles += 2;
    }
}

static void call(Spc *s, uint16_t to)
{
    push(s, (uint8_t)(s->pc >> 8));
    push(s, (uint8_t)s->pc);
    s->pc = to;
}

/* Operand address of the ALU/MOV columns 4-7 (row even) and 4-7 (row odd). */
static uint16_t ea(Spc *s, int col, int odd)
{
    uint8_t o;
    uint16_t a;
    if (!odd) {
        switch (col) {
        case 4: return DP(FETCH());
        case 5: return fetch16(s);
        case 6: return DP(s->x);
        default: o = FETCH(); return rd16dp(s, (uint8_t)(o + s->x));         /* [dp+X] */
        }
    }
    switch (col) {
    case 4: return DP(FETCH() + s->x);
    case 5: return (uint16_t)(fetch16(s) + s->x);
    case 6: return (uint16_t)(fetch16(s) + s->y);
    default: o = FETCH(); a = rd16dp(s, o); return (uint16_t)(a + s->y);     /* [dp]+Y */
    }
}

static void membit(Spc *s, uint16_t *addr, int *bit)
{
    uint16_t w = fetch16(s);
    *addr = w & 0x1FFF;
    *bit = w >> 13;
}

static void step(Spc *s)
{
    uint8_t op = FETCH(), v, w, o;
    uint16_t a, ya;
    int row = op >> 4, col = op & 15, bit;
    s->cycles += cycles[op];
    s->steps++;

    /* The ALU block: OR AND EOR CMP ADC SBC (even rows), and the same with
     * the indexed modes (odd rows), columns 4-9. */
    if (row < 0xC && col >= 4 && col <= 9) {
        int r = row & 0xE, odd = row & 1;
        if (col <= 7) {
            a = ea(s, col, odd);
            s->a = alu(s, r, s->a, rd(s, a));
            return;
        }
        if (col == 8 && !odd) { s->a = alu(s, r, s->a, FETCH()); return; }
        if (col == 9 && !odd) {                                /* dd, ss */
            uint16_t src = DP(FETCH()), dst = DP(FETCH());
            v = alu(s, r, rd(s, dst), rd(s, src));
            if (r != 0x6) wr(s, dst, v);
            return;
        }
        if (col == 8) {                                        /* dp, #imm */
            uint8_t imm = FETCH();
            uint16_t dst = DP(FETCH());
            v = alu(s, r, rd(s, dst), imm);
            if (r != 0x6) wr(s, dst, v);
            return;
        }
        /* (X), (Y) */
        v = alu(s, r, rd(s, DP(s->x)), rd(s, DP(s->y)));
        if (r != 0x6) wr(s, DP(s->x), v);
        return;
    }
    if (col == 1) {                                            /* TCALL n */
        a = (uint16_t)(0xFFDE - 2 * row);
        call(s, (uint16_t)(s->ram[a] | s->ram[a + 1] << 8));
        return;
    }
    if (col == 2) {                                            /* SET1 / CLR1 dp.bit */
        a = DP(FETCH());
        v = rd(s, a);
        if (row & 1) v &= (uint8_t)~(1 << (row >> 1));
        else v |= (uint8_t)(1 << (row >> 1));
        wr(s, a, v);
        return;
    }
    if (col == 3) {                                            /* BBS / BBC dp.bit */
        v = rd(s, DP(FETCH()));
        branch(s, !!(v & (1 << (row >> 1))) == !(row & 1));
        return;
    }
    if (col == 0 && (row & 1)) {                               /* conditional branches */
        /* 10 BPL 30 BMI 50 BVC 70 BVS 90 BCC B0 BCS D0 BNE F0 BEQ */
        int idx = row >> 1;
        uint8_t f = idx < 2 ? FN : idx < 4 ? FV : idx < 6 ? FC : FZ;
        branch(s, !!(s->psw & f) == (idx & 1));
        return;
    }

    switch (op) {
    case 0x00: break;                                              /* NOP */
    case 0x20: s->psw &= ~FP; break;
    case 0x40: s->psw |= FP; break;
    case 0x60: s->psw &= ~FC; break;
    case 0x80: s->psw |= FC; break;
    case 0xA0: s->psw |= FI; break;
    case 0xC0: s->psw &= ~FI; break;
    case 0xE0: s->psw &= ~(FV | FH); break;
    case 0xED: s->psw ^= FC; break;                                /* NOTC */
    case 0x2F: branch(s, 1); break;                                /* BRA */

    /* shifts on memory and A */
    case 0x0B: case 0x2B: case 0x4B: case 0x6B:
        a = DP(FETCH()); wr(s, a, shift(s, row, rd(s, a))); break;
    case 0x0C: case 0x2C: case 0x4C: case 0x6C:
        a = fetch16(s); wr(s, a, shift(s, row, rd(s, a))); break;
    case 0x1B: case 0x3B: case 0x5B: case 0x7B:
        a = DP(FETCH() + s->x); wr(s, a, shift(s, row - 1, rd(s, a))); break;
    case 0x1C: case 0x3C: case 0x5C: case 0x7C:
        s->a = shift(s, row - 1, s->a); break;

    /* INC / DEC */
    case 0x8B: a = DP(FETCH()); v = (uint8_t)(rd(s, a) - 1); NZ(v); wr(s, a, v); break;
    case 0xAB: a = DP(FETCH()); v = (uint8_t)(rd(s, a) + 1); NZ(v); wr(s, a, v); break;
    case 0x8C: a = fetch16(s); v = (uint8_t)(rd(s, a) - 1); NZ(v); wr(s, a, v); break;
    case 0xAC: a = fetch16(s); v = (uint8_t)(rd(s, a) + 1); NZ(v); wr(s, a, v); break;
    case 0x9B: a = DP(FETCH() + s->x); v = (uint8_t)(rd(s, a) - 1); NZ(v); wr(s, a, v); break;
    case 0xBB: a = DP(FETCH() + s->x); v = (uint8_t)(rd(s, a) + 1); NZ(v); wr(s, a, v); break;
    case 0x9C: s->a--; NZ(s->a); break;
    case 0xBC: s->a++; NZ(s->a); break;
    case 0x1D: s->x--; NZ(s->x); break;
    case 0x3D: s->x++; NZ(s->x); break;
    case 0xDC: s->y--; NZ(s->y); break;
    case 0xFC: s->y++; NZ(s->y); break;

    /* 16-bit */
    case 0x1A: case 0x3A:                                          /* DECW / INCW */
        o = FETCH();
        ya = (uint16_t)(rd16dp(s, o) + (op == 0x3A ? 1 : -1));
        wr(s, DP(o), (uint8_t)ya); wr(s, DP(o + 1), (uint8_t)(ya >> 8));
        s->psw = (uint8_t)((s->psw & ~(FN | FZ)) | (ya & 0x8000 ? FN : 0) | (ya ? 0 : FZ));
        break;
    case 0x5A: {                                                   /* CMPW YA, dp */
        uint16_t m = rd16dp(s, FETCH()), r;
        ya = (uint16_t)(s->y << 8 | s->a);
        r = (uint16_t)(ya - m);
        SETC(ya >= m);
        s->psw = (uint8_t)((s->psw & ~(FN | FZ)) | (r & 0x8000 ? FN : 0) | (r ? 0 : FZ));
        break;
    }
    case 0x7A: case 0x9A: {                                        /* ADDW / SUBW YA, dp */
        uint16_t m = rd16dp(s, FETCH());
        uint32_t r;
        ya = (uint16_t)(s->y << 8 | s->a);
        if (op == 0x9A) m = (uint16_t)~m;
        r = (uint32_t)ya + m + (op == 0x9A ? 1 : 0);
        s->psw &= ~(FV | FH | FC | FN | FZ);
        if (~(ya ^ m) & (ya ^ r) & 0x8000) s->psw |= FV;
        if ((ya ^ m ^ r) & 0x1000) s->psw |= FH;
        if (r > 0xFFFF) s->psw |= FC;
        if (r & 0x8000) s->psw |= FN;
        if (!(r & 0xFFFF)) s->psw |= FZ;
        s->a = (uint8_t)r; s->y = (uint8_t)(r >> 8);
        break;
    }
    case 0xBA:                                                     /* MOVW YA, dp */
        o = FETCH(); s->a = rd(s, DP(o)); s->y = rd(s, DP(o + 1));
        ya = (uint16_t)(s->y << 8 | s->a);
        s->psw = (uint8_t)((s->psw & ~(FN | FZ)) | (ya & 0x8000 ? FN : 0) | (ya ? 0 : FZ));
        break;
    case 0xDA:                                                     /* MOVW dp, YA */
        o = FETCH(); wr(s, DP(o), s->a); wr(s, DP(o + 1), s->y); break;
    case 0xCF:                                                     /* MUL YA */
        ya = (uint16_t)(s->y * s->a); s->a = (uint8_t)ya; s->y = (uint8_t)(ya >> 8); NZ(s->y); break;
    case 0x9E: {                                                   /* DIV YA, X */
        unsigned yv = (unsigned)(s->y << 8 | s->a), x = s->x;
        s->psw &= ~(FV | FH);
        if (s->y >= x) s->psw |= FV;
        if ((s->y & 15) >= (x & 15)) s->psw |= FH;
        if (s->y < (x << 1)) {
            s->a = (uint8_t)(yv / x);
            s->y = (uint8_t)(yv % x);
        } else {
            s->a = (uint8_t)(255 - (yv - (x << 9)) / (256 - x));
            s->y = (uint8_t)(x + (yv - (x << 9)) % (256 - x));
        }
        NZ(s->a);
        break;
    }
    case 0x9F: s->a = (uint8_t)(s->a >> 4 | s->a << 4); NZ(s->a); break;   /* XCN */
    case 0xDF:                                                     /* DAA */
        if ((s->psw & FC) || s->a > 0x99) { s->a += 0x60; s->psw |= FC; }
        if ((s->psw & FH) || (s->a & 15) > 9) s->a += 6;
        NZ(s->a); break;
    case 0xBE:                                                     /* DAS */
        if (!(s->psw & FC) || s->a > 0x99) { s->a -= 0x60; s->psw &= ~FC; }
        if (!(s->psw & FH) || (s->a & 15) > 9) s->a -= 6;
        NZ(s->a); break;

    /* compares with X and Y */
    case 0x1E: cmp(s, s->x, rd(s, fetch16(s))); break;
    case 0x3E: cmp(s, s->x, rd(s, DP(FETCH()))); break;
    case 0xC8: cmp(s, s->x, FETCH()); break;
    case 0x5E: cmp(s, s->y, rd(s, fetch16(s))); break;
    case 0x7E: cmp(s, s->y, rd(s, DP(FETCH()))); break;
    case 0xAD: cmp(s, s->y, FETCH()); break;

    /* loads */
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        s->a = rd(s, ea(s, col, row & 1)); NZ(s->a); break;
    case 0xE8: s->a = FETCH(); NZ(s->a); break;
    case 0xBF: s->a = rd(s, DP(s->x)); s->x++; NZ(s->a); break;   /* MOV A, (X)+ */
    case 0xCD: s->x = FETCH(); NZ(s->x); break;
    case 0xE9: s->x = rd(s, fetch16(s)); NZ(s->x); break;
    case 0xF8: s->x = rd(s, DP(FETCH())); NZ(s->x); break;
    case 0xF9: s->x = rd(s, DP(FETCH() + s->y)); NZ(s->x); break;
    case 0x8D: s->y = FETCH(); NZ(s->y); break;
    case 0xEC: s->y = rd(s, fetch16(s)); NZ(s->y); break;
    case 0xEB: s->y = rd(s, DP(FETCH())); NZ(s->y); break;
    case 0xFB: s->y = rd(s, DP(FETCH() + s->x)); NZ(s->y); break;

    /* stores */
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        wr(s, ea(s, col, row & 1), s->a); break;
    case 0xAF: wr(s, DP(s->x), s->a); s->x++; break;               /* MOV (X)+, A */
    case 0xC9: wr(s, fetch16(s), s->x); break;
    case 0xD8: wr(s, DP(FETCH()), s->x); break;
    case 0xD9: wr(s, DP(FETCH() + s->y), s->x); break;
    case 0xCC: wr(s, fetch16(s), s->y); break;
    case 0xCB: wr(s, DP(FETCH()), s->y); break;
    case 0xDB: wr(s, DP(FETCH() + s->x), s->y); break;
    case 0x8F: v = FETCH(); wr(s, DP(FETCH()), v); break;          /* MOV dp, #imm */
    case 0xFA: v = rd(s, DP(FETCH())); wr(s, DP(FETCH()), v); break; /* MOV dd, ss */

    /* register moves */
    case 0x5D: s->x = s->a; NZ(s->x); break;
    case 0x7D: s->a = s->x; NZ(s->a); break;
    case 0xDD: s->a = s->y; NZ(s->a); break;
    case 0xFD: s->y = s->a; NZ(s->y); break;
    case 0x9D: s->x = s->sp; NZ(s->x); break;
    case 0xBD: s->sp = s->x; break;

    /* stack */
    case 0x0D: push(s, s->psw); break;
    case 0x2D: push(s, s->a); break;
    case 0x4D: push(s, s->x); break;
    case 0x6D: push(s, s->y); break;
    case 0x8E: s->psw = pop(s); break;
    case 0xAE: s->a = pop(s); break;
    case 0xCE: s->x = pop(s); break;
    case 0xEE: s->y = pop(s); break;

    /* jumps and calls */
    case 0x5F: s->pc = fetch16(s); break;
    case 0x1F: a = (uint16_t)(fetch16(s) + s->x); s->pc = (uint16_t)(s->ram[a] | s->ram[(uint16_t)(a + 1)] << 8); break;
    case 0x3F: a = fetch16(s); call(s, a); break;
    case 0x4F: v = FETCH(); call(s, (uint16_t)(0xFF00 | v)); break;
    case 0x6F: v = pop(s); s->pc = (uint16_t)(v | pop(s) << 8); break;
    case 0x7F: s->psw = pop(s); v = pop(s); s->pc = (uint16_t)(v | pop(s) << 8); break;
    case 0x0F:                                                     /* BRK */
        push(s, (uint8_t)(s->pc >> 8)); push(s, (uint8_t)s->pc); push(s, s->psw);
        s->psw = (uint8_t)((s->psw | FB) & ~FI);
        s->pc = (uint16_t)(s->ram[0xFFDE] | s->ram[0xFFDF] << 8);
        break;
    case 0x2E: a = DP(FETCH()); v = rd(s, a); branch(s, v != s->a); break;                                   /* CBNE dp */
    case 0xDE: a = DP(FETCH() + s->x); v = rd(s, a); branch(s, v != s->a); break;                              /* CBNE dp+X */
    case 0x6E: a = DP(FETCH()); v = (uint8_t)(rd(s, a) - 1); wr(s, a, v); branch(s, v != 0); break;          /* DBNZ dp */
    case 0xFE: s->y--; branch(s, s->y != 0); break;                                                           /* DBNZ Y */

    /* TSET1 / TCLR1 */
    case 0x0E: case 0x4E:
        a = fetch16(s); v = rd(s, a);
        NZ((uint8_t)(s->a - v));
        wr(s, a, op == 0x0E ? (uint8_t)(v | s->a) : (uint8_t)(v & ~s->a));
        break;

    /* bit operations on mem.bit */
    case 0x0A: membit(s, &a, &bit); if (rd(s, a) >> bit & 1) s->psw |= FC; break;           /* OR1 C,m.b */
    case 0x2A: membit(s, &a, &bit); if (!(rd(s, a) >> bit & 1)) s->psw |= FC; break;        /* OR1 C,/m.b */
    case 0x4A: membit(s, &a, &bit); if (!(rd(s, a) >> bit & 1)) s->psw &= ~FC; break;       /* AND1 C,m.b */
    case 0x6A: membit(s, &a, &bit); if (rd(s, a) >> bit & 1) s->psw &= ~FC; break;          /* AND1 C,/m.b */
    case 0x8A: membit(s, &a, &bit); if (rd(s, a) >> bit & 1) s->psw ^= FC; break;           /* EOR1 C,m.b */
    case 0xAA: membit(s, &a, &bit); SETC(rd(s, a) >> bit & 1); break;                       /* MOV1 C,m.b */
    case 0xCA: membit(s, &a, &bit); w = rd(s, a);                                           /* MOV1 m.b,C */
        w = (uint8_t)((w & ~(1 << bit)) | ((s->psw & FC) << bit)); wr(s, a, w); break;
    case 0xEA: membit(s, &a, &bit); wr(s, a, (uint8_t)(rd(s, a) ^ (1 << bit))); break;     /* NOT1 m.b */

    case 0xEF: case 0xFF:                                          /* SLEEP, STOP */
    default:
        s->stopped = 1;
        s->bad_pc = (uint16_t)(s->pc - 1);
        s->pc--;
        break;
    }
}

void spc_reset(Spc *s)
{
    uint8_t *p = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++)
        p[i] = 0;
    s->out[0] = 0xAA;
    s->out[1] = 0xBB;
    s->sp = 0xEF;
    s->mode = SPC_IPL_WAIT;
}

/* The boot ROM's upload protocol: $CC on port 0 starts a block at the
 * address in ports 2-3; port 1 = 0 runs it, otherwise bytes follow, each
 * written to port 1 with its index on port 0 and echoed; a port-0 value
 * ahead of the index starts the next block. */
static void ipl_block(Spc *s)
{
    s->ipl_addr = (uint16_t)(s->in[2] | s->in[3] << 8);
    s->out[0] = s->in[0];
    if (s->in[1] == 0) {
        s->mode = SPC_RUN;          /* the echo of port 0 stays: the 65816 waits for it */
        s->pc = s->ipl_addr;
        return;
    }
    s->mode = SPC_IPL_FIRST;
    s->ipl_y = 0;
}

void spc_cpu_write(Spc *s, int port, uint8_t v)
{
    s->in[port & 3] = v;
    if ((port & 3) != 0 || s->mode == SPC_RUN)
        return;
    switch (s->mode) {
    case SPC_IPL_WAIT:
        if (v == 0xCC)
            ipl_block(s);
        break;
    case SPC_IPL_FIRST:
        if (v != 0)
            break;
        s->mode = SPC_IPL_XFER;
        /* fall through */
    case SPC_IPL_XFER:
        if (v == s->ipl_y) {
            s->ram[(uint16_t)(s->ipl_addr + v)] = s->in[1];
            s->out[0] = v;
            s->ipl_y++;
            if (!s->ipl_y)
                s->ipl_addr += 0x100;
        } else if ((uint8_t)(s->ipl_y - v) & 0x80) {
            ipl_block(s);
        }
        break;
    }
}

uint8_t spc_cpu_read(Spc *s, int port)
{
    return s->out[port & 3];
}

void spc_run(Spc *s, uint32_t until)
{
    if (s->mode != SPC_RUN || s->stopped)
        return;
    s->until = until;
    last_io = 0xFFFF;
    while ((int32_t)(s->cycles - until) < 0 && !s->stopped)
        step(s);
    timers(s);
}

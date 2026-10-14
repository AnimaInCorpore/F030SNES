/* High-level DSP-1 hot paths: see dsp1_hle.h. Mirrors tools/smkx/dsp1_hle.py
 * operation for operation, including every flag and the return stack. */
#include "dsp1_hle.h"

enum { OV0, OV1, Z, C, S0, S1 };
enum { OR = 1, AND, XOR, SUB, ADD, DEC = 8, INC, CMP };

/* The ALU (upd7725.UPD.alu) for the ops used here; p = operand, q = acc. */
static uint16_t alu(int op, uint16_t p, uint16_t q, uint8_t *f)
{
    uint32_t r;
    switch (op) {
    case OR:  r = q | p; break;
    case AND: r = q & p; break;
    case XOR: r = q ^ p; break;
    case SUB: r = (uint32_t)q - p; break;
    case ADD: r = (uint32_t)q + p; break;
    case DEC: r = (uint32_t)q - 1; p = 1; break;
    case INC: r = (uint32_t)q + 1; p = 1; break;
    default:  r = (uint16_t)~q; break;       /* CMP */
    }
    uint16_t v = (uint16_t)r;
    f[S0] = (v & 0x8000) ? 0xFF : 0;
    f[Z] = v == 0 ? 0xFF : 0;
    if (op == OR || op == AND || op == XOR || op == CMP) {
        f[C] = f[OV0] = f[OV1] = 0;
        return v;
    }
    int ov;
    if (op == ADD || op == INC) {
        ov = ((q ^ v) & (p ^ v) & 0x8000) != 0;
        f[C] = v < q ? 0xFF : 0;
    } else {
        ov = ((q ^ v) & (q ^ p) & 0x8000) != 0;
        f[C] = v > q ? 0xFF : 0;
    }
    f[OV0] = ov ? 0xFF : 0;
    if (ov) {
        f[S1] = (uint8_t)(((f[OV1] ? 1 : 0) ^ ((v & 0x8000) ? 0 : 1)) ? 0xFF : 0);
        f[OV1] ^= 0xFF;
    }
    return v;
}

#define ALU_A(op, p) (s->a = alu((op), (uint16_t)(p), s->a, s->fa))
#define ALU_B(op, p) (s->b = alu((op), (uint16_t)(p), s->b, s->fb))

static uint16_t mul_m(uint16_t k, uint16_t l)
{
    int32_t prod = (int32_t)(int16_t)k * (int16_t)l;
    return (uint16_t)(prod >> 15);
}

#define M() mul_m(s->k, s->l)
#define DP_INC() (s->dp = (uint16_t)((s->dp & 0xF0) | ((s->dp + 1) & 0x0F)))
#define DP_DEC() (s->dp = (uint16_t)((s->dp & 0xF0) | ((s->dp - 1) & 0x0F)))
#define KLM(v) (s->l = (v), s->k = ram[(s->dp | 0x40) & 0xFF])

static void call(UpdState *s, uint16_t ret)
{
    s->stack[3] = s->stack[2];
    s->stack[2] = s->stack[1];
    s->stack[1] = s->stack[0];
    s->stack[0] = ret;
}

static void ret(UpdState *s)
{
    s->stack[0] = s->stack[1];
    s->stack[1] = s->stack[2];
    s->stack[2] = s->stack[3];
}

/* $03A: A normalized, B -= shifts (B = 0 for A = 0); DP cleared. */
static void normalize(UpdState *s, uint16_t ret_to)
{
    call(s, ret_to);
    s->dp = 0;
    uint16_t idb = s->a;
    ALU_A(AND, idb);
    s->l = idb;
    if (s->fa[Z]) {
        ALU_B(XOR, s->b);
    } else {
        do {
            idb = s->a;
            ALU_A(ADD, idb);
            s->l = idb;
            DP_INC();
        } while (!s->fa[OV0]);
        s->a = s->l;
        DP_DEC();
        ALU_B(SUB, s->dp);
    }
    s->dp &= 0xF0;
    ret(s);
}

/* $0D3: A, B = inverse of the normalized coefficient, exponent 1 - B. */
static void inverse(UpdState *s, uint16_t *ram, const uint16_t *rom, uint16_t ret_to)
{
    uint16_t idb;
    call(s, ret_to);
    ALU_A(AND, s->a);
    s->dp &= 0xF0;
    if (s->fa[Z]) {
        s->a = 0x7FFF;
        s->b = 0x002F;
        s->dp &= 0xF0;
        ret(s);
        return;
    }
    if (s->fa[S0]) {
        ALU_A(CMP, ram[s->dp & 0xFF]);
        DP_INC();
        ALU_A(INC, 1);
        if (s->fa[OV0])
            ALU_A(DEC, 1);
    }
    s->rp = 0x64;
    idb = s->a;                                            /* 0DB */
    ALU_B(CMP, ram[s->dp & 0xFF]);
    s->k = idb;
    s->l = rom[s->rp];
    s->rp = (s->rp - 1) & 0x3FF;
    idb = rom[s->rp];                                      /* 0DC */
    ALU_B(INC, 1);
    s->a = idb;
    s->rp = (s->rp - 1) & 0x3FF;
    idb = rom[s->rp];                                      /* 0DD */
    ALU_A(ADD, M());
    s->tr = idb;
    idb = s->a;                                            /* 0DE */
    ALU_A(XOR, idb);
    s->rp = idb & 0x3FF;
    idb = rom[s->rp];                                      /* 0DF */
    ALU_B(INC, 1);
    s->l = idb;
    idb = s->k;                                            /* 0E0 */
    ALU_A(SUB, M());
    s->trb = idb;
    ALU_A(SUB, s->tr);                                     /* 0E1 */
    for (int pass = 0; pass < 2; pass++) {                 /* 0E2-0EE */
        idb = s->a;
        ALU_A(XOR, idb);
        s->k = idb;
        ALU_A(OR, M());
        if (pass == 0)
            s->k = s->trb;
        ALU_A(ADD, s->a);
        if (s->fa[OV0])
            s->a = 0x7FFF;
        if (pass == 0) {
            idb = s->a;
            ALU_A(XOR, idb);
            s->l = idb;
            ALU_A(SUB, M());
            ALU_A(SUB, s->tr);
        }
    }
    if (s->dp & 0x0F) {                                    /* 0F0-0F1 */
        ALU_A(CMP, ram[s->dp & 0xFF]);
        ALU_A(INC, 1);
    }
    s->dp &= 0xF0;
    ret(s);
}

/* $077: normalize, then scale by ROM[$31 + B - 15] or clip. */
static void denormalize(UpdState *s, const uint16_t *rom, uint16_t ret_to)
{
    uint16_t idb;
    call(s, ret_to);
    normalize(s, 0x078);
    s->rp = 0x0F;
    ALU_B(SUB, s->rp);
    if (s->fb[S0]) {
        s->rp = 0x31;
        ALU_B(ADD, s->rp);
        idb = s->b;
        ALU_B(XOR, idb);
        s->rp = idb & 0x3FF;
        idb = s->a;
        ALU_A(XOR, idb);
        s->k = idb;
        s->l = rom[s->rp];
        ALU_A(ADD, M());
    } else if (!s->fb[Z]) {
        ALU_A(AND, s->a);
        s->a = s->fa[S0] ? 0x8001 : 0x7FFF;
        ALU_B(XOR, s->b);
    } else {
        ALU_B(XOR, s->b);
    }
    ret(s);
}

unsigned dsp1_hle_raster(UpdState *s, uint16_t *ram, const uint16_t *rom)
{
    uint16_t idb;
    s->dp = 0x09;                                          /* 364 */
    s->b = 0x0001;                                         /* 365 */
    idb = ram[s->dp];                                      /* 366 */
    ALU_B(ADD, idb);
    KLM(idb);
    idb = s->b;                                            /* 367 */
    ALU_A(OR, M());
    ram[s->dp] = idb;
    DP_DEC();
    s->dp ^= 0xC0;
    ALU_A(ADD, ram[s->dp]);                                /* 368 */
    s->b = 0x0007;                                         /* 369 */
    normalize(s, 0x36B);                                   /* 36A */
    inverse(s, ram, rom, 0x36C);                           /* 36B */
    s->dp = 0x4C;                                          /* 36C */
    idb = s->a;                                            /* 36D */
    ALU_A(XOR, idb);
    KLM(idb);
    s->dp ^= 0x80;
    ALU_B(ADD, ram[s->dp]);                                /* 36E */
    idb = s->b;                                            /* 36F */
    ALU_A(OR, M());
    s->trb = idb;
    s->tr = s->a;                                          /* 370 */
    denormalize(s, rom, 0x372);                            /* 371 */
    s->dp = 0x3F;                                          /* 372 */
    ram[s->dp] = s->a;                                     /* 373 */
    s->dp ^= 0x10;
    ram[s->dp] = s->a;                                     /* 374 */
    s->dp = 0x4B;                                          /* 375 */
    s->a = s->tr;                                          /* 376 */
    idb = s->a;                                            /* 377 */
    ALU_A(XOR, idb);
    KLM(idb);
    s->dp ^= 0x80;
    idb = s->trb;                                          /* 378 */
    ALU_A(OR, M());
    s->b = idb;
    ALU_B(ADD, ram[s->dp]);                                /* 379 */
    denormalize(s, rom, 0x37B);                            /* 37A */
    s->dp = 0x3E;                                          /* 37B */
    ram[s->dp] = s->a;                                     /* 37C */
    s->dp ^= 0x10;
    idb = s->a;                                            /* 37D */
    ALU_A(XOR, idb);
    ram[s->dp] = idb;
    s->dp = 0x46;                                          /* 37E */
    s->dp = ram[s->dp] & 0xFF;                             /* 37F */
    if (s->dp & 0x0F)                                      /* 380 */
        return 0x392;
    s->dp = 0x3F;                                          /* 381 */
    idb = ram[s->dp];                                      /* 382 */
    KLM(idb);
    DP_DEC();
    idb = ram[s->dp];                                      /* 383 */
    ALU_A(OR, M());
    KLM(idb);
    DP_INC();
    s->dp ^= 0x10;
    return 0x384;
}

/* The SuperFX as the runtime sees it (falcon/smkx_rt.s): the 65816's
 * accesses to $3000-$34FF, and a synchronous run to STOP whenever a write
 * starts the GSU.
 *
 * With YIX_GSU_XLAT the run uses the translated program (tools/smkx/
 * gsu2c.py, build/yix/gsu_code.c): whenever the next opcode is a node it
 * translated, its bank's function takes over until STOP or until control
 * reaches something it did not translate; the interpreter (src/gsu.c) then
 * executes one instruction and the translated code is tried again. With
 * YIX_GSU_CHECK every run executes twice, translated and interpreted, from
 * the same state, and the first difference stops the program.
 * Freestanding. */
#include "gsu.h"

extern uint8_t smkx_rom[], rt_gsuram[];

Gsu yix_gsu;
uint32_t yix_gsu_runs;          /* GSU starts */
uint32_t yix_gsu_steps;         /* interpreted GSU instructions, all runs */
uint32_t yix_gsu_faults;        /* runs stopped by the step limit */
uint32_t yix_gsu_xsteps;        /* entries into translated code */
uint32_t yix_gsu_mismatch;      /* YIX_GSU_CHECK: first differing run (0 none) */
uint16_t yix_gsu_mismatch_pc;   /* its start address */
static int busy;

#define STEP_LIMIT 20000000u     /* interpreted instructions in one run: a hang */

#ifdef YIX_GSU_XLAT
int yix_gsu_enter(Gsu *g);          /* build/yix/gsu_code.c */

static void run_translated(Gsu *g)
{
    uint32_t interp = 0;
    gsu_step1(g);                       /* the NOP in the pipeline at a start */
    while (g->g) {
        g->r[15] = (uint16_t)(g->r[15] - 1);            /* the opcode in the pipeline */
        yix_gsu_xsteps++;
        if (!yix_gsu_enter(g))
            break;                                      /* STOP */
        /* not translated (3), unsupported (2), another bank (4): the
           interpreter takes the opcode at r15 */
        gsu_resume_at(g, g->r[15]);
        if (g->fault == 4)
            continue;
        gsu_step1(g);
        if (++interp > STEP_LIMIT) {
            g->fault = 1;
            break;
        }
    }
    g->last_steps = interp;
}
#endif

#ifdef YIX_GSU_CHECK
static Gsu shadow;
static uint8_t shadow_ram[GSU_RAM_BYTES];

static int same(const Gsu *a, const Gsu *b)
{
    unsigned i;
    for (i = 0; i < 16; i++)
        if (a->r[i] != b->r[i])
            return 0;
    if (a->z != b->z || a->s != b->s || a->cy != b->cy || a->ov != b->ov || a->g != b->g)
        return 0;
    if (a->rombr != b->rombr || a->rambr != b->rambr || a->colr != b->colr || a->por != b->por)
        return 0;
    for (i = 0; i < GSU_RAM_BYTES; i++)
        if (a->ram[i] != b->ram[i])
            return 0;
    return 1;
}
#endif

void yix_gsu_init(void)
{
    gsu_init(&yix_gsu, smkx_rom, rt_gsuram);
}

unsigned yix_gsu_read(unsigned addr)
{
    return gsu_read_reg(&yix_gsu, (uint16_t)addr);
}

void yix_gsu_write(unsigned addr, unsigned value)
{
    uint32_t before;
    if (!gsu_write_reg(&yix_gsu, (uint16_t)addr, (uint8_t)value) || busy)
        return;
    busy = 1;
    yix_gsu_runs++;
    before = yix_gsu.steps;
#if defined(YIX_GSU_XLAT) && defined(YIX_GSU_CHECK)
    {
        unsigned i;
        shadow = yix_gsu;
        shadow.ram = shadow_ram;
        for (i = 0; i < GSU_RAM_BYTES; i++)
            shadow_ram[i] = rt_gsuram[i];
        run_translated(&shadow);
        gsu_run(&yix_gsu, STEP_LIMIT);
        if (!yix_gsu_mismatch && !same(&shadow, &yix_gsu)) {
            yix_gsu_mismatch = yix_gsu_runs;
            yix_gsu_mismatch_pc = yix_gsu.start_pc;
        }
    }
#elif defined(YIX_GSU_XLAT)
    run_translated(&yix_gsu);
#else
    gsu_run(&yix_gsu, STEP_LIMIT);
#endif
    yix_gsu_steps += yix_gsu.steps - before;
    if (yix_gsu.fault == 1)
        yix_gsu_faults++;
    busy = 0;
}

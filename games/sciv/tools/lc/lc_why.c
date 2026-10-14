/* Which of the snapshots the list compositor's planner covers, and why not the others (src/lc_plan.c, lc_why).
 *
 *   lc_why rom.sfc SNAP.BIN...
 *
 * One line a snapshot, planned cold from its inputs alone: rc 0 covered, 1 forced blank only (skipped), -1 not covered
 * with the reasons as hex bits (1 no frame, 2 per-line changes, 8 mosaic, 10 forced blank, 20 mode, 40 16x16
 * tiles, 80 OAM segments, 100 ring layer with per-row scroll, 200 colour math, 400 mid-frame CGRAM). Windows are covered. */
#define main lc_model_main
#include "lc_model.c"
#undef main
#include "lc_plan.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s rom.sfc SNAP.BIN...\n", argv[0]);
        return 2;
    }
    long n;
    const uint8_t *rom = load(argv[1], &n);
    smw_rules = getenv("LC_SMW") != NULL;
    lc_refuse_math = getenv("LC_REFUSE_MATH") != NULL;
    ppu_init();
    static LcPlan plan;
    for (int a = 2; a < argc; a++) {
        Snap *s = snap_load(argv[a], rom);
        PpuInput in = snap_input(s, s->io);
        int covers = lc_covers(&in);
        lc_plan_reset();
        int rc = lc_plan(&in, &plan, NULL);
        if (covers != (rc == 0)) printf("MISMATCH: lc_covers %d, lc_plan %d: ", covers, rc);
        printf("%s rc %d why %03x mode %d tm %02x nlog %d segs %d diff %08x", s->name, rc, lc_why, s->io[0x105] & 7, s->io[0x12C],
               s->nlog, s->noamseg, lc_diff);
        for (int i = 0; i < s->noamseg; i++) printf(" @%d", s->oamseg[i].line);
        for (int i = 0; i < s->nlog && i < 4; i++) printf(" L%d:%04x=%02x", s->log[i].line, s->log[i].reg, s->log[i].value);
        printf("\n");
        free(s);
    }
    return 0;
}

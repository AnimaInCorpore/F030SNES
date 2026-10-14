/* Offline opportunity census, not a production culling algorithm. Snapshots
 * are sampled states, not assumed consecutive VBLs. Oracle counts compare
 * exact output indices and therefore include redundant dirtiness as well as
 * occlusion. No host timings are interpreted as Falcon throughput. */
#define main old_lc_model_main
#include "lc_model.c"
#undef main
#include "lc_plan.h"
#include "occlusion_ref.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: occlusion_probe rom.sfc SNAP.BIN...\n"); return 2; }
    long len;
    const uint8_t *rom = load(argv[1], &len);
    ppu_init();
    lc_refuse_math = 1;
    static OcclusionFrame frames[2];
    static LcPlan plan;
    unsigned long long planned = 0, oracle_same = 0, changed = 0;
    unsigned long long enabled[4] = {0}, hidden_main[4] = {0}, hidden_both[4] = {0};
    unsigned long long hidden_opaque[4] = {0}, dirty_zone[3] = {0}, redundant_zone[3] = {0};
    int prev = -1, covered = 0;
    puts("snapshot,covered,fixed,planned,unchanged_planned,changed_groups,blend_rows,BG1_hidden,BG2_hidden,BG3_hidden,BG4_hidden");
    for (int a = 2; a < argc; a++) {
        Snap *s = snap_load(argv[a], rom);
        PpuInput in = snap_input(s, s->io);
        int cur = prev < 0 ? 0 : prev ^ 1;
        occlusion_reference(&in, &frames[cur]);
        OcclusionFrame *f = &frames[cur];
        int rc = lc_plan(&in, &plan, NULL), same = 0, diff = 0, blends = 0;
        unsigned h[4] = {0};
        for (int y = 0; y < PPU_H; y++) {
            blends += f->blend[y];
            for (int g = 0; g < 16; g++) {
                unsigned m = 0, both = 0;
                for (int x = g * 16; x < g * 16 + 16; x++) { m |= f->main_used[y][x]; both |= f->both_used[y][x]; }
                for (int b = 0; b < 4; b++) if ((f->layers[y] >> b & 1) && (f->mode[y] == 0 || b < (f->mode[y] == 7 ? 1 : 3))) {
                    enabled[b]++;
                    hidden_main[b] += !(m >> b & 1);
                    hidden_both[b] += !(both >> b & 1);
                    h[b] += !(both >> b & 1);
                    hidden_opaque[b] += !(both >> b & 1) && f->opaque[b][y][g] != 0;
                }
                if (prev >= 0) {
                    int d = memcmp(f->pixels[y] + g * 16, frames[prev].pixels[y] + g * 16, 16) != 0;
                    diff += d;
                    if (!rc && plan.fixed && (plan.maskA[y] >> g & 1)) {
                        same += !d;
                        int zone = y < 31 ? 0 : y < 191 ? 1 : 2;
                        dirty_zone[zone]++;
                        redundant_zone[zone] += !d;
                    }
                }
            }
        }
        const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        printf("%s,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u\n", name, !rc, !rc && plan.fixed, !rc ? plan.ngroups : 0, same, diff, blends, h[0],h[1],h[2],h[3]);
        if (!rc && plan.fixed) { covered++; planned += plan.ngroups; oracle_same += same; changed += diff; }
        prev = cur;
        free(s);
    }
    printf("TOTAL fixed_frames=%d planned=%llu unchanged_planned=%llu actual_changed=%llu\n", covered, planned, oracle_same, changed);
    for (int b = 0; b < 4; b++) printf("BG%d enabled=%llu hidden_main=%llu hidden_both=%llu opaque_hidden=%llu\n", b+1, enabled[b], hidden_main[b], hidden_both[b], hidden_opaque[b]);
    for (int z = 0; z < 3; z++) printf("ZONE%d dirty=%llu unchanged=%llu\n", z, dirty_zone[z], redundant_zone[z]);
    return 0;
}

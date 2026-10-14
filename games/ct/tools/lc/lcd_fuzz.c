/* The planner's blank-row logic against the reference on mutated frames (a fuzz test for tools/lc/lcd_model.c's model).
 *
 *   lcd_fuzz rom.sfc SNAP.BIN [frames [seed]]
 *
 * Takes one covered snapshot and makes a sequence of frames out of it: the ring layer scrolls by a few pixels now and then, and a
 * forced blank (INIDISP = $80 written by an IRQ) starts at a random row, or not at all, frame by frame (CT's field frames blank
 * the bottom rows; the row, and whether there is one, changes here every frame, so ring groups come into and out of the blank rows).
 * Every frame goes through lc_plan and the model's execute(): the shown page must equal the reference picture. Prints the
 * totals; exit status 1 when a frame is wrong. */
#define LCD_NO_MAIN
#include "lcd_model.c"

static uint32_t rs = 1;
static uint32_t frnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 16; }

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s rom.sfc SNAP.BIN [frames [seed]]\n", argv[0]);
        return 2;
    }
    int nframes = argc > 3 ? atoi(argv[3]) : 200;
    rs = (argc > 4 ? (uint32_t)atoi(argv[4]) : 1) * 2654435761u + 12345u;   /* (the LCG's sequences are shifts of one another: spread the seeds) */
    long n;
    const uint8_t *rom = load(argv[1], &n);
    lc_refuse_math = 0;
    ppu_init();
    Snap *base = snap_load(argv[2], rom);
    static LFrame fr;
    int parity = 0, bad_frames = 0, bad_px = 0, covered = 0, slow = 0, blank_frames = 0;
    long comp = 0;
    int ring = base->io[0x12C] & 1 ? 0 : base->io[0x12C] & 2 ? 1 : 2;
    uint16_t sx = base->bgofs[ring * 2], sy = base->bgofs[ring * 2 + 1];
    for (int i = 0; i < nframes; i++) {
        Snap *s = snap_clone(base);
        /* scroll: mostly still, sometimes a few pixels, rarely a jump */
        int r = (int)(frnd() % 10);
        if (r < 3) sx = (uint16_t)((sx + (int)(frnd() % 7) - 3) & 0x3FF);
        else if (r < 5) sy = (uint16_t)((sy + (int)(frnd() % 5) - 2) & 0x3FF);
        else if (r == 9) { sx = (uint16_t)((sx + (int)(frnd() % 40) - 20) & 0x3FF); sy = (uint16_t)((sy + (int)(frnd() % 24) - 12) & 0x3FF); }
        s->bgofs[ring * 2] = sx;
        s->bgofs[ring * 2 + 1] = sy;
        /* the blank: none, or from a random row; the IRQ logs a write of INIDISP = $80 (and some VRAM address writes after it) */
        s->nlog = 0;
        s->noamseg = 0;
        int brow = -1;
        if (frnd() % 4) {
            brow = 100 + (int)(frnd() % 124);
            s->log[s->nlog++] = (PpuWrite){(uint16_t)(brow + 1), 0x2100, 0x80, 0};
            s->log[s->nlog++] = (PpuWrite){(uint16_t)(brow + 1), 0x2115, 0x80, 0};
            s->log[s->nlog++] = (PpuWrite){(uint16_t)(brow + 1), 0x2116, 0x00, 0};
            s->log[s->nlog++] = (PpuWrite){(uint16_t)(brow + 1), 0x2117, 0x3C, 0};
            if (frnd() % 2) {                      /* an OAM segment at the same line, as CT's IRQ leaves one */
                s->oamseg[0].line = (uint16_t)(brow + 1);
                memcpy(s->oamseg[0].oam, s->oam, PPU_OAM_BYTES);
                s->oamseg[0].oam[0] ^= 0x5A;
                s->noamseg = 1;
            }
            blank_frames++;
        }
        PpuInput in = snap_input(s, s->io);
        if (lc_plan(&in, &plan, NULL) != 0) {
            if (getenv("LCD_FUZZ_VERBOSE")) printf("frame %d: slow (why %02x) brow %d\n", i, lc_why, brow);
            slow++;
            free(s);
            continue;
        }
        dbg_cur = NULL;
        lcd_cgram = s->cgram;
        plan_inidisp = s->io[0x100];
        frame_renders(s, &fr);
        int bad;
        execute(&fr, parity, &bad);
        parity ^= 1;
        covered++;
        comp += plan.ngroups;
        if (bad) {
            bad_frames++;
            bad_px += bad;
            printf("frame %d WRONG: %d pixels (blank from row %d, scroll %d,%d, S %d dl %d)\n", i, bad, brow, sx, sy, plan.S, plan.dl);
        } else if (getenv("LCD_FUZZ_VERBOSE")) {
            printf("frame %d ok: comp %d copies %d blank from %d%s\n", i, plan.ngroups, plan.ncopies, brow, plan.cold ? " cold" : "");
        }
        free(s);
    }
    printf("%d frames: %d covered (%d with a blank, of %d), %d slow; mean %ld groups composed; %d frames wrong, %d wrong pixels\n", nframes, covered,
           blank_frames, nframes, slow, covered ? comp / covered : 0, bad_frames, bad_px);
    return bad_frames != 0;
}

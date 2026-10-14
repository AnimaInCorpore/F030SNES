/* Deterministic SCIV LoROM compositor regression: scrolling, windows,
 * changing tile/sprite data, blank rows and unsupported-frame transitions.
 * The retained ring must match the reference's CGRAM indices every frame. */
#define LCD_NO_MAIN
#include "lcd_model.c"
#include "../../src/ppu_dsp.h"
#include "../../src/palette.h"

static Snap scene;
static uint8_t rom[0x100000];

static void scene_init(void)
{
    scene.bus = (PpuBus){scene.wram, scene.sram, rom};
    scene.io[0x100] = 15;
    scene.io[0x105] = 1;
    scene.io[0x107] = 0x20;
    scene.io[0x108] = 0x28;
    scene.io[0x10B] = 0x10;
    scene.io[0x12C] = 0x13;
    for (int i = 0; i < 256; i++) {
        scene.cgram[i * 2] = (uint8_t)i;
        scene.cgram[i * 2 + 1] = (uint8_t)(i >> 3);
    }
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < 64 * 32; i++) scene.vram[b * 0x2000 + i] = (uint8_t)(i * 73 + (i >> 3) * 17);
        for (int i = 0; i < 1024; i++) {
            unsigned w = (unsigned)(i % 64) | (unsigned)((i >> 5) % 8) << 10 | (unsigned)(i & 3) << 14;
            scene.vram[0x8000 + b * 0x2000 + i * 2] = (uint8_t)w;
            scene.vram[0x8001 + b * 0x2000 + i * 2] = (uint8_t)(w >> 8);
        }
    }
    for (int i = 0; i < 128; i++) scene.oam[i * 4 + 1] = 240;
    scene.oam[1] = 90;
    scene.oam[3] = 0x30;
    /* Repeated WRAM HDMA window edges exercise replay skipping and ensure
     * the planner does not reuse ROM-only replay state for mutable RAM. */
    scene.io[0x2301] = 0x26;
    scene.io[0x2303] = 0x30;
    scene.io[0x2304] = 0x7E;
    scene.wram[0x3000] = 0x80;
    for (int i = 0; i < 128; i++) scene.wram[0x3001 + i] = 70;
    scene.wram[0x3081] = 0xE0;
    for (int i = 0; i < 96; i++) scene.wram[0x3082 + i] = 90;
}

/* Generate version-4 fixtures for the existing snapshot/DSP kernel tools.
 * These are synthetic inputs, never presented as captured gameplay. */
static void fixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite("SMKXSNAP", 1, 8, f);
#define B32(v) do { uint32_t z_ = (uint32_t)(v); for (int k_ = 3; k_ >= 0; k_--) fputc((int)(z_ >> (8 * k_)) & 255, f); } while (0)
#define B16(v) do { uint16_t z_ = (uint16_t)(v); fputc(z_ >> 8, f); fputc(z_ & 255, f); } while (0)
    B32(PPU_SNAP_VERSION);
    fwrite(scene.vram, 1, sizeof scene.vram, f);
    fwrite(scene.cgram, 1, sizeof scene.cgram, f);
    fwrite(scene.oam, 1, sizeof scene.oam, f);
    fwrite(scene.io, 1, sizeof scene.io, f);
    for (int i = 0; i < 8; i++) B16(scene.bgofs[i]);
    for (int i = 0; i < 6; i++) B16(scene.m7[i]);
    for (int i = 0; i < 2; i++) B16(scene.m7ofs[i]);
    B32(scene.nlog);
    for (int i = 0; i < 256; i++) { B16(scene.log[i].line); B16(scene.log[i].reg); fputc(scene.log[i].value, f); fputc(0, f); }
    fwrite(scene.wram, 1, sizeof scene.wram, f);
    fwrite(scene.sram, 1, sizeof scene.sram, f);
    B32(8);
    PpuInput in = snap_input(&scene, scene.io);
    static uint8_t pixels[PPU_W * PPU_H];
    static uint16_t planes[PPU_SNAP_PLANAR / 2];
    ppu_render(&in, pixels);
    ppu_planar8(pixels, planes);
    for (int i = 0; i < PPU_SNAP_PLANAR / 2; i++) B16(planes[i]);
    for (int i = 0; i < 2 * PAL_SNAP_BYTES; i++) fputc(0, f);
    B32(scene.noamseg);
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) { B16(scene.oamseg[i].line); B16(0); fwrite(scene.oamseg[i].oam, 1, PPU_OAM_BYTES, f); }
    fclose(f);
}

int main(int argc, char **argv)
{
    scene_init();
    ppu_init();
    lc_refuse_math = 1;
    static LFrame frame;
    int covered = 0, wrong = 0, parity = 0, windows = 0;
    for (int n = 0; n < 160; n++) {
        scene.bgofs[0] = (uint16_t)((n * 3 + 1000) & 1023);
        scene.bgofs[1] = (uint16_t)((n * 5 + 495) & 1023);
        scene.bgofs[2] = (uint16_t)((n + 200) & 1023);
        scene.bgofs[3] = (uint16_t)((n * 2) & 1023);
        scene.oam[0] = (uint8_t)(n * 7);
        scene.oam[1] = (uint8_t)(50 + n % 140);
        scene.vram[(n * 53) % 2048] ^= 0x5A;
        scene.io[0x12E] = n % 3 ? 3 : 0;
        scene.io[0x220C] = (uint8_t)(n % 2);
        scene.io[0x123] = 0x22;
        scene.io[0x126] = (uint8_t)(30 + n % 60);
        scene.io[0x127] = (uint8_t)(180 - n % 40);
        scene.nlog = n % 4 ? 1 : 0;
        scene.log[0] = (PpuWrite){(uint16_t)(160 + n % 60), 0x2100, 0x80, 0};
        PpuInput in = snap_input(&scene, scene.io);
        if (!lc_covers(&in) || lc_plan(&in, &plan, NULL)) { fprintf(stderr, "unexpected refusal frame %d why %x\n", n, lc_why); return 1; }
        dbg_cur = NULL;
        lcd_cgram = scene.cgram;
        plan_inidisp = scene.io[0x100];
        frame_renders(&scene, &frame);
        int bad = 0;
        execute(&frame, parity, &bad);
        parity ^= 1;
        wrong += bad;
        covered++;
        windows += scene.io[0x12E] != 0;
        if (argc > 1 && (n == 0 || n == 1)) {
            char path[512];
            snprintf(path, sizeof path, "%s/synthetic-%s.BIN", argv[1], n ? "window" : "tile");
            fixture(path);
        }
    }
    PpuInput in = snap_input(&scene, scene.io);
    scene.nlog = 1;
    in.nlog = 1;
    scene.log[0] = (PpuWrite){100, 0x2105, 7, 0};
    if (!lc_covers(&in) || lc_plan(&in, &plan, NULL) || !plan.fixed) { fprintf(stderr, "mixed mode was refused\n"); return 1; }
    for (int n = 0; n < 24; n++) {
        scene.nlog = in.nlog = 4;
        scene.log[0] = (PpuWrite){(uint16_t)(33 + n), 0x2105, 7, 0};
        scene.log[1] = (PpuWrite){(uint16_t)(33 + n), 0x212C, 0x11, 0};
        scene.log[2] = (PpuWrite){(uint16_t)(170 - n), 0x2105, 1, 0};
        scene.log[3] = (PpuWrite){(uint16_t)(170 - n), 0x212C, 0x13, 0};
        scene.m7[0] = scene.m7[3] = 256;
        scene.vram[n * 73] ^= 0x31;
        if (!lc_covers(&in) || lc_plan(&in, &plan, NULL) || !plan.fixed) return 1;
        frame_renders(&scene, &frame);
        int bad = 0;
        execute(&frame, parity, &bad);
        parity ^= 1;
        wrong += bad;
        covered++;
    }
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/synthetic-mixed.BIN", argv[1]);
        fixture(path);
    }
    int retained = 0;
    for (int n = 0; n < 48; n++) {
        scene.oam[0] = (uint8_t)(50 + n * 3);
        scene.oam[1] = (uint8_t)(80 + n % 10);
        if (n % 6 == 0) scene.vram[(n * 97) & 2047] ^= 0x5A;
        if (n % 11 == 0) scene.vram[0x8000 + (n * 2) % 2048] ^= 0x31;
        scene.log[0].line = scene.log[1].line = (uint16_t)(56 + n / 12);
        if (!lc_covers(&in) || lc_plan(&in, &plan, NULL) || !plan.fixed) return 1;
        retained += !plan.cold && plan.ngroups < PPU_H * 16;
        frame_renders(&scene, &frame);
        int bad = 0;
        execute(&frame, parity, &bad);
        parity ^= 1;
        wrong += bad;
        covered++;
    }
    if (retained < 40) { fprintf(stderr, "fixed screen did not retain groups\n"); return 1; }
    if (lc_plan(&in, &plan, NULL) || plan.ngroups) { fprintf(stderr, "stationary fixed frame redrawn\n"); return 1; }
    frame_renders(&scene, &frame);
    int quietbad = 0;
    execute(&frame, parity, &quietbad);
    parity ^= 1;
    wrong += quietbad;
    scene.nlog = in.nlog = 1;
    scene.log[0] = (PpuWrite){100, 0x2105, 3, 0};
    if (lc_covers(&in) || lc_plan(&in, &plan, NULL) >= 0) return 1;
    scene.nlog = in.nlog = 0;
    scene.io[0x220C] = 0;
    scene.io[0x130] = 2;
    scene.io[0x12D] = 1;
    scene.io[0x131] = 0x41;
    if (!lc_covers(&in) || lc_plan(&in, &plan, NULL) || !plan.fixed || !plan.color) return 1;
    frame_renders(&scene, &frame);
    int subbad = 0;
    execute(&frame, parity, &subbad);
    parity ^= 1;
    wrong += subbad;
    covered++;
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/synthetic-subhalf.BIN", argv[1]);
        fixture(path);
    }
    scene.io[0x130] = scene.io[0x12D] = 0;
    scene.io[0x131] = 1;
    if (lc_covers(&in) || lc_plan(&in, &plan, NULL) >= 0) { fprintf(stderr, "colour math was accepted\n"); return 1; }
    scene.io[0x131] = 0;
    scene.nlog = in.nlog = 1;
    scene.log[0] = (PpuWrite){100, 0x2131, 1, 0};
    if (lc_covers(&in) || lc_plan(&in, &plan, NULL) >= 0) { fprintf(stderr, "mid-frame colour math was accepted\n"); return 1; }
    scene.nlog = in.nlog = 2;
    scene.log[0] = (PpuWrite){100, 0x2122, 0xFF, 0};
    scene.log[1] = (PpuWrite){100, 0x2122, 0x7F, 0};
    if (lc_covers(&in) || lc_plan(&in, &plan, NULL) >= 0) { fprintf(stderr, "mid-frame CGRAM was accepted\n"); return 1; }
    scene.nlog = in.nlog = 0;
    if (lc_plan(&in, &plan, NULL) || !plan.cold) { fprintf(stderr, "fallback did not invalidate ring\n"); return 1; }
    /* A LoROM read above the SMW 512 KiB range must stay in SCIV's 1 MiB. */
    rom[0x88000] = 0xA7;
    uint32_t len;
    const uint8_t *p = ppu_aptr_lorom(0x118000, &scene.bus, &len);
    if (ppu_abus_lorom(0x118000, &scene.bus) != 0xA7 || !p || *p != 0xA7) return 1;
    printf("SCIV LC: %d frames (%d windows), %d wrong pixels; mode/layer splits, unsupported-mode/math fallbacks and LoROM mapping passed\n", covered, windows, wrong);
    return wrong != 0;
}

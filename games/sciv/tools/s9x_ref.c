/* Reference pictures from snes9x (its libretro core, run headless): the
 * attract demo without input (work RAM cleared at power-on, as the port
 * has it), frame by frame; for each snapshot of the
 * port (SNAP.BIN files) the snes9x frame whose work RAM matches the
 * snapshot's best, and that frame's picture and its neighbours'.
 *
 *   build/s9x_ref core.dylib rom.sfc outdir [-range B A] SNAP.BIN...
 *   build/s9x_ref core.dylib rom.sfc outdir -every K N
 *
 * The second form writes every K-th frame up to N as outdir/sNNNNNN.ppm
 * (the game's frames, for finding a scene).
 *   build/s9x_ref core.dylib rom.sfc outdir -wram F...
 * writes the work RAM after each game frame F as outdir/wNNNNNN.bin.
 *
 * A snapshot named Fxxxxxxxx.BIN (the port's frame count xxxxxxxx) is
 * looked for among the game frames xxxxxxxx / 4 - 200 to xxxxxxxx + 100:
 * the port counts the frames it lost too, so its count runs ahead of the
 * game's (twice as fast in the field engine). Each snes9x frame is first
 * compared on every 32nd byte, then in full where that is the closest yet.
 * A second run looks at the frames from B (8) before the match to A (2)
 * after it for the one closest to the snapshot's picture as our reference
 * draws it (outdir/Fxxxxxxxx_ref.ppm, from build/ppu_rgb), and writes that
 * one: a snapshot's registers are its frame start's, its work RAM (and the
 * HDMA tables in it) is copied a few frames later in snapshot builds, and
 * the port's NMI-driven effects run ahead of or behind its main loop.
 * Colours within 8 of 255 per channel count as equal.
 * Per snapshot: outdir/Fxxxxxxxx_s9x.ppm (256 x 224) and one line on
 * stdout: the snapshot, the work RAM match, its bytes that differ, the
 * closest frame's offset from it and its pixels that differ from the
 * reference. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

static uint8_t *load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*size);
    if (!d || fread(d, 1, (size_t)*size, f) != (size_t)*size) { perror(path); exit(2); }
    fclose(f);
    return d;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* the last frame snes9x drew, as 0xRRGGBB */
static uint32_t frame[512 * 478];
static unsigned fw, fh;
static enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_0RGB1555;

static bool env(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        fmt = *(const enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = ".";
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    default:
        return false;
    }
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) return;                          /* a duplicate of the last frame */
    fw = w;
    fh = h;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            uint32_t c;
            if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                c = ((const uint32_t *)((const uint8_t *)data + y * pitch))[x] & 0xFFFFFF;
            } else if (fmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t v = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
                c = (uint32_t)((v >> 11) * 255 / 31) << 16 | (uint32_t)(((v >> 5) & 63) * 255 / 63) << 8 |
                    (uint32_t)((v & 31) * 255 / 31);
            } else {
                uint16_t v = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
                c = (uint32_t)(((v >> 10) & 31) * 255 / 31) << 16 | (uint32_t)(((v >> 5) & 31) * 255 / 31) << 8 |
                    (uint32_t)((v & 31) * 255 / 31);
            }
            frame[y * w + x] = c;
        }
}

static void audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t n) { (void)d; return n; }
/* Scripted joypad 1 (S9X_INPUT="frame:pad,frame:pad,...", pad as the port's
 * SCIVX_INPUT: $4219 << 8 | $4218; frames are the NMIs the game has had since it enabled
 * them (WRAM $1E82 bit 7; the port's rt_nmis). */
static long gframe;
static const uint8_t *g_wram;
static long nmik;                       /* frames run with the game's NMI enabled (WRAM $1E82 bit 7)*/           /* the core's work RAM, once it is up */
static unsigned long pad_at[256][2];
static int npad;
static void input_poll(void) {}
static void input_parse(void)
{
    const char *e = getenv("S9X_INPUT");
    while (e && *e && npad < 256) {
        char *end;
        long fr = strtol(e, &end, 10);
        if (*end != ':') break;
        unsigned long pad = strtoul(end + 1, &end, 16);
        pad_at[npad][0] = (unsigned long)fr; pad_at[npad][1] = pad; npad++;
        e = (*end == ',') ? end + 1 : end;
    }
}
static int16_t input_state(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    static const int bit[12] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4};
    if (port != 0 || dev != 1 || idx != 0 || id > 11) return 0;
    unsigned long pad = 0;
    unsigned long t = (unsigned long)nmik;                    /* frames since the game enabled its NMI */
    for (int i = 0; i < npad; i++) if (t >= pad_at[i][0]) pad = pad_at[i][1];
    return (pad >> bit[id]) & 1;
}

/* The last frame as 256 x 224: a hires frame halved, overscan lines left out. */
static void grab(uint32_t *out)
{
    unsigned sx = fw >= 512 ? 2 : 1, sy = fh >= 448 ? 2 : 1;
    for (unsigned y = 0; y < 224; y++)
        for (unsigned x = 0; x < 256; x++)
            out[y * 256 + x] = (y * sy < fh && x * sx < fw) ? frame[(y * sy) * fw + x * sx] : 0;
}

static uint32_t *read_ppm(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int w, h, mx;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w != 256 || h != 224) { fclose(f); return NULL; }
    fgetc(f);
    uint32_t *p = malloc(256 * 224 * 4);
    for (int i = 0; i < 256 * 224; i++) {
        int r = fgetc(f), g = fgetc(f), b = fgetc(f);
        p[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
    fclose(f);
    return p;
}

static long pixdiff(const uint32_t *a, const uint32_t *b)
{
    long d = 0;
    for (int i = 0; i < 256 * 224; i++)
        for (int k = 0; k < 24; k += 8) {
            int x = (int)((a[i] >> k) & 255) - (int)((b[i] >> k) & 255);
            if (x > 8 || x < -8) { d++; break; }
        }
    return d;
}

static void write_ppm(const char *path, const uint32_t *pix)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "P6\n256 224\n255\n");
    for (int i = 0; i < 256 * 224; i++) {
        fputc((int)(pix[i] >> 16) & 255, f);
        fputc((int)(pix[i] >> 8) & 255, f);
        fputc((int)pix[i] & 255, f);
    }
    fclose(f);
}

typedef struct {
    const char *name;
    long frame;                                 /* from the file name */
    uint8_t *wram;
    long best, bestdiff, bestsample;
    uint32_t *ref, *pic;                        /* our reference; the closest snes9x frame */
    long picdiff, picoff;

} Target;

#define SYM(name) do { *(void **)&p_##name = dlsym(h, #name); \
    if (!p_##name) { fprintf(stderr, "%s: no %s\n", argv[1], #name); return 2; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s core.dylib rom.sfc outdir [-window N] SNAP.BIN...\n", argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    void (*p_retro_set_environment)(retro_environment_t);
    void (*p_retro_set_video_refresh)(retro_video_refresh_t);
    void (*p_retro_set_audio_sample)(retro_audio_sample_t);
    void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*p_retro_set_input_poll)(retro_input_poll_t);
    void (*p_retro_set_input_state)(retro_input_state_t);
    void (*p_retro_init)(void);
    bool (*p_retro_load_game)(const struct retro_game_info *);
    void (*p_retro_run)(void);
    void (*p_retro_reset)(void);
    void *(*p_retro_get_memory_data)(unsigned);
    size_t (*p_retro_get_memory_size)(unsigned);
    SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
    SYM(retro_init); SYM(retro_load_game); SYM(retro_run); SYM(retro_reset); SYM(retro_get_memory_data);
    SYM(retro_get_memory_size);
    p_retro_set_environment(env);
    p_retro_init();
    p_retro_set_video_refresh(video);
    p_retro_set_audio_sample(audio);
    p_retro_set_audio_sample_batch(audio_batch);
    p_retro_set_input_poll(input_poll);
    p_retro_set_input_state(input_state);
    long romsize;
    uint8_t *rom = load(argv[2], &romsize);
    struct retro_game_info gi = {argv[2], rom, (size_t)romsize, NULL};
    if (!p_retro_load_game(&gi)) { fprintf(stderr, "%s: not loaded\n", argv[2]); return 2; }
    const char *outdir = argv[3];
    if (argc >= 6 && !strcmp(argv[4], "-wram")) {
        /* -wram K...: the work RAM after the frame at which the game has had K NMIs
         * (with S9X_INPUT scripted, keyed the same way) */
        uint8_t *w = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        g_wram = w;
        memset(w, 0, 131072);
        input_parse();
        long last = 0;
        for (int i = 5; i < argc; i++) if (atol(argv[i]) > last) last = atol(argv[i]);
        for (long f = 1; nmik <= last && f < 1000000; f++) {
            gframe = f;
            p_retro_run();
            if (w[0x1E82] & 0x80) nmik++;
            if (getenv("S9X_BGMODE") && nmik >= atol(getenv("S9X_BGMODE")) && nmik < atol(getenv("S9X_BGMODE")) + 8) {
                void *ppu = dlsym(h, "PPU");                       /* snes9x's SPPU: BGMode at +64 */
                if (ppu) fprintf(stderr, "K=%ld BGMode=%d\n", nmik, ((uint8_t *)ppu)[64]);
            }
            for (int i = 5; i < argc; i++)
                if (atol(argv[i]) == nmik && (w[0x1E82] & 0x80)) {
                    char path[1024];
                    snprintf(path, sizeof path, "%s/w%06ld.bin", outdir, nmik);
                    FILE *o = fopen(path, "wb");
                    fwrite(w, 1, 131072, o);
                    fclose(o);
                    {
                        const uint8_t *vr = p_retro_get_memory_data(3);       /* RETRO_MEMORY_VIDEO_RAM */
                        if (vr) {
                            snprintf(path, sizeof path, "%s/v%06ld.bin", outdir, nmik);
                            FILE *ov = fopen(path, "wb");
                            fwrite(vr, 1, 65536, ov);
                            fclose(ov);
                        }
                    }
                    static uint32_t pic[256 * 224];
                    snprintf(path, sizeof path, "%s/p%06ld.ppm", outdir, nmik);
                    grab(pic);
                    write_ppm(path, pic);
                }
        }
        return 0;
    }
    if (argc == 7 && !strcmp(argv[4], "-every")) {
        long k = atol(argv[5]), n = atol(argv[6]);
        uint8_t *w = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        g_wram = w;
        if (w) memset(w, 0, 131072);
        static uint32_t pic[256 * 224];
        input_parse();
        for (long f = 1; f <= n; f++) {
            gframe = f;
            p_retro_run();
            if (g_wram && (g_wram[0x1E82] & 0x80)) nmik++;
            if (f % k) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/s%06ld.ppm", outdir, f);
            grab(pic);
            write_ppm(path, pic);
        }
        return 0;
    }
    int a = 4;
    long before = 8, after = 2;
    if (a + 2 < argc && !strcmp(argv[a], "-range")) { before = atol(argv[a + 1]); after = atol(argv[a + 2]); a += 3; }
    int nt = argc - a;
    Target *t = calloc((size_t)nt, sizeof *t);
    long last = 0;
    for (int i = 0; i < nt; i++) {
        long n;
        uint8_t *snap = load(argv[a + i], &n);
        if (n < 12 || memcmp(snap, "SMKXSNAP", 8)) { fprintf(stderr, "%s: not a snapshot\n", argv[a + i]); return 2; }
        (void)be32;
        /* the work RAM: after VRAM, CGRAM, OAM, io, the latched values and the IRQ log */
        size_t off = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 + 256 * 6;
        t[i].wram = malloc(131072);
        memcpy(t[i].wram, snap + off, 131072);
        free(snap);
        const char *base = strrchr(argv[a + i], '/');
        base = base ? base + 1 : argv[a + i];
        t[i].name = base;
        t[i].frame = base[0] == 'F' ? strtol(base + 1, NULL, 16) : 0;
        t[i].best = -1;
        t[i].bestdiff = t[i].bestsample = 1L << 30;
        {
            char path[1024];
            snprintf(path, sizeof path, "%s/%.*s_ref.ppm", outdir, (int)strcspn(base, "."), base);
            t[i].ref = read_ppm(path);
            t[i].pic = malloc(256 * 224 * 4);
            t[i].picdiff = 1L << 30;
        }
        if (t[i].frame + 100 > last) last = t[i].frame + 100;
    }
    uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    if (!wram || p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) < 131072) {
        fprintf(stderr, "no work RAM from the core\n");
        return 2;
    }
    memset(wram, 0, 131072);                    /* as the port's: snes9x starts it at $55 */
    static uint32_t cur[256 * 224];
    for (long f = 1; f <= last; f++) {
        p_retro_run();
        for (int i = 0; i < nt; i++) {
            Target *x = &t[i];
            if (f < x->frame / 4 - 200 || f > x->frame + 100) continue;
            long ds = 0;
            for (int k = 0; k < 131072; k += 32) ds += wram[k] != x->wram[k];
            if (ds > x->bestsample) continue;
            x->bestsample = ds;
            long d = 0;
            for (int k = 0; k < 131072; k++) d += wram[k] != x->wram[k];
            if (d < x->bestdiff) {
                x->bestdiff = d;
                x->best = f;
            }
        }
    }
    /* the second run: the frame around each match closest to the reference */
    p_retro_reset();
    memset(wram, 0, 131072);
    long lastbest = 0;
    for (int i = 0; i < nt; i++) if (t[i].best + after > lastbest) lastbest = t[i].best + after;
    for (long f = 1; f <= lastbest; f++) {
        p_retro_run();
        int grabbed = 0;
        for (int i = 0; i < nt; i++) {
            long k = f - t[i].best;
            if (t[i].best < 0 || !t[i].ref || k < -before || k > after) continue;
            if (!grabbed) { grab(cur); grabbed = 1; }
            long d = pixdiff(cur, t[i].ref);
            if (d < t[i].picdiff) {
                t[i].picdiff = d;
                t[i].picoff = k;
                memcpy(t[i].pic, cur, sizeof cur);
            }
        }
    }
    for (int i = 0; i < nt; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%.*s_s9x.ppm", outdir, (int)strcspn(t[i].name, "."), t[i].name);
        if (t[i].ref) write_ppm(path, t[i].pic);
        printf("%s frame %ld: work RAM differs in %ld of 131072 bytes; closest frame %+ld: %ld pixels differ\n",
               t[i].name, t[i].best, t[i].bestdiff, t[i].picoff, t[i].ref ? t[i].picdiff : -1);
    }
    return 0;
}

#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>

static int write_binary(const char *path, const void *data, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    int ok = fwrite(data, 1, bytes, f) == bytes;
    if (fclose(f)) ok = 0;
    if (!ok) perror(path);
    return ok;
}

static int write_asset_pack(const char *path, const Assets *assets)
{
    enum { MAP_BYTES = MAP_TILES * MAP_TILES,
           TILES_BYTES = TILE_COUNT * TILE_BYTES,
           KARTS_BYTES = 2 * SPRITE_BYTES };
    const unsigned map_words = (MAP_BYTES + 2) / 3;
    const unsigned tiles_words = (TILES_BYTES + 2) / 3;
    const unsigned karts_words = (KARTS_BYTES + 2) / 3;
    const uint8_t header[16] = {
        'F', '3', 'M', 'C', 0, 1, 0, 3,
        (uint8_t)(map_words >> 8), (uint8_t)map_words,
        (uint8_t)(tiles_words >> 8), (uint8_t)tiles_words,
        (uint8_t)(karts_words >> 8), (uint8_t)karts_words,
        0, 0
    };
    const uint8_t padding[2] = {0, 0};
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    int ok = fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
             fwrite(assets->map, 1, MAP_BYTES, f) == MAP_BYTES &&
             fwrite(padding, 1, map_words * 3 - MAP_BYTES, f) ==
                 map_words * 3 - MAP_BYTES &&
             fwrite(assets->tiles, 1, TILES_BYTES, f) == TILES_BYTES &&
             fwrite(padding, 1, tiles_words * 3 - TILES_BYTES, f) ==
                 tiles_words * 3 - TILES_BYTES &&
             fwrite(assets->kart, 1, KARTS_BYTES, f) == KARTS_BYTES &&
             fwrite(padding, 1, karts_words * 3 - KARTS_BYTES, f) ==
                 karts_words * 3 - KARTS_BYTES;
    if (fclose(f)) ok = 0;
    if (!ok) perror(path);
    return ok;
}

static int write_ppm(const char *path, const Assets *assets,
                     const uint8_t planar[FRAME_PLANAR_BYTES])
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    if (fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H) < 0) {
        fclose(f); return 0;
    }
    uint8_t line[SCREEN_W];
    for (int y = 0; y < SCREEN_H; ++y) {
        p2c_line(planar + y * SCREEN_W / 2, line);
        for (int x = 0; x < SCREEN_W; ++x) {
            if (fwrite(assets->palette[line[x]], 1, 3, f) != 3) {
                fclose(f); return 0;
            }
        }
    }
    return fclose(f) == 0;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s frame.ppm scene_4bpp.bin assets.f3mc frame_4bpp.bin\n", argv[0]);
        return 2;
    }
    Assets *assets = malloc(sizeof(*assets));
    uint8_t *frame = malloc(FRAME_PLANAR_BYTES);
    if (!assets || !frame) {
        fprintf(stderr, "allocation failed\n");
        free(frame); free(assets); return 1;
    }
    assets_generate(assets);
    Camera camera = {512.0, 784.0, 1.5707963267948966};
    const Billboard sprites[] = {
        {162, 62, 18, 18, 1},
        {108, 70, 40, 40, 0}
    };
    render_frame(assets, &camera, sprites, 2, frame);
    int ok = write_ppm(argv[1], assets, frame) &&
             write_binary(argv[2], frame, SCENE_PLANAR_BYTES) &&
             write_asset_pack(argv[3], assets) &&
             write_binary(argv[4], frame, FRAME_PLANAR_BYTES);
    if (ok) {
        printf("wrote %s (%dx%d preview)\n", argv[1], SCREEN_W, SCREEN_H);
        printf("wrote %s (%d-byte, 256x112 4-plane scene)\n",
               argv[2], SCENE_PLANAR_BYTES);
        printf("wrote %s (24-bit DSP word-aligned map, tiles and sprites)\n",
               argv[3]);
        printf("wrote %s (%d-byte, 256x224 4-plane Videl image)\n",
               argv[4], FRAME_PLANAR_BYTES);
        printf("scene transfer floor: %.2f ms at 1 MB/s specification; "
               "%.2f ms at 245 KiB/s Hatari result\n",
               1000.0 * SCENE_PLANAR_BYTES / 1000000.0,
               1000.0 * SCENE_PLANAR_BYTES / (245.0 * 1024.0));
    }
    free(frame);
    free(assets);
    return ok ? 0 : 1;
}

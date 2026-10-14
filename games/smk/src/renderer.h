#ifndef F030_MARIOCART_RENDERER_H
#define F030_MARIOCART_RENDERER_H

#include <stddef.h>
#include <stdint.h>

enum {
    SCREEN_W = 256,
    SCREEN_H = 224,
    SCENE_H = 112,
    HORIZON_Y = 28,
    MAP_TILES = 128,
    TILE_SIZE = 8,
    TILE_COUNT = 256,
    SPRITE_SIZE = 32,
    PLANES = 4,
    TILE_BYTES = TILE_SIZE * TILE_SIZE / 2,
    SPRITE_BYTES = SPRITE_SIZE * SPRITE_SIZE / 2,
    SCENE_PLANAR_BYTES = SCREEN_W * SCENE_H / 2,
    FRAME_PLANAR_BYTES = SCREEN_W * SCREEN_H / 2
};

typedef struct {
    uint8_t map[MAP_TILES * MAP_TILES];
    uint8_t tiles[TILE_COUNT][TILE_BYTES];
    uint8_t kart[2][SPRITE_BYTES];
    uint8_t palette[16][3];
} Assets;

typedef struct {
    double world_x;
    double world_y;
    double angle;
} Camera;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    unsigned art;
} Billboard;

/* A packed 4 bpp byte stores its left pixel in bits 7..4. */
void packed_set(uint8_t *data, size_t pixel, uint8_t colour);
uint8_t packed_get(const uint8_t *data, size_t pixel);

void assets_generate(Assets *assets);
uint8_t track_sample(const Assets *assets, int world_x, int world_y);
void scene_line(const Assets *assets, const Camera *camera, int y,
                const Billboard *sprites, size_t sprite_count,
                uint8_t line[SCREEN_W]);
void c2p_line(const uint8_t line[SCREEN_W], uint8_t planar[SCREEN_W / 2]);
void p2c_line(const uint8_t planar[SCREEN_W / 2], uint8_t line[SCREEN_W]);
void render_scene(const Assets *assets, const Camera *camera,
                  const Billboard *sprites, size_t sprite_count,
                  uint8_t planar[SCENE_PLANAR_BYTES]);
void render_map_line(const Assets *assets, int y, uint8_t line[SCREEN_W]);
void render_frame(const Assets *assets, const Camera *camera,
                  const Billboard *sprites, size_t sprite_count,
                  uint8_t planar[FRAME_PLANAR_BYTES]);

#endif

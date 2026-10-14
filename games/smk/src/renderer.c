#include "renderer.h"

#include <math.h>
#include <string.h>

void packed_set(uint8_t *data, size_t pixel, uint8_t colour)
{
    size_t byte = pixel >> 1;
    if ((pixel & 1u) == 0)
        data[byte] = (uint8_t)((data[byte] & 0x0f) | ((colour & 15u) << 4));
    else
        data[byte] = (uint8_t)((data[byte] & 0xf0) | (colour & 15u));
}

uint8_t packed_get(const uint8_t *data, size_t pixel)
{
    uint8_t byte = data[pixel >> 1];
    return (uint8_t)((pixel & 1u) ? (byte & 15u) : (byte >> 4));
}

static void make_tile(uint8_t *tile, unsigned id)
{
    for (unsigned y = 0; y < TILE_SIZE; ++y) {
        for (unsigned x = 0; x < TILE_SIZE; ++x) {
            unsigned hash = (x * 17u + y * 31u + x * y * 7u + id * 11u) & 31u;
            uint8_t colour;
            switch (id) {
            case 0: colour = hash < 5 ? 3 : 2; break;              /* grass */
            case 1: colour = hash < 8 ? 4 : 3; break;              /* grass */
            case 2: colour = hash < 3 ? 9 : 8; break;              /* road */
            case 3: colour = x == 3 || x == 4 ? 13 : 8; break;     /* road mark */
            case 4: colour = ((x + y) & 4u) ? 12 : 15; break;      /* curb */
            case 5: colour = hash < 7 ? 6 : 5; break;              /* dirt */
            case 6: colour = ((x >> 2) ^ (y >> 2)) & 1u ? 15 : 1; break;
            default: colour = (uint8_t)(id & 15u); break;
            }
            packed_set(tile, y * TILE_SIZE + x, colour);
        }
    }
}

static void make_kart(uint8_t *kart, uint8_t body)
{
    for (int y = 0; y < SPRITE_SIZE; ++y) {
        for (int x = 0; x < SPRITE_SIZE; ++x) {
            uint8_t colour = 0;
            int dx = x - 16;
            int dy = y - 13;
            if (((x >= 3 && x <= 9) || (x >= 23 && x <= 29)) &&
                y >= 19 && y <= 29) colour = 1;                    /* wheels */
            if (((dx * dx) / 110 + ((y - 23) * (y - 23)) / 46) <= 1 &&
                y >= 16 && y <= 30) colour = body;                 /* chassis */
            if (x >= 9 && x <= 22 && y >= 24 && y <= 27) colour = 15;
            if (dx * dx + dy * dy <= 49 && y <= 19) colour = 1;    /* helmet rim */
            if (dx * dx + dy * dy <= 36 && y <= 18) colour = 14;   /* helmet */
            if (x >= 12 && x <= 20 && y >= 14 && y <= 17) colour = 15;
            if (x >= 14 && x <= 18 && y >= 9 && y <= 12) colour = body;
            if (x >= 6 && x <= 10 && y == 28) colour = 14;
            if (x >= 22 && x <= 26 && y == 28) colour = 14;
            packed_set(kart, (size_t)y * SPRITE_SIZE + (size_t)x, colour);
        }
    }
}

void assets_generate(Assets *assets)
{
    static const uint8_t palette[16][3] = {
        {0, 0, 0},       {20, 22, 38},    {23, 100, 42},   {38, 140, 56},
        {75, 174, 69},   {123, 94, 48},   {166, 130, 66},  {94, 177, 222},
        {87, 91, 105},   {111, 116, 129}, {45, 96, 210},  {220, 57, 49},
        {236, 70, 54},  {203, 211, 218}, {249, 205, 65}, {255, 250, 226}
    };
    memset(assets, 0, sizeof(*assets));
    memcpy(assets->palette, palette, sizeof(palette));
    for (unsigned i = 0; i < TILE_COUNT; ++i)
        make_tile(assets->tiles[i], i);
    make_kart(assets->kart[0], 11);
    make_kart(assets->kart[1], 10);

    for (int ty = 0; ty < MAP_TILES; ++ty) {
        for (int tx = 0; tx < MAP_TILES; ++tx) {
            double dx = ((double)tx + 0.5 - 64.0) / 46.0;
            double dy = ((double)ty + 0.5 - 64.0) / 39.0;
            double radius = sqrt(dx * dx + dy * dy);
            uint8_t id;
            if ((radius > 0.70 && radius < 0.73) ||
                (radius > 1.02 && radius < 1.05)) id = 4;
            else if (radius >= 0.73 && radius <= 1.02)
                id = (ty == 98 && tx >= 59 && tx <= 61) ? 6 :
                     ((tx + ty) % 17 == 0 ? 3 : 2);
            else if (radius >= 1.05 && radius < 1.12) id = 5;
            else id = (tx + ty) & 1 ? 0 : 1;
            assets->map[ty * MAP_TILES + tx] = id;
        }
    }
}

uint8_t track_sample(const Assets *assets, int world_x, int world_y)
{
    /* 1024-pixel toroidal map. Unsigned masking defines negative wrap too. */
    unsigned x = (uint32_t)world_x & 1023u;
    unsigned y = (uint32_t)world_y & 1023u;
    unsigned tile_id = assets->map[(y >> 3) * MAP_TILES + (x >> 3)];
    return packed_get(assets->tiles[tile_id], (y & 7u) * 8u + (x & 7u));
}

static void overlay_sprite(const Assets *assets, const Billboard *sprite,
                           int y, uint8_t line[SCREEN_W])
{
    if (sprite->width <= 0 || sprite->height <= 0 || sprite->art >= 2 ||
        y < sprite->y || y >= sprite->y + sprite->height) return;
    int sy = (y - sprite->y) * SPRITE_SIZE / sprite->height;
    int left = sprite->x < 0 ? 0 : sprite->x;
    int right = sprite->x + sprite->width > SCREEN_W ? SCREEN_W :
                sprite->x + sprite->width;
    for (int x = left; x < right; ++x) {
        int sx = (x - sprite->x) * SPRITE_SIZE / sprite->width;
        uint8_t colour = packed_get(assets->kart[sprite->art],
                                     (size_t)sy * SPRITE_SIZE + (size_t)sx);
        if (colour) line[x] = colour;
    }
}

void scene_line(const Assets *assets, const Camera *camera, int y,
                const Billboard *sprites, size_t sprite_count,
                uint8_t line[SCREEN_W])
{
    if (y < HORIZON_Y) {
        uint8_t sky = y < 13 ? 7 : 10;
        memset(line, sky, SCREEN_W);
        for (int x = 0; x < SCREEN_W; ++x) {
            int ridge = 25 - ((x * 13 + 37) % 53) / 12;
            if (y >= ridge) line[x] = 3;
        }
    } else {
        /* Perspective division happens once per line. The inner loop is two
           fixed-point additions and one tile/texel lookup per pixel. */
        double depth = 2280.0 / (double)(y - HORIZON_Y + 1);
        double forward_x = sin(camera->angle);
        double forward_y = -cos(camera->angle);
        double right_x = cos(camera->angle);
        double right_y = sin(camera->angle);
        double step_x = right_x * depth / 128.0;
        double step_y = right_y * depth / 128.0;
        int64_t u = (int64_t)llround((camera->world_x + forward_x * depth -
                         128.0 * step_x) * 65536.0);
        int64_t v = (int64_t)llround((camera->world_y + forward_y * depth -
                         128.0 * step_y) * 65536.0);
        int64_t du = (int64_t)llround(step_x * 65536.0);
        int64_t dv = (int64_t)llround(step_y * 65536.0);
        for (int x = 0; x < SCREEN_W; ++x) {
            line[x] = track_sample(assets, (int)(u >> 16), (int)(v >> 16));
            u += du;
            v += dv;
        }
    }
    /* Caller supplies back-to-front order. Transparent index 0 keeps floor. */
    for (size_t i = 0; i < sprite_count; ++i)
        overlay_sprite(assets, &sprites[i], y, line);
}

void c2p_line(const uint8_t line[SCREEN_W], uint8_t planar[SCREEN_W / 2])
{
    for (unsigned block = 0; block < SCREEN_W / 16; ++block) {
        for (unsigned plane = 0; plane < PLANES; ++plane) {
            uint16_t word = 0;
            for (unsigned bit = 0; bit < 16; ++bit)
                word |= (uint16_t)(((line[block * 16 + bit] >> plane) & 1u)
                                    << (15u - bit));
            unsigned offset = block * 8 + plane * 2;
            planar[offset] = (uint8_t)(word >> 8);
            planar[offset + 1] = (uint8_t)word;
        }
    }
}

void p2c_line(const uint8_t planar[SCREEN_W / 2], uint8_t line[SCREEN_W])
{
    for (unsigned block = 0; block < SCREEN_W / 16; ++block) {
        uint16_t words[PLANES];
        for (unsigned plane = 0; plane < PLANES; ++plane) {
            unsigned offset = block * 8 + plane * 2;
            words[plane] = (uint16_t)((planar[offset] << 8) | planar[offset + 1]);
        }
        for (unsigned bit = 0; bit < 16; ++bit) {
            uint8_t colour = 0;
            for (unsigned plane = 0; plane < PLANES; ++plane)
                colour |= (uint8_t)(((words[plane] >> (15u - bit)) & 1u) << plane);
            line[block * 16 + bit] = colour;
        }
    }
}

void render_scene(const Assets *assets, const Camera *camera,
                  const Billboard *sprites, size_t sprite_count,
                  uint8_t planar[SCENE_PLANAR_BYTES])
{
    uint8_t line[SCREEN_W];
    for (int y = 0; y < SCENE_H; ++y) {
        scene_line(assets, camera, y, sprites, sprite_count, line);
        c2p_line(line, planar + y * SCREEN_W / 2);
    }
}

void render_map_line(const Assets *assets, int y, uint8_t line[SCREEN_W])
{
    memset(line, 1, SCREEN_W);
    for (int x = 0; x < SCREEN_W; ++x) {
        if (x < 24 || x >= 232 || y < 5 || y >= 107) continue;
        if (x < 28 || x >= 228 || y < 9 || y >= 103) {
            line[x] = 14;
            continue;
        }
        int tx = (x - 28) * MAP_TILES / 200;
        int ty = (y - 9) * MAP_TILES / 94;
        uint8_t id = assets->map[ty * MAP_TILES + tx];
        line[x] = id == 2 || id == 3 || id == 6 ? 8 :
                  id == 4 ? 15 : id == 5 ? 5 : 3;
    }
    /* Current kart position near the southern straight. */
    if (y >= 78 && y < 83)
        for (int x = 126; x < 131; ++x) line[x] = 12;
}

void render_frame(const Assets *assets, const Camera *camera,
                  const Billboard *sprites, size_t sprite_count,
                  uint8_t planar[FRAME_PLANAR_BYTES])
{
    uint8_t line[SCREEN_W];
    render_scene(assets, camera, sprites, sprite_count, planar);
    for (int y = 0; y < SCENE_H; ++y) {
        render_map_line(assets, y, line);
        c2p_line(line, planar + (SCENE_H + y) * SCREEN_W / 2);
    }
}

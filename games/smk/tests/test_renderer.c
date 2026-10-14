#include "renderer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_packed_nibbles(void)
{
    uint8_t bytes[8] = {0};
    for (size_t i = 0; i < 16; ++i) packed_set(bytes, i, (uint8_t)i);
    for (size_t i = 0; i < 16; ++i) assert(packed_get(bytes, i) == i);
    assert(bytes[0] == 0x01 && bytes[7] == 0xef);
}

static void test_planar_layout(void)
{
    uint8_t line[SCREEN_W] = {0};
    uint8_t planar[SCREEN_W / 2] = {0};
    uint8_t decoded[SCREEN_W] = {0};
    line[0] = 1;
    line[1] = 2;
    line[2] = 4;
    line[3] = 8;
    c2p_line(line, planar);
    /* Falcon 4-plane words are interleaved per 16-pixel group, big endian. */
    const uint8_t expected[] = {0x80, 0x00, 0x40, 0x00,
                                0x20, 0x00, 0x10, 0x00};
    assert(memcmp(planar, expected, sizeof(expected)) == 0);
    p2c_line(planar, decoded);
    assert(memcmp(line, decoded, sizeof(line)) == 0);
    for (size_t i = 0; i < SCREEN_W; ++i) line[i] = (uint8_t)(i & 15u);
    c2p_line(line, planar);
    p2c_line(planar, decoded);
    assert(memcmp(line, decoded, sizeof(line)) == 0);
}

static void test_track_wrap_and_frame(void)
{
    Assets *assets = malloc(sizeof(*assets));
    uint8_t *frame = malloc(FRAME_PLANAR_BYTES);
    assert(assets && frame);
    assets_generate(assets);
    for (int y = -17; y < 18; ++y)
        for (int x = -17; x < 18; ++x)
            assert(track_sample(assets, x, y) ==
                   track_sample(assets, x + 1024, y + 1024));
    Camera camera = {512, 784, 1.5707963267948966};
    Billboard sprite = {108, 70, 40, 40, 0};
    render_frame(assets, &camera, &sprite, 1, frame);
    uint8_t line[SCREEN_W];
    p2c_line(frame + 90 * SCREEN_W / 2, line);
    int red_pixels = 0;
    for (size_t x = 100; x < 160; ++x) red_pixels += line[x] == 11;
    assert(red_pixels > 0);
    free(frame);
    free(assets);
}

int main(void)
{
    test_packed_nibbles();
    test_planar_layout();
    test_track_wrap_and_frame();
    puts("renderer tests passed");
    return 0;
}

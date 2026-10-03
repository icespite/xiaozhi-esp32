#include "gifdec.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

static const unsigned char animation[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x03, 0x00, 0x02, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x08, 0x06, 0x00, 0x01, 0x08, 0x1c, 0x18, 0x10, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x81, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x06, 0x00, 0x01, 0x08, 0x1c, 0x18, 0x10, 0x00, 0x3b};

static void Decode(std::vector<unsigned char> data) {
    // Keep fuzzed dimensions small; the webpage checks dimensions before decoding.
    if (data.size() >= 10) { data[6] &= 15; data[7] = 0; data[8] &= 15; data[9] = 0; }
    auto* gif = gd_open_gif_data_sized(data.data(), data.size());
    if (!gif) return;
    for (int i = 0; i < 8; ++i) {
        if (gd_get_frame(gif) != 1) break;
        gd_render_frame(gif, gif->canvas);
    }
    gd_close_gif(gif);
}

int main() {
    auto* gif = gd_open_gif_data_sized(animation, sizeof(animation));
    assert(gif && gif->width == 3 && gif->height == 2);
    assert(gd_get_frame(gif) == 1);
    gd_render_frame(gif, gif->canvas);
    assert(gif->canvas[0] == 0 && gif->canvas[3] == 255);
    assert(gd_get_frame(gif) == 1);
    gd_render_frame(gif, gif->canvas);
    assert(gif->canvas[0] == 255 && gif->canvas[3] == 255);
    assert(gd_get_frame(gif) == 1); // Infinite loop returns to first frame.
    gd_close_gif(gif);
    for (size_t size = 0; size < sizeof(animation); ++size) {
        Decode({animation, animation + size});
    }
    // Missing color table / invalid LZW / extension sizes / frame rectangles.
    std::mt19937 rng(42);
    for (int i = 0; i < 20000; ++i) {
        std::vector<unsigned char> data(animation, animation + sizeof(animation));
        for (int j = 0; j < 1 + i % 8; ++j) data[rng() % data.size()] = rng() % 256;
        Decode(std::move(data));
    }
    std::cout << "GIF animation, all truncated prefixes and 20000 mutated payloads passed.\n";
}

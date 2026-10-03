#include "gifdec.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    assert(argc == 4);
    std::ifstream source(argv[1], std::ios::binary);
    std::ifstream pixels(argv[2], std::ios::binary);
    assert(source.good() && pixels.good());
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(source)), {});
    std::vector<unsigned char> expected((std::istreambuf_iterator<char>(pixels)), {});
    auto* gif = gd_open_gif_data_sized(data.data(), data.size());
    assert(gif);
    const size_t frame_size = size_t(gif->width) * gif->height * 4;
    assert(expected.size() == frame_size * 3);
    const int loops = std::stoi(argv[3]); // -1: once, 0: infinite, >0: repeats.
    const int cycles = loops == 0 ? 3 : loops < 0 ? 1 : loops + 1;
    const int delays[] = {7, 19, 33}; // GIF units are 10 milliseconds.
    for (int i = 0; i < cycles * 3; ++i) {
        assert(gd_get_frame(gif) == 1);
        assert(gif->gce.delay == delays[i % 3]);
        gd_render_frame(gif, gif->canvas);
        // The server output is black/white, so RGBA and BGRA channels agree.
        assert(std::memcmp(gif->canvas, expected.data() + (i % 3) * frame_size, frame_size) == 0);
    }
    if (loops != 0) assert(gd_get_frame(gif) == 0);
    gd_close_gif(gif);
    std::cout << "Uploaded GIF pixels, frame timing and loops match the device decoder\n";
}

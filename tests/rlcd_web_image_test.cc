#include "web_image.h"
#include "jpg/jpeg_to_image.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

// JPEG uses ESP-IDF APIs; this test exercises uploaded PNGs through real LVGL.
extern "C" esp_err_t jpeg_to_image(const uint8_t*, size_t, uint8_t**, size_t*,
                                    size_t*, size_t*, size_t*) {
    return ESP_FAIL;
}

static std::vector<uint8_t> Read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}

int main(int argc, char** argv) {
    assert(argc > 1);
    lv_init();
    auto* display = lv_display_create(400, 300);
    std::vector<uint16_t> framebuffer(400 * 300);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer.data(), nullptr,
                           framebuffer.size() * sizeof(uint16_t), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* disp, const lv_area_t*, uint8_t*) {
        lv_display_flush_ready(disp);
    });
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_white(), 0);
    auto* image = lv_image_create(lv_screen_active());
    lv_obj_set_pos(image, 0, 0);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    // Repeat page turns and refreshes to exercise descriptor/cache ownership.
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int arg = 1; arg < argc; ++arg) {
            auto png = Read(argv[arg]);
            auto expected = Read(std::string(argv[arg]) + ".gray");
            web_page::WebImage media;
            media.size = png.size();
            media.bytes.reset(static_cast<uint8_t*>(malloc(media.size)));
            assert(media.bytes);
            memcpy(media.bytes.get(), png.data(), media.size);
            for (int visit = 0; visit < 2; ++visit) {
                const auto* descriptor = media.Decode();
                assert(descriptor && media.error.empty());
                assert(expected.size() == size_t(descriptor->header.w) * descriptor->header.h);
                lv_obj_set_size(image, descriptor->header.w, descriptor->header.h);
                lv_image_set_src(image, descriptor);
                // A successful decode alone does not mean the widget accepted it.
                assert(lv_image_get_src(image) == descriptor);
                lv_refr_now(display);
                for (unsigned y = 0; y < descriptor->header.h; ++y) {
                    for (unsigned x = 0; x < descriptor->header.w; ++x) {
                        uint16_t pixel = expected[y * descriptor->header.w + x] ? 0xffff : 0;
                        assert(framebuffer[y * 400 + x] == pixel);
                    }
                }
                lv_image_set_src(image, nullptr);
                media.Release();
                lv_refr_now(display);
                assert(framebuffer[0] == 0xffff);
            }
        }
    }
    lv_display_delete(display);
    lv_deinit();
    std::cout << "Uploaded PNGs accepted by LVGL; RGB565 pixels and repeated page turns passed\n";
}

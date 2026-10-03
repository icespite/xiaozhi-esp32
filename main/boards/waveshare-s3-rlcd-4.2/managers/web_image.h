#pragma once

#include "web_page_parser.h"
#include "gif/lvgl_gif.h"
#include <esp_heap_caps.h>
#include <memory>

namespace web_page {

constexpr size_t kMaxImageBytes = 256 * 1024;
constexpr size_t kMaxImagePixels = 512 * 1024;
constexpr size_t kMaxMediaBytes = 1024 * 1024;

// Keep compressed data; decode only the current image. LVGL calls need the display lock.
class WebImage {
public:
    using Buffer = std::unique_ptr<uint8_t, decltype(&heap_caps_free)>;
    Buffer bytes{nullptr, heap_caps_free};
    size_t size = 0;
    std::string alt;
    std::string error;
    ~WebImage();
    const lv_image_dsc_t* Decode();
    void Release();
    void SetPlaying(bool playing);
    void SetFrameCallback(std::function<void()> callback);

private:
    lv_image_dsc_t descriptor_{};
    lv_draw_buf_t* png_ = nullptr;
    Buffer jpeg_{nullptr, heap_caps_free};
    std::unique_ptr<LvglGif> gif_;
};

}  // namespace web_page

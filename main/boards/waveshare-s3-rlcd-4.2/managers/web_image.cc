#include "web_image.h"
#include "jpg/jpeg_to_image.h"
#define LODEPNG_NO_COMPILE_CPP
#include <src/libs/lodepng/lodepng.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <src/misc/cache/instance/lv_image_header_cache.h>
#include <algorithm>

namespace web_page {

WebImage::~WebImage() { Release(); }

const lv_image_dsc_t* WebImage::Decode() {
    if (!error.empty()) return nullptr;
    if (gif_) return gif_->image_dsc();
    if (descriptor_.data) return &descriptor_;
    auto info = InspectImage(std::string_view(reinterpret_cast<const char*>(bytes.get()), size));
    if (!info.width || !info.height || info.width > 2048 || info.height > 2048 ||
        info.width > kMaxImagePixels / info.height) {
        error = "图片尺寸过大或格式不支持";
        return nullptr;
    }
    descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    if (info.format == ImageFormat::Gif) {
        lv_image_dsc_t source{};
        source.data = bytes.get();
        source.data_size = size;
        gif_ = std::make_unique<LvglGif>(&source);
        if (gif_->IsLoaded()) return gif_->image_dsc();
    } else if (info.format == ImageFormat::Png) {
        LodePNGState state;
        lodepng_state_init(&state);
        state.decoder.zlibsettings.max_output_size = kMaxImagePixels * 8 + 16384;
#ifdef LODEPNG_COMPILE_ANCILLARY_CHUNKS
        state.decoder.read_text_chunks = 0;
        state.decoder.max_icc_size = 4096;
#endif
        unsigned char* output = nullptr;
        unsigned width = 0, height = 0;
        unsigned result = lodepng_decode(&output, &width, &height, &state, bytes.get(), size);
        lodepng_state_cleanup(&state);
        // LVGL's bundled LodePNG returns a draw buffer, not a bare pixel array.
        png_ = reinterpret_cast<lv_draw_buf_t*>(output);
        if (!result && png_ && width == info.width && height == info.height) {
            for (size_t i = 0; i < size_t(width) * height; ++i) {
                std::swap(png_->data[i * 4], png_->data[i * 4 + 2]);
            }
            descriptor_.header = png_->header;
            // This is an image descriptor, not the lv_draw_buf_t that owns the
            // pixels. ALLOCATED would make LVGL read nonexistent buffer fields.
            descriptor_.header.flags &= ~LV_IMAGE_FLAGS_ALLOCATED;
            descriptor_.data = png_->data;
            descriptor_.data_size = png_->data_size;
            return &descriptor_;
        }
    } else if (info.format == ImageFormat::Jpeg) {
        uint8_t* output = nullptr;
        size_t length = 0, width = 0, height = 0, stride = 0;
        auto result = jpeg_to_image(bytes.get(), size, &output, &length, &width, &height, &stride);
        jpeg_.reset(output);
        if (result == ESP_OK && output) {
            descriptor_.header.cf = LV_COLOR_FORMAT_RGB565;
            descriptor_.header.w = width;
            descriptor_.header.h = height;
            descriptor_.header.stride = stride;
            descriptor_.data = output;
            descriptor_.data_size = length;
            return &descriptor_;
        }
    }
    Release();
    error = "图片解码失败";
    return nullptr;
}

void WebImage::Release() {
    if (descriptor_.data) {
        lv_image_cache_drop(&descriptor_);
        lv_image_header_cache_drop(&descriptor_);
    }
    if (gif_ && gif_->image_dsc()) {
        lv_image_cache_drop(gif_->image_dsc());
        lv_image_header_cache_drop(gif_->image_dsc());
    }
    gif_.reset();
    if (png_) lv_draw_buf_destroy(png_);
    png_ = nullptr;
    jpeg_.reset();
    descriptor_ = {};
}

void WebImage::SetPlaying(bool playing) {
    if (!gif_) return;
    if (playing && !gif_->IsPlaying()) gif_->Start();
    else if (!playing) gif_->Pause();
}

void WebImage::SetFrameCallback(std::function<void()> callback) {
    if (gif_) gif_->SetFrameCallback(std::move(callback));
}

}  // namespace web_page

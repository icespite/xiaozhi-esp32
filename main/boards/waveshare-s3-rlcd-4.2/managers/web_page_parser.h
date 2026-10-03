#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace web_page {

enum class ImageFormat { Unsupported, Png, Jpeg, Gif };
struct ImageInfo {
    ImageFormat format = ImageFormat::Unsupported;
    uint32_t width = 0;
    uint32_t height = 0;
};
// Header inspection only; decoders still validate the complete payload.
ImageInfo InspectImage(std::string_view bytes);

struct ImageReference {
    size_t text_offset = 0;
    std::string url;
    std::string alt;
};

struct TextContent {
    std::string text;
    bool truncated = false;
    std::vector<ImageReference> images;
};

// Resolve an image reference against the final document URL. Only HTTP(S).
std::string ResolveUrl(std::string_view base, std::string_view reference);

// Retains image positions in the bounded text stream, including lazy image sources.
TextContent ExtractPage(std::string_view html, std::string_view base_url,
                        size_t max_bytes = 8192, size_t max_images = 8);

// A bounded HTML text reader, not a CSS/JavaScript rendering engine.
TextContent ExtractText(std::string_view html, size_t max_bytes = 8192);

}  // namespace web_page

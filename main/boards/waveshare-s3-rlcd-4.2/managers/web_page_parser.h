#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace web_page {

struct TextContent {
    std::string text;
    bool truncated = false;
};

// A bounded HTML text reader, not a CSS/JavaScript rendering engine.
TextContent ExtractText(std::string_view html, size_t max_bytes = 8192);

}  // namespace web_page

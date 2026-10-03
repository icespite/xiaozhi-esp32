#include "web_url.h"

#include <nvs.h>

namespace web_page {

std::string GetUrl() {
    nvs_handle_t handle;
    if (nvs_open("web", NVS_READONLY, &handle) != ESP_OK) return kDefaultUrl;
    size_t length = 0;
    std::string value;
    if (nvs_get_str(handle, "url", nullptr, &length) == ESP_OK &&
        length > 0 && length <= kMaxUrlBytes + 1) {
        value.resize(length);
        if (nvs_get_str(handle, "url", value.data(), &length) == ESP_OK) value.resize(length - 1);
        else value.clear();
    }
    nvs_close(handle);
    std::string url;
    return NormalizeUrl(value, url) ? url : kDefaultUrl;
}

}  // namespace web_page

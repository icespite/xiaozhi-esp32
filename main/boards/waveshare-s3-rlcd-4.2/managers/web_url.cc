#include "web_url.h"

#include <nvs.h>

namespace web_page {

namespace {
std::string GetStoredUrl(const char* key, const char* fallback) {
    nvs_handle_t handle;
    if (nvs_open("web", NVS_READONLY, &handle) != ESP_OK) return fallback;
    size_t length = 0;
    std::string value;
    if (nvs_get_str(handle, key, nullptr, &length) == ESP_OK &&
        length > 0 && length <= kMaxUrlBytes + 1) {
        value.resize(length);
        if (nvs_get_str(handle, key, value.data(), &length) == ESP_OK) value.resize(length - 1);
        else value.clear();
    }
    nvs_close(handle);
    std::string url;
    return !value.empty() && NormalizeUrl(value, url) ? url : fallback;
}
}  // namespace

std::string GetUrl() { return GetStoredUrl("url", kDefaultUrl); }
std::string GetUploadUrl() { return GetStoredUrl("upload_url", kDefaultUploadUrl); }

}  // namespace web_page

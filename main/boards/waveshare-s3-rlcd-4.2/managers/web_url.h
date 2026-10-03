#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <sys/socket.h>
#include <arpa/inet.h>

namespace web_page {

inline constexpr char kDefaultUrl[] = "https://icespite.top/";
inline constexpr char kDefaultUploadUrl[] = "http://192.168.8.176:8001/display";
inline constexpr size_t kMaxUrlBytes = 1024;

// An empty field restores the default. Reject unsupported schemes, credentials,
// whitespace and malformed authorities before persisting a URL.
inline bool NormalizeUrl(std::string_view input, std::string& url) {
    auto first = input.find_first_not_of(" \t\r\n");
    if (first == input.npos) {
        url = kDefaultUrl;
        return true;
    }
    input = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    if (input.size() > kMaxUrlBytes) return false;
    for (unsigned char c : input) {
        if (c <= 0x20 || c == 0x7f || c == '\\') return false;
    }
    std::string normalized(input);
    auto separator = normalized.find("://");
    if (separator == std::string::npos) return false;
    for (size_t i = 0; i < separator; ++i) {
        if (normalized[i] >= 'A' && normalized[i] <= 'Z') normalized[i] += 'a' - 'A';
    }
    if (normalized.compare(0, separator, "http") != 0 &&
        normalized.compare(0, separator, "https") != 0) return false;
    auto authority = std::string_view(normalized).substr(separator + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (authority.empty() || authority.find('@') != authority.npos) return false;
    std::string_view port;
    if (authority.front() == '[') {
        auto end = authority.find(']');
        if (end == authority.npos || end <= 1) return false;
        in6_addr address;
        const std::string host(authority.substr(1, end - 1));
        if (inet_pton(AF_INET6, host.c_str(), &address) != 1) return false;
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') return false;
            port = authority.substr(end + 2);
            if (port.empty()) return false;
        }
    } else {
        auto colon = authority.find(':');
        auto host = authority.substr(0, colon);
        if (host.empty()) return false;
        for (unsigned char c : host) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
        }
        if (colon != authority.npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) return false;
        }
    }
    if (!port.empty()) {
        if (port.size() > 5) return false;
        unsigned value = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        if (value == 0 || value > 65535) return false;
    }
    url = std::move(normalized);
    return true;
}

std::string GetUrl();
// An empty field restores the default upload server URL.
inline bool NormalizeUploadUrl(std::string_view input, std::string& url) {
    if (input.find_first_not_of(" \t\r\n") == input.npos) {
        url = kDefaultUploadUrl;
        return true;
    }
    return NormalizeUrl(input, url);
}
std::string GetUploadUrl();

}  // namespace web_page

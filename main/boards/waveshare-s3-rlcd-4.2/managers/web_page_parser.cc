#include "web_page_parser.h"

#include <cstdint>

namespace web_page {
namespace {

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

bool IsName(char c) {
    return (Lower(c) >= 'a' && Lower(c) <= 'z') || (c >= '0' && c <= '9');
}

size_t FindClosingTag(std::string_view html, size_t start, const std::string& name) {
    for (size_t pos = start; (pos = html.find("</", pos)) != html.npos; ++pos) {
        size_t i = 0;
        while (i < name.size() && pos + 2 + i < html.size() &&
               Lower(html[pos + 2 + i]) == name[i]) {
            ++i;
        }
        size_t end = pos + 2 + i;
        if (i == name.size() && end < html.size() &&
            (html[end] == '>' || IsSpace(html[end]))) {
            return pos;
        }
    }
    return html.npos;
}

bool IsBlock(const std::string& name) {
    return name == "br" || name == "p" || name == "div" || name == "li" ||
           name == "ul" || name == "ol" || name == "article" || name == "section" ||
           name == "header" || name == "footer" || name == "nav" || name == "tr" ||
           name == "hr" || name == "pre" || name == "blockquote" ||
           (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
}

uint32_t DecodeEntity(std::string_view entity) {
    if (entity == "amp") return '&';
    if (entity == "lt") return '<';
    if (entity == "gt") return '>';
    if (entity == "quot") return '"';
    if (entity == "apos" || entity == "#39") return '\'';
    if (entity == "nbsp" || entity == "ensp" || entity == "emsp") return ' ';
    if (entity == "copy") return 0xa9;
    if (entity == "ndash") return 0x2013;
    if (entity == "mdash") return 0x2014;
    if (entity == "hellip") return 0x2026;
    if (entity == "middot") return 0xb7;
    if (entity == "bull") return 0x2022;
    if (entity.empty() || entity[0] != '#') return 0;
    size_t pos = 1;
    uint32_t base = 10;
    if (pos < entity.size() && Lower(entity[pos]) == 'x') {
        base = 16;
        ++pos;
    }
    if (pos == entity.size()) return 0;
    uint32_t value = 0;
    for (; pos < entity.size(); ++pos) {
        char c = Lower(entity[pos]);
        uint32_t digit = c >= '0' && c <= '9' ? c - '0' :
                         c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
        if (digit >= base || value > (0x10ffff - digit) / base) return 0;
        value = value * base + digit;
    }
    if (value < 0x20 || (value >= 0xd800 && value <= 0xdfff)) return 0;
    return value == 0xa0 ? ' ' : value;
}

std::string Utf8(uint32_t cp) {
    std::string result;
    if (cp < 0x80) {
        result += static_cast<char>(cp);
    } else if (cp < 0x800) {
        result += static_cast<char>(0xc0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        result += static_cast<char>(0xe0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        result += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        result += static_cast<char>(0xf0 | (cp >> 18));
        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        result += static_cast<char>(0x80 | (cp & 0x3f));
    }
    return result;
}

std::string DecodeAttribute(std::string_view value) {
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '&') {
            size_t end = value.find(';', i + 1);
            if (end != value.npos && end - i <= 16) {
                uint32_t cp = DecodeEntity(value.substr(i + 1, end - i - 1));
                if (cp) {
                    result += Utf8(cp);
                    i = end;
                    continue;
                }
            }
        }
        if (static_cast<unsigned char>(value[i]) >= 0x20) result += value[i];
    }
    return result;
}

std::string Attribute(std::string_view tag, std::string_view wanted) {
    for (size_t pos = 0; pos < tag.size();) {
        while (pos < tag.size() && (IsSpace(tag[pos]) || tag[pos] == '/')) ++pos;
        size_t start = pos;
        while (pos < tag.size() && !IsSpace(tag[pos]) && tag[pos] != '=' && tag[pos] != '/') ++pos;
        std::string name;
        for (size_t i = start; i < pos; ++i) name += Lower(tag[i]);
        while (pos < tag.size() && IsSpace(tag[pos])) ++pos;
        if (pos == tag.size()) break;
        if (tag[pos] != '=') continue;
        ++pos;
        while (pos < tag.size() && IsSpace(tag[pos])) ++pos;
        char quote = pos < tag.size() && (tag[pos] == '\'' || tag[pos] == '"') ? tag[pos++] : 0;
        start = pos;
        while (pos < tag.size() && (quote ? tag[pos] != quote : !IsSpace(tag[pos]))) ++pos;
        auto value = tag.substr(start, pos - start);
        if (quote && pos < tag.size()) ++pos;
        if (name == wanted) return DecodeAttribute(value);
    }
    return {};
}

}  // namespace

std::string ResolveUrl(std::string_view base, std::string_view reference) {
    while (!reference.empty() && IsSpace(reference.front())) reference.remove_prefix(1);
    while (!reference.empty() && IsSpace(reference.back())) reference.remove_suffix(1);
    reference = reference.substr(0, reference.find('#'));
    if (reference.empty() || reference.size() > 2048) return {};
    for (unsigned char c : reference) if (c <= 0x20 || c == 0x7f || c == '\\') return {};
    std::string url;
    auto scheme_end = base.find("://");
    if (reference.substr(0, 2) == "//") {
        if (scheme_end == base.npos) return {};
        url = std::string(base.substr(0, scheme_end)) + ":" + std::string(reference);
    } else if (reference.find(':') < reference.find_first_of("/?")) {
        url = reference;
    } else {
        if (scheme_end == base.npos) return {};
        size_t path = base.find_first_of("/?#", scheme_end + 3);
        std::string origin(base.substr(0, path));
        std::string base_path = path == base.npos || base[path] != '/' ? "/" :
                                std::string(base.substr(path, base.find_first_of("?#", path) - path));
        if (reference.front() == '/') url = origin + std::string(reference);
        else if (reference.front() == '?') url = origin + base_path + std::string(reference);
        else url = origin + base_path.substr(0, base_path.rfind('/') + 1) + std::string(reference);
    }
    scheme_end = url.find("://");
    if (scheme_end == url.npos) return {};
    std::string scheme = url.substr(0, scheme_end);
    for (char& c : scheme) c = Lower(c);
    if (scheme != "https" && scheme != "http") return {};
    size_t path = url.find_first_of("/?", scheme_end + 3);
    std::string authority = url.substr(scheme_end + 3, path - scheme_end - 3);
    if (authority.empty() || authority.find('@') != authority.npos) return {};
    std::string origin = scheme + "://" + authority;
    if (path == url.npos) return origin + "/";
    if (url[path] == '?') return origin + "/" + url.substr(path);
    size_t query = url.find('?', path);
    std::string_view raw_path(url.data() + path, (query == url.npos ? url.size() : query) - path);
    std::vector<std::string_view> parts;
    for (size_t pos = 1; pos <= raw_path.size();) {
        size_t end = raw_path.find('/', pos);
        if (end == raw_path.npos) end = raw_path.size();
        auto part = raw_path.substr(pos, end - pos);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (part != ".") parts.push_back(part);
        pos = end + 1;
    }
    for (auto part : parts) { origin += '/'; origin.append(part.data(), part.size()); }
    if (parts.empty() || raw_path.substr(raw_path.size() >= 2 ? raw_path.size() - 2 : 0) == "/." ||
        raw_path.substr(raw_path.size() >= 3 ? raw_path.size() - 3 : 0) == "/..") origin += '/';
    if (query != url.npos) origin += url.substr(query);
    return origin.size() <= 2048 ? origin : std::string{};
}

static TextContent Extract(std::string_view html, size_t max_bytes,
                           std::string_view base_url, size_t max_images) {
    TextContent result;
    char separator = 0;
    bool images_truncated = false;
    auto append = [&](std::string_view value) {
        size_t padding = separator && !result.text.empty() ? 1 : 0;
        if (padding + value.size() > max_bytes - result.text.size()) {
            result.truncated = true;
            return;
        }
        if (padding) result.text += separator;
        separator = 0;
        result.text.append(value.data(), value.size());
    };

    for (size_t pos = 0; pos < html.size() && !result.truncated;) {
        if (html.substr(pos, 4) == "<!--") {
            size_t end = html.find("-->", pos + 4);
            pos = end == html.npos ? html.size() : end + 3;
            continue;
        }
        if (html[pos] == '<' && pos + 1 < html.size() &&
            (IsName(html[pos + 1]) || html[pos + 1] == '/' || html[pos + 1] == '!')) {
            size_t start = pos + 1;
            bool closing = html[start] == '/';
            if (closing) ++start;
            size_t name_end = start;
            while (name_end < html.size() && IsName(html[name_end])) ++name_end;
            std::string name;
            for (size_t i = start; i < name_end; ++i) name += Lower(html[i]);
            // A '>' inside a quoted attribute does not end a tag.
            char quote = 0;
            size_t end = name_end;
            for (; end < html.size(); ++end) {
                char c = html[end];
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (c == '\'' || c == '"') {
                    quote = c;
                } else if (c == '>') {
                    break;
                }
            }
            if (end == html.size()) break;
            pos = end + 1;
            if (!closing && (name == "script" || name == "style" || name == "head" ||
                             name == "textarea" || name == "noscript" ||
                             name == "template" || name == "svg")) {
                size_t close = FindClosingTag(html, pos, name);
                pos = close == html.npos ? html.size() : close;
            }
            if (!closing && name == "img" && max_images) {
                if (result.images.size() >= max_images) {
                    images_truncated = true;
                    separator = '\n';
                    continue;
                }
                auto attributes = html.substr(name_end, end - name_end);
                std::string source = Attribute(attributes, "data-src");
                if (source.empty()) source = Attribute(attributes, "data-original");
                if (source.empty()) source = Attribute(attributes, "src");
                if (source.empty()) {
                    source = Attribute(attributes, "srcset");
                    source = source.substr(0, source.find_first_of(" ,\t\r\n"));
                }
                auto alt = ExtractText(Attribute(attributes, "alt"), 160).text;
                result.images.push_back({result.text.size(), ResolveUrl(base_url, source), std::move(alt)});
                separator = '\n';
            }
            if (IsBlock(name)) separator = '\n';
            if (name == "td" && separator != '\n') separator = ' ';
            continue;
        }
        if (IsSpace(html[pos])) {
            if (!separator) separator = ' ';
            ++pos;
            continue;
        }
        if (html[pos] == '&') {
            size_t end = html.find(';', pos + 1);
            if (end != html.npos && end - pos <= 16) {
                uint32_t cp = DecodeEntity(html.substr(pos + 1, end - pos - 1));
                if (cp) {
                    if (cp == ' ') {
                        if (!separator) separator = ' ';
                    } else {
                        append(Utf8(cp));
                    }
                    pos = end + 1;
                    continue;
                }
            }
        }
        unsigned char first = html[pos];
        if (first < 0x20 || first == 0x7f) {
            ++pos;
            continue;
        }
        size_t bytes = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                       first >= 0xe0 && first <= 0xef ? 3 :
                       first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = bytes && pos + bytes <= html.size();
        for (size_t i = 1; valid && i < bytes; ++i) {
            valid = (static_cast<unsigned char>(html[pos + i]) & 0xc0) == 0x80;
        }
        if (valid && bytes >= 3) {
            unsigned char second = html[pos + 1];
            valid = !(first == 0xe0 && second < 0xa0) &&
                    !(first == 0xed && second >= 0xa0) &&
                    !(first == 0xf0 && second < 0x90) &&
                    !(first == 0xf4 && second >= 0x90);
        }
        if (!valid) {
            ++pos;
            continue;
        }
        append(html.substr(pos, bytes));
        pos += bytes;
    }
    result.truncated = result.truncated || images_truncated;
    return result;
}

ImageInfo InspectImage(std::string_view bytes) {
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
    auto be16 = [&](size_t i) -> uint32_t { return (uint32_t(p[i]) << 8) | p[i + 1]; };
    auto be32 = [&](size_t i) -> uint32_t { return (be16(i) << 16) | be16(i + 2); };
    if (bytes.size() >= 33 && bytes.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8) &&
        be32(8) == 13 && bytes.substr(12, 4) == "IHDR") {
        return {ImageFormat::Png, be32(16), be32(20)};
    }
    if (bytes.size() >= 13 && (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a")) {
        return {ImageFormat::Gif, uint32_t(p[6]) | (uint32_t(p[7]) << 8),
                                 uint32_t(p[8]) | (uint32_t(p[9]) << 8)};
    }
    if (bytes.size() >= 4 && p[0] == 0xff && p[1] == 0xd8) {
        size_t pos = 2;
        while (pos < bytes.size() && p[pos++] == 0xff) {
            while (pos < bytes.size() && p[pos] == 0xff) ++pos;
            if (pos >= bytes.size()) break;
            uint8_t marker = p[pos++];
            if (marker == 0xda || marker == 0xd9) break;
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
            if (bytes.size() - pos < 2) break;
            uint32_t length = be16(pos);
            if (length < 2 || length > bytes.size() - pos) break;
            if ((marker == 0xc0 || marker == 0xc1 || marker == 0xc2) && length >= 8) {
                return {ImageFormat::Jpeg, be16(pos + 5), be16(pos + 3)};
            }
            pos += length;
        }
    }
    return {};
}

TextContent ExtractText(std::string_view html, size_t max_bytes) {
    return Extract(html, max_bytes, {}, 0);
}

TextContent ExtractPage(std::string_view html, std::string_view base_url,
                        size_t max_bytes, size_t max_images) {
    return Extract(html, max_bytes, base_url, max_images);
}

}  // namespace web_page

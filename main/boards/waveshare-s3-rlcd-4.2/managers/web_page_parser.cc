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

}  // namespace

TextContent ExtractText(std::string_view html, size_t max_bytes) {
    TextContent result;
    char separator = 0;
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
    return result;
}

}  // namespace web_page

#include "web_url.h"

#include <cassert>
#include <iostream>

int main() {
    std::string url;
    for (auto input : {"", " \t\r\n"}) {
        assert(web_page::NormalizeUrl(input, url));
        assert(url == web_page::kDefaultUrl);
    }
    assert(web_page::NormalizeUrl("  HTTPS://example.com/news?q=one%20two#top\r\n", url));
    assert(url == "https://example.com/news?q=one%20two#top");
    for (auto input : {"http://192.168.1.2:8080/", "http://localhost", "http://[::1]:8080/",
                       "https://example.com/中文?q=1&x=2", "https://example.com:65535/"}) {
        assert(web_page::NormalizeUrl(input, url));
        assert(url == input);
    }
    for (auto input : {"example.com", "ftp://example.com", "javascript:alert(1)", "https://",
                       "https:///path", "https://?q=x", "https://#top", "https://user:pass@example.com",
                       "http://:80/", "http://host:", "http://host:bad/", "http://host:65536/",
                       "http://host:0/", "http://host:999999999999999999/", "http://[::1",
                       "http://[bad:ip]/", "http://[::1]bad/", "https://exa mple.com",
                       "https://example.com/line\nheader", "https://example.com\\evil"}) {
        url = "unchanged";
        assert(!web_page::NormalizeUrl(input, url));
        assert(url == "unchanged");
    }
    assert(!web_page::NormalizeUrl(std::string("https://example.com/\0hidden", 26), url));
    std::string longest = "https://example.com/";
    longest.resize(web_page::kMaxUrlBytes, 'a');
    assert(web_page::NormalizeUrl(longest, url));
    assert(!web_page::NormalizeUrl(longest + "a", url));
    assert(web_page::NormalizeUploadUrl(" \n", url) && url == web_page::kDefaultUploadUrl);
    assert(web_page::NormalizeUploadUrl("http://192.168.1.100:8000/display", url));
    assert(url == "http://192.168.1.100:8000/display");
    assert(!web_page::NormalizeUploadUrl("file:///tmp/image.png", url));
    std::cout << "Web URL validation tests passed\n";
}

#include "web_page_parser.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>

using web_page::ExtractText;

int main(int argc, char** argv) {
    assert(ExtractText("").text.empty());
    assert(ExtractText("<h1>网页标题</h1><p>你好 <b>世界</b></p>").text ==
           "网页标题\n你好 世界");
    assert(ExtractText("<HEAD><title>Hidden</title></HEAD><body>Visible</body>").text == "Visible");
    assert(ExtractText("<textarea >ignore</textarea ><p>正文</p>").text == "正文");
    assert(ExtractText("a<!-- <p>hidden</p> -->b<script>if (a < b) bad()</script>c").text == "abc");
    assert(ExtractText("<style>p {display: none}</style><noscript>fallback</noscript>content").text == "content");
    assert(ExtractText("<p title='a > b'>Text</p><p>Next</p>").text == "Text\nNext");
    assert(ExtractText("<p>  A\n\t B </p><p>C<br>D</p>").text == "A B\nC\nD");
    assert(ExtractText("&lt;b&gt; &amp; &quot;x&quot; &#39;y&#39; &copy;").text == "<b> & \"x\" 'y' ©");
    assert(ExtractText("&#20013;&#x6587;&nbsp;测试").text == "中文 测试");
    assert(ExtractText("&#x1F600;").text == "😀");
    assert(ExtractText("a&unknown;b").text == "a&unknown;b");
    assert(ExtractText("&#x110000; &#xd800;").text == "&#x110000; &#xd800;");
    assert(ExtractText("visible<!-- unfinished").text == "visible");
    assert(ExtractText("visible<script>unfinished").text == "visible");
    assert(ExtractText("2 < 3").text == "2 < 3");
    assert(ExtractText("<p>bad", 0).truncated);
    assert(ExtractText("中文", 3).text == "中");
    assert(ExtractText("中文", 3).truncated);
    assert(ExtractText("中文", 6).text == "中文");
    assert(!ExtractText("中文", 6).truncated);
    assert(ExtractText("中 文", 6).text == "中");
    assert(ExtractText("😀", 3).text.empty());
    assert(ExtractText("a<br>b", 2).text == "a");
    assert(ExtractText(std::string("a\0b", 3)).text == "ab");
    assert(ExtractText(std::string("\xed\xa0\x80") + "ok").text == "ok");

    using web_page::ResolveUrl;
    using web_page::ExtractPage;
    const std::string base = "https://example.org/blog/post.html?old=1";
    assert(ResolveUrl(base, "../images/a.gif?x=1#frame") == "https://example.org/images/a.gif?x=1");
    assert(ResolveUrl(base, "images/photo.jpg") == "https://example.org/blog/images/photo.jpg");
    assert(ResolveUrl(base, "/logo.png") == "https://example.org/logo.png");
    assert(ResolveUrl(base, "//cdn.example.org/a.gif") == "https://cdn.example.org/a.gif");
    assert(ResolveUrl(base, "?new=2") == "https://example.org/blog/post.html?new=2");
    assert(ResolveUrl("https://example.org", "a.png") == "https://example.org/a.png");
    assert(ResolveUrl(base, "HTTPS://cdn.example.org/a.png") == "https://cdn.example.org/a.png");
    assert(ResolveUrl(base, "/a/../../b.png") == "https://example.org/b.png");
    assert(ResolveUrl(base, "/") == "https://example.org/");
    assert(ResolveUrl(base, "/a/.") == "https://example.org/a/");
    assert(ResolveUrl(base, "/a/..") == "https://example.org/");
    assert(ResolveUrl(base, "javascript:alert(1)").empty());
    assert(ResolveUrl(base, "data:image/gif;base64,abc").empty());
    assert(ResolveUrl(base, "https://user:password@example.org/a.png").empty());
    assert(ResolveUrl(base, "#anchor").empty());
    assert(ResolveUrl(base, "bad\\path.png").empty());
    auto page = ExtractPage("<p>before</p><IMG ALT='A &amp; B' SRC='../a.gif?x=1&amp;y=2'><p>after</p>", base);
    assert(page.text == "before\nafter");
    assert(page.images.size() == 1 && page.images[0].text_offset == 6);
    assert(page.images[0].url == "https://example.org/a.gif?x=1&y=2");
    assert(page.images[0].alt == "A & B");
    page = ExtractPage("<img src=a.png><img src=b.gif>body", base);
    assert(page.images.size() == 2 && page.images[1].text_offset == 0);
    assert(page.text == "body");
    assert(ExtractPage("<img data-src='lazy.gif' src='placeholder.png'>", base).images[0].url ==
           "https://example.org/blog/lazy.gif");
    assert(ExtractPage("<img disabled data-original='a.jpg'>", base).images[0].url ==
           "https://example.org/blog/a.jpg");
    assert(ExtractPage("<img srcset='small.png 1x, large.png 2x'>", base).images[0].url ==
           "https://example.org/blog/small.png");
    assert(ExtractPage("<script><img src='bad'></script><!--<img src='bad'>--><img src='ok'>", base).images.size() == 1);
    page = ExtractPage("<img src=a><img src=b><p>keep reading</p>", base, 100, 1);
    assert(page.images.size() == 1 && page.truncated && page.text == "keep reading");
    assert(ExtractPage("<img src=x><img src=x>", base).images.size() == 2);
    assert(ExtractPage("<img src='oops", base).images.empty());
    assert(ExtractPage("<img src='data:image/png;base64,x' alt='fallback'>", base).images[0].url.empty());
    using web_page::InspectImage;
    using web_page::ImageFormat;
    assert(InspectImage("GIF89a").format == ImageFormat::Unsupported);
    assert(InspectImage(std::string("GIF89a\x02\x00\x03\x00\x80\x00\x00", 13)).width == 2);
    assert(InspectImage(std::string("GIF89a\x02\x00\x03\x00\x80\x00\x00", 13)).height == 3);
    assert(InspectImage("<html>404</html>").format == ImageFormat::Unsupported);

    // Exercise arbitrary/truncated network payloads under ASan/UBSan.
    std::mt19937 random(42);
    for (int i = 0; i < 3000; ++i) {
        std::string input;
        size_t size = random() % 1024;
        for (size_t j = 0; j < size; ++j) input += static_cast<char>(random() % 256);
        size_t limit = random() % 256;
        auto result = ExtractText(input, limit);
        assert(result.text.size() <= limit);
        assert(result.text.find('\0') == std::string::npos);
        auto page = ExtractPage(input, base, limit, 4);
        assert(page.text.size() <= limit && page.images.size() <= 4);
        size_t previous = 0;
        for (const auto& image : page.images) {
            assert(image.text_offset >= previous && image.text_offset <= page.text.size());
            previous = image.text_offset;
        }
        ResolveUrl(base, input);
        InspectImage(input);
    }

    if (argc >= 2) {
        std::ifstream file(argv[1]);
        assert(file.good());
        std::string html((std::istreambuf_iterator<char>(file)), {});
        auto result = ExtractText(html);
        auto page = ExtractPage(html, "https://icespite.top/");
        assert(page.images.size() == 3);
        assert(page.images[0].url == "https://cdnpicture.icespite.top/static/logo.png");
        assert(page.images[1].url == "https://icespite.top/images/pic02.jpg");
        assert(!result.truncated);
        assert(result.text.find("珍惜此刻即是在穿越时空") != std::string::npos);
        assert(result.text.find("Contact") != std::string::npos);
        assert(result.text.find("admin@icespite.top") != std::string::npos);
        assert(result.text.find("jquery") == std::string::npos);
        assert(result.text.find("trim") == std::string::npos);
        std::cout << "Live homepage extracted:\n" << result.text << '\n';
    }
    if (argc == 4) {
        const web_page::ImageFormat formats[] = {ImageFormat::Png, ImageFormat::Jpeg};
        for (int i = 2; i < 4; ++i) {
            std::ifstream file(argv[i], std::ios::binary);
            assert(file.good());
            std::string bytes((std::istreambuf_iterator<char>(file)), {});
            auto info = InspectImage(bytes);
            assert(info.format == formats[i - 2] && info.width && info.height);
            for (size_t size = 0; size < bytes.size(); ++size) InspectImage(std::string_view(bytes).substr(0, size));
            std::cout << "Live image header: " << info.width << "x" << info.height << '\n';
        }
    }
    std::cout << "All web page parser checks passed.\n";
}

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
    }

    if (argc == 2) {
        std::ifstream file(argv[1]);
        assert(file.good());
        std::string html((std::istreambuf_iterator<char>(file)), {});
        auto result = ExtractText(html);
        assert(!result.truncated);
        assert(result.text.find("珍惜此刻即是在穿越时空") != std::string::npos);
        assert(result.text.find("Contact") != std::string::npos);
        assert(result.text.find("admin@icespite.top") != std::string::npos);
        assert(result.text.find("jquery") == std::string::npos);
        assert(result.text.find("trim") == std::string::npos);
        std::cout << "Live homepage extracted:\n" << result.text << '\n';
    }
    std::cout << "All web page parser checks passed.\n";
}

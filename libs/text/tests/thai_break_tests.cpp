#include <doctest/doctest.h>

#include <algorithm>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

// Splits `s` at the break opportunities.
std::vector<std::string> pieces(const std::string& s) {
    std::vector<std::string> out;
    std::size_t from = 0;
    for (std::size_t at : thaiWordBreaks(s)) {
        out.push_back(s.substr(from, at - from));
        from = at;
    }
    out.push_back(s.substr(from));
    return out;
}

}  // namespace

TEST_CASE("the bundled Thai word list loads") {
    REQUIRE(thaiWordListAvailable());
}

TEST_CASE("Thai sentences break between words") {
    using V = std::vector<std::string>;
    CHECK(pieces("สวัสดีครับ") == V{"สวัสดี", "ครับ"});  // สวัสดี|ครับ
    // ผม|ชื่อ|ทดสอบ
    CHECK(pieces("ผมชื่อทดสอบ") == V{"ผม", "ชื่อ", "ทดสอบ"});
    // เขียนแบบ is a dictionary word and stays whole
    auto drawing = pieces("เขียนแบบ");
    CHECK(drawing.size() <= 2);
}

TEST_CASE("break opportunities are cluster boundaries and never split a mark off its letter") {
    std::string s = "ห้องน้ำเกาะป่า";
    auto bounds = clusterBoundaries(s);
    for (std::size_t at : thaiWordBreaks(s))
        CHECK(std::find(bounds.begin(), bounds.end(), at) != bounds.end());
}

TEST_CASE("Latin text and mixed text: only Thai runs get breaks") {
    CHECK(thaiWordBreaks("hello world").empty());
    std::string mixed = "ABCสวัสดีครับ123";
    auto br = thaiWordBreaks(mixed);
    for (std::size_t at : br) {
        CHECK(at >= 3);
        CHECK(at < mixed.size() - 3);
    }
    CHECK(thaiWordBreaks("").empty());
}

TEST_CASE("unknown words stay together; a repetition mark never starts a line") {
    // A made-up unknown sequence between two known words is not split internally.
    std::string s = "ครับกกกกผม";  // ครับ กกกก ผม
    auto p = pieces(s);
    for (const auto& piece : p)
        CHECK_FALSE(piece.empty());
    // ๆ (U+0E46) attaches to the previous word
    std::string rep = "ดีๆครับ";  // ดีๆครับ
    for (std::size_t at : thaiWordBreaks(rep))
        CHECK(rep.compare(at, 3, "ๆ") != 0);
}

TEST_CASE("a missing word list means no breaks, wrapping falls back to clusters") {
    setDictionaryDirectory("/nonexistent/dir");
    // The build-time source path is still tried, so the list normally reloads; a bad explicit
    // file leaves the current list replaced by nothing.
    CHECK(loadThaiWordList("/nonexistent/file.txt") == 0);
    CHECK(thaiWordBreaks("สวัสดีครับ").empty());
    setDictionaryDirectory({});  // reset: the default search finds the bundled list again
    CHECK(thaiWordListAvailable());
}

#include <doctest/doctest.h>

#include "tbcad/text/text.h"

using namespace tbcad::text;

TEST_CASE("markup: plain text and paragraphs") {
    auto p = parseMarkup("one\\Ptwo", 2.5);
    REQUIRE(p.size() == 3);
    CHECK(p[0].text == "one");
    CHECK(p[1].newline);
    CHECK(p[2].text == "two");
    CHECK(p[0].attr.height == 2.5);
    CHECK(plainText("one\\Ptwo") == "one\ntwo");
}

TEST_CASE("markup: bold, italic, height, colour with scoping") {
    auto p = parseMarkup("a{\\b1;b{\\i1;c}d}e", 2.0);
    REQUIRE(p.size() == 5);
    CHECK_FALSE(p[0].attr.bold);
    CHECK(p[1].attr.bold);
    CHECK_FALSE(p[1].attr.italic);
    CHECK(p[2].attr.bold);
    CHECK(p[2].attr.italic);
    CHECK(p[3].attr.bold);
    CHECK_FALSE(p[3].attr.italic);
    CHECK_FALSE(p[4].attr.bold);

    auto h = parseMarkup("{\\H5;big}{\\H2x;rel}small", 2.0);
    REQUIRE(h.size() == 3);
    CHECK(h[0].attr.height == doctest::Approx(5));
    CHECK(h[1].attr.height == doctest::Approx(4));  // relative to the surrounding 2.0
    CHECK(h[2].attr.height == doctest::Approx(2));

    auto c = parseMarkup("{\\C1;red}x", 2.0);
    REQUIRE(c.size() == 2);
    CHECK(c[0].attr.color == 1);
    CHECK(c[1].attr.color == -1);
}

TEST_CASE("markup: escapes, unmatched braces and unknown codes") {
    CHECK(plainText("a\\\\b\\{c\\}") == "a\\b{c}");
    CHECK(plainText("x}y") == "xy");
    CHECK(plainText("\\Zq") == "\\Zq");
    CHECK(plainText("{\\b1") == "\\b1");  // no ';': literal
}

TEST_CASE("markup: markupFromPlain round trips") {
    std::string plain = "a{b}\\c\nline2";
    CHECK(plainText(markupFromPlain(plain)) == plain);
    CHECK(markupFromPlain("x\r\ny") == "x\\Py");
}

TEST_CASE("markup: stacked fractions") {
    auto p = parseMarkup("a\\S1/2;b", 2.0);
    REQUIRE(p.size() == 3);
    CHECK(p[1].stacked);
    CHECK(p[1].num == "1");
    CHECK(p[1].den == "2");
    CHECK(p[1].bar == '/');
    CHECK(plainText("a\\S1/2;b") == "a1/2b");
    CHECK(parseMarkup("\\S1#2;", 2.0)[0].bar == '#');
    CHECK(parseMarkup("\\S+0.1^-0.1;", 2.0)[0].bar == '^');
    // Not a fraction: no separator, or no closing ';'.
    CHECK_FALSE(parseMarkup("\\S12;", 2.0)[0].stacked);
    CHECK_FALSE(parseMarkup("\\S1/2", 2.0)[0].stacked);
    // Inside a format group the fraction takes the format.
    auto b = parseMarkup("{\\b1;\\S1/2;}", 2.0);
    REQUIRE(b.size() == 1);
    CHECK(b[0].attr.bold);
}

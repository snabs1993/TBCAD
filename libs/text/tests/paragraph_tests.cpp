// MTEXT paragraphs: tab stops, indents, alignment (also justified and distributed, Thai
// included), spacing before and after, line spacing and column breaks.
#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

MTextData mtext(const std::string& markup, double width = 0) {
    MTextData m;
    m.utf8 = markup;
    m.height = 10;
    m.width = width;
    return m;
}

double widthOf(const std::string& s) {
    return measureWidth(nullptr, s, 10);
}

// Origin x of the glyph at index `i` in layout space.
double glyphX(const TextLayout& l, std::size_t i) {
    return l.glyphs.at(i).origin.x;
}

}  // namespace

TEST_CASE("\\p settings are parsed: indents, alignment, tabs and spacing") {
    auto pieces = parseMarkup("\\pxi-3,l5,r2,qj,t4,8,12,sb1.5,sa2;A", 10);
    REQUIRE(pieces.size() == 2);
    REQUIRE(pieces[0].para.has_value());
    const ParaProps& pp = *pieces[0].para;
    CHECK(pp.firstIndent == doctest::Approx(-3));
    CHECK(pp.leftIndent == doctest::Approx(5));
    CHECK(pp.rightIndent == doctest::Approx(2));
    CHECK(pp.align == ParaProps::Align::Justify);
    REQUIRE(pp.tabs.size() == 3);
    CHECK(pp.tabs[1] == doctest::Approx(8));
    CHECK(pp.spaceBefore == doctest::Approx(1.5));
    CHECK(pp.spaceAfter == doctest::Approx(2));
    // A no-break space is kept as text and a column break is a piece of its own.
    auto nb = parseMarkup("A\\~B\\NC", 10);
    REQUIRE(nb.size() == 3);
    CHECK(nb[0].text ==
          "A\xC2\xA0"
          "B");
    CHECK(nb[1].columnBreak);
}

TEST_CASE("a tab goes to the next tab stop, or to a multiple of four text heights") {
    Document doc;
    {
        TextLayout l = layoutMText(doc, mtext("A\tB"));
        REQUIRE(l.glyphs.size() == 2);
        CHECK(glyphX(l, 1) == doctest::Approx(40));  // 4 x the height of 10
    }
    {
        TextLayout l = layoutMText(doc, mtext("\\pxt25,60;A\tB\tC"));
        REQUIRE(l.glyphs.size() == 3);
        CHECK(glyphX(l, 1) == doctest::Approx(25));
        CHECK(glyphX(l, 2) == doctest::Approx(60));
    }
}

TEST_CASE("first-line and left indents move the lines and narrow the room") {
    Document doc;
    const double w = widthOf("TBCAD");
    // Room for one word per line after the 20 mm left indent.
    TextLayout l = layoutMText(doc, mtext("\\pxi5,l20;TBCAD TBCAD", w + 25));
    REQUIRE(l.lines.size() == 2);
    CHECK(l.lines[0].x0 == doctest::Approx(25));  // first line: left + first-line indent
    CHECK(l.lines[1].x0 == doctest::Approx(20));
    // A hanging indent: the first line starts left of the others.
    TextLayout h = layoutMText(doc, mtext("\\pxi-10,l20;AB\\PCD"));
    REQUIRE(h.lines.size() == 2);
    CHECK(h.lines[0].x0 == doctest::Approx(10));
}

TEST_CASE("a paragraph can be aligned left, centred or right in the frame") {
    Document doc;
    const double w = widthOf("TBCAD");
    const double W = 200;
    TextLayout left = layoutMText(doc, mtext("\\pxql;TBCAD", W));
    TextLayout centre = layoutMText(doc, mtext("\\pxqc;TBCAD", W));
    TextLayout right = layoutMText(doc, mtext("\\pxqr;TBCAD", W));
    CHECK(left.lines[0].x0 == doctest::Approx(0));
    CHECK(centre.lines[0].x0 == doctest::Approx((W - w) / 2));
    CHECK(right.lines[0].x0 == doctest::Approx(W - w));
    // Right indent keeps the text off the right edge.
    TextLayout ri = layoutMText(doc, mtext("\\pxqr,r15;TBCAD", W));
    CHECK(ri.lines[0].x0 == doctest::Approx(W - 15 - w));
}

TEST_CASE("justify stretches the spaces of every line but the last") {
    Document doc;
    const double w = widthOf("TBCAD");
    const double W = 2 * w + widthOf(" ") + 20;  // two words fit with 20 mm to spare
    TextLayout l = layoutMText(doc, mtext("\\pxqj;TBCAD TBCAD TBCAD TBCAD TBCAD", W));
    REQUIRE(l.lines.size() == 3);
    CHECK(l.lines[0].width == doctest::Approx(W));
    CHECK(l.lines[1].width == doctest::Approx(W));
    CHECK(l.lines[2].width == doctest::Approx(w));  // the last line keeps its natural width
    // Distribute stretches the last line too, when it has a space to stretch.
    TextLayout d = layoutMText(doc, mtext("\\pxqd;TBCAD TBCAD", W));
    REQUIRE(d.lines.size() == 1);
    CHECK(d.lines[0].width == doctest::Approx(W));
}

TEST_CASE("justified Thai text puts the slack between words") {
    Document doc;
    // Words joined by zero-width spaces: no visible spaces to stretch.
    const std::string text =
        "สวัสดี\xE2\x80\x8B"
        "ครับ\xE2\x80\x8B"
        "ผม";
    const double natural = widthOf("สวัสดีครับผม");
    const double W = natural + 30;
    TextLayout l = layoutMText(doc, mtext("\\pxqd;" + text, W));
    REQUIRE(l.lines.size() == 1);
    CHECK(l.lines[0].width == doctest::Approx(W));
    // Left aligned it keeps its natural width.
    TextLayout n = layoutMText(doc, mtext(text, W));
    CHECK(n.lines[0].width == doctest::Approx(natural));
}

TEST_CASE("space before and after, and the line spacing factor") {
    Document doc;
    auto pitch = [&](const MTextData& m) {
        TextLayout l = layoutMText(doc, m);
        REQUIRE(l.lines.size() == 2);
        return l.lines[0].baseline - l.lines[1].baseline;
    };
    const double normal = pitch(mtext("A\\PB"));
    CHECK(normal == doctest::Approx(10 * kLineSpacing));
    CHECK(pitch(mtext("\\pxsb4,sa6;A\\P\\pxsb4,sa6;B")) == doctest::Approx(normal + 6 + 4));
    MTextData wide = mtext("A\\PB");
    wide.lineSpacing = 2;
    CHECK(pitch(wide) == doctest::Approx(2 * normal));
}

TEST_CASE("a column break starts the next column") {
    Document doc;
    MTextData m = mtext("A\\PB\\NC\\PD", 50);
    m.columns = 2;
    m.columnGap = 20;
    TextLayout l = layoutMText(doc, m);
    REQUIRE(l.lines.size() == 4);
    CHECK(l.lines[0].x0 == doctest::Approx(0));
    CHECK(l.lines[1].x0 == doctest::Approx(0));
    CHECK(l.lines[2].x0 == doctest::Approx(70));  // 50 + gap 20
    CHECK(l.lines[3].x0 == doctest::Approx(70));
    // The second column starts at the top of the frame.
    CHECK(l.lines[2].baseline == doctest::Approx(l.lines[0].baseline));
}

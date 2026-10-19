// MTEXT character codes: \W width factor, \T tracking, \Q oblique, \A vertical alignment, \K
// strike.
#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

TextLayout lay(const std::string& markup, double width = 0) {
    Document doc;
    MTextData m;
    m.utf8 = markup;
    m.height = 10;
    m.width = width;
    return layoutMText(doc, m);
}

}  // namespace

TEST_CASE("character codes are parsed and scoped by braces") {
    auto p = parseMarkup("{\\W1.5;\\T2;\\Q15;\\A1;\\K A}B", 10);
    REQUIRE(p.size() == 2);
    CHECK(p[0].attr.widthFactor == doctest::Approx(1.5));
    CHECK(p[0].attr.tracking == doctest::Approx(2));
    CHECK(p[0].attr.oblique == doctest::Approx(std::tan(15 * kPi / 180)));
    CHECK(p[0].attr.valign == 1);
    CHECK(p[0].attr.strike);
    CHECK(p[1].attr == Attr{.height = 10});
    // Nonsense values are ignored.
    auto q = parseMarkup("\\W-3;\\T0;\\Q95;\\A7;x", 10);
    REQUIRE(q.size() == 1);
    CHECK(q[0].attr.widthFactor == 1);
    CHECK(q[0].attr.tracking == 1);
    CHECK(q[0].attr.oblique == 0);
    CHECK(q[0].attr.valign == 0);
}

TEST_CASE("\\W widens glyphs and advances, \\T only the spacing") {
    const TextLayout base = lay("HAH");
    const TextLayout wide = lay("\\W2;HAH");
    const TextLayout track = lay("\\T2;HAH");
    REQUIRE(base.glyphs.size() == 3);
    CHECK(wide.lines[0].width == doctest::Approx(2 * base.lines[0].width));
    CHECK(wide.glyphs[0].xScale == doctest::Approx(2));
    CHECK(wide.glyphs[1].origin.x == doctest::Approx(2 * base.glyphs[1].origin.x));
    CHECK(track.lines[0].width == doctest::Approx(2 * base.lines[0].width));
    CHECK(track.glyphs[0].xScale == doctest::Approx(1));
    CHECK(track.glyphs[2].origin.x == doctest::Approx(2 * base.glyphs[2].origin.x));
}

TEST_CASE("\\W works on Thai and in wrapped text") {
    const std::string thai = "สวัสดีครับ";
    const TextLayout a = lay(thai);
    const TextLayout b = lay("\\W0.5;" + thai);
    CHECK(b.lines[0].width == doctest::Approx(0.5 * a.lines[0].width));
    // Wide text wraps sooner.
    const std::string words = "alpha bravo charlie delta echo foxtrot golf";
    CHECK(lay("\\W2;" + words, 200).lines.size() > lay(words, 200).lines.size());
}

TEST_CASE("\\Q slants the glyphs without moving the pen") {
    const TextLayout base = lay("H");
    const TextLayout slant = lay("\\Q20;H");
    CHECK(slant.lines[0].width == doctest::Approx(base.lines[0].width));
    CHECK(slant.glyphs[0].shear == doctest::Approx(std::tan(20 * kPi / 180)));
    CHECK(slant.box.max.x > base.box.max.x);
}

TEST_CASE("\\A puts small text at the bottom, middle or top of the line, \\K strikes through") {
    // The tall letter first, the small one lifted inside the same line:
    TextLayout mid = lay("{\\H10;H}{\\H4;\\A1;x}");
    TextLayout top = lay("{\\H10;H}{\\H4;\\A2;x}");
    TextLayout bot = lay("{\\H10;H}{\\H4;\\A0;x}");
    CHECK(bot.glyphs[1].origin.y == doctest::Approx(bot.glyphs[0].origin.y));
    CHECK(mid.glyphs[1].origin.y - mid.glyphs[0].origin.y == doctest::Approx(3));
    CHECK(top.glyphs[1].origin.y - top.glyphs[0].origin.y == doctest::Approx(6));
    TextLayout k = lay("\\KAB\\kC");
    REQUIRE(k.lines[0].rules.size() == 1);
    CHECK(k.lines[0].rules[0].y == doctest::Approx(k.lines[0].baseline + 3.5));
    CHECK(k.lines[0].rules[0].x0 == doctest::Approx(0));
}

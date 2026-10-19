// Golden checks of the text engine on mixed scripts: justified Thai/Latin lines end flush at the
// frame edge, wrapped lines never overflow, stacked fractions of every kind have the same width
// rules inside justified text.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

TextLayout lay(const std::string& markup, double width) {
    Document doc;
    MTextData m;
    m.utf8 = markup;
    m.height = 10;
    m.width = width;
    return layoutMText(doc, m);
}

// Right edge of the last glyph advance of line `n` (layout x): the line's x0 plus its width.
double rightEdge(const TextLayout& l, std::size_t n) {
    return l.lines.at(n).x0 + l.lines.at(n).width;
}

const std::string kThaiText =
    "การออกแบบโครงสร้างอาคารต้องคำนึงถึงความปลอดภัยของผู้ใช้งานและความแข็งแรงของวัสดุ";

}  // namespace

TEST_CASE("justified mixed Thai and Latin text ends flush on every line but the last") {
    const std::string text =
        "\\pqj;Beam B1 คานคอนกรีตเสริมเหล็ก size 200x400 และ column C2 เสาหลัก "
        "ของอาคาร with note ตามแบบ " +
        kThaiText;
    for (double w : {120.0, 180.0, 260.0}) {
        const TextLayout l = lay(text, w);
        REQUIRE(l.lines.size() >= 2);
        for (std::size_t i = 0; i + 1 < l.lines.size(); ++i) {
            INFO("width " << w << " line " << i << ": " << l.lines[i].text);
            // A line with a single unbreakable unit cannot be stretched.
            if (l.lines[i].text.find(' ') == std::string::npos &&
                l.lines[i].text.find("\xE2\x80\x8B") == std::string::npos)
                continue;
            CHECK(rightEdge(l, i) == doctest::Approx(w).epsilon(1e-4));
        }
        CHECK(rightEdge(l, l.lines.size() - 1) <= w + 1e-6);
    }
}

TEST_CASE("wrapped Thai and mixed text never overflows the frame, at any width") {
    const std::string text = kThaiText + " Slab S1 แผ่นพื้นหนา 150 mm " + kThaiText;
    for (double w = 60; w <= 400; w += 17) {
        const TextLayout l = lay(text, w);
        for (const LayoutLine& ln : l.lines) {
            INFO("width " << w << ": " << ln.text);
            // Only a single cluster wider than the frame may stick out.
            if (ln.width > w + 1e-6)
                CHECK(ln.endGlyph - ln.firstGlyph <= 4);
        }
        // No letters are lost.
        std::size_t total = 0;
        for (const LayoutLine& ln : l.lines)
            total += ln.endGlyph - ln.firstGlyph;
        CHECK(total == l.glyphs.size());
    }
}

TEST_CASE("stacked fractions: bar kinds, width and justification") {
    const TextLayout slash = lay("\\S1/2;", 0);
    const TextLayout hash = lay("\\S1#2;", 0);
    const TextLayout caret = lay("\\S+0.1^-0.2;", 0);
    REQUIRE(slash.lines.size() == 1);
    CHECK(slash.lines[0].rules.size() == 1);  // the bar
    CHECK(hash.lines[0].rules.size() == 1);
    CHECK(caret.lines[0].rules.empty());  // tolerances have no bar
    CHECK(slash.glyphs.size() == 2);
    // The stack is one unit as wide as its wider part, at 70% of the text size.
    const TextLayout plain = lay("1", 0);
    CHECK(slash.lines[0].width == doctest::Approx(plain.lines[0].width * 0.7));
    // Inside justified text the stack is not stretched and nothing is lost.
    const TextLayout j =
        lay("\\pqj;Use \\S1/2; bar for the long span of the beam and the slab "
            "on the second floor of the house",
            120);
    REQUIRE(j.lines.size() >= 2);
    for (std::size_t i = 0; i + 1 < j.lines.size(); ++i)
        CHECK(rightEdge(j, i) == doctest::Approx(120).epsilon(1e-4));
}

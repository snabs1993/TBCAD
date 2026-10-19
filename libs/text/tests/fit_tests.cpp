// TEXT Fit / Aligned / oblique: geometry of the placed text in the drawing, Thai and mixed script
// included, and the oblique shear of the drawn glyph outlines.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

struct Reg {
    Reg() { registerDisplay(); }
};
const Reg kReg;

TextData spanText(const std::string& s, HAlign h, Vec2 a, Vec2 b) {
    TextData t;
    t.utf8 = s;
    t.height = 10;
    t.h = h;
    t.position = a;
    t.alignPoint = b;
    return t;
}

Vec2 endOf(const TextLayout& l) {
    return l.toWorld.apply({l.lines.at(0).width, 0});
}

}  // namespace

TEST_CASE("Fit ends exactly at the align point for Latin, Thai and mixed text") {
    Document doc;
    for (const char* s : {"TBCAD", "สวัสดีครับ", "ABC สวัสดี 123"}) {
        for (Vec2 b : {Vec2{100, 0}, Vec2{30, 40}, Vec2{-60, 25}}) {
            TextLayout l = layoutText(doc, spanText(s, HAlign::Fit, {5, 5}, b + Vec2{5, 5}));
            const Vec2 e = endOf(l);
            CHECK(e.x == doctest::Approx(b.x + 5).epsilon(1e-9));
            CHECK(e.y == doctest::Approx(b.y + 5).epsilon(1e-9));
            // Fit keeps the height: the cap height of the first glyph column stays 10 mm.
            const Vec2 up = l.toWorld.apply({0, 10}) - l.toWorld.apply({0, 0});
            CHECK(length(up) == doctest::Approx(10));
        }
    }
}

TEST_CASE("Aligned keeps the width/height proportion set by the width factor, Fit keeps height") {
    Document doc;
    const TextLayout natural = layoutText(doc, spanText("TBCAD", HAlign::Left, {0, 0}, {}));
    const double w = natural.lines[0].width;
    for (double wf : {1.0, 0.5, 3.0}) {
        TextData t = spanText("TBCAD", HAlign::Aligned, {0, 0}, {50, 0});
        t.widthFactor = wf;
        TextLayout l = layoutText(doc, t);
        CHECK(endOf(l).x == doctest::Approx(50));
        const Vec2 up = l.toWorld.apply({0, 10}) - l.toWorld.apply({0, 0});
        CHECK(length(up) == doctest::Approx(10 * 50 / (w * wf)).epsilon(1e-6));
        t.h = HAlign::Fit;
        TextLayout f = layoutText(doc, t);
        CHECK(endOf(f).x == doctest::Approx(50));
        CHECK(length(f.toWorld.apply({0, 10}) - f.toWorld.apply({0, 0})) == doctest::Approx(10));
    }
}

TEST_CASE("Fit and Aligned with both points equal fall back to the natural text") {
    Document doc;
    for (HAlign h : {HAlign::Fit, HAlign::Aligned}) {
        TextLayout l = layoutText(doc, spanText("TBCAD", h, {7, 7}, {7, 7}));
        REQUIRE(l.lines.size() == 1);
        const Vec2 e = endOf(l);
        CHECK(std::isfinite(e.x));
        CHECK(std::isfinite(e.y));
        CHECK(e.x - 7 == doctest::Approx(l.lines[0].width));
        CHECK(e.y == doctest::Approx(7));
    }
}

TEST_CASE("backward Fit text is mirrored about its start point") {
    Document doc;
    TextData t = spanText("TBCAD", HAlign::Fit, {0, 0}, {80, 0});
    t.backward = true;
    TextLayout l = layoutText(doc, t);
    CHECK(endOf(l).x == doctest::Approx(-80));
    CHECK(l.toWorld.determinant() < 0);
}

TEST_CASE("oblique shears every glyph outline by the same angle") {
    Document doc;
    TextData t;
    t.utf8 = "I";
    t.height = 10;
    double base = 0;  // the upright letter's own top/bottom corner offset
    for (double a : {0.0, 0.3, -0.2}) {
        t.oblique = a;
        Entity e{{}, t};
        geom::DisplayContext ctx;
        ctx.pixelSizeMm = 0.01;
        DisplayList out;
        geom::buildDisplay(doc, e, ctx, out);
        REQUIRE_FALSE(out.items.empty());
        // The slant of the stem: x at the top minus x at the bottom over the height.
        double xTop = 0, xBot = 0, yTop = -1e9, yBot = 1e9;
        for (const auto& it : out.items)
            if (const auto* f = std::get_if<DisplayFill>(&it))
                for (const auto& r : f->rings)
                    for (const Vec2& p : r) {
                        if (p.y > yTop) {
                            yTop = p.y;
                            xTop = p.x;
                        }
                        if (p.y < yBot) {
                            yBot = p.y;
                            xBot = p.x;
                        }
                    }
        CHECK(yTop - yBot == doctest::Approx(10).epsilon(0.02));
        if (a == 0)
            base = xTop - xBot;
        // Shear moves the top by height * tan(a) relative to the upright letter.
        CHECK(xTop - xBot - base == doctest::Approx((yTop - yBot) * std::tan(a)).epsilon(0.02));
    }
}

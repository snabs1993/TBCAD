// MTEXT background mask: a rectangle behind the text, in the drawing background colour (hides
// what is behind) or in a palette colour.
#include <doctest/doctest.h>

#include <algorithm>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

struct Reg {
    Reg() { registerDisplay(); }
};
const Reg kReg;

DisplayList build(const Document& doc, const Entity& e) {
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.01;
    DisplayList out;
    geom::buildDisplay(doc, e, ctx, out);
    return out;
}

MTextData sample() {
    MTextData m;
    m.utf8 = "Hello";
    m.height = 10;
    m.width = 100;
    return m;
}

BBox2 ringBox(const DisplayFill& f) {
    BBox2 b;
    for (Vec2 p : f.rings.at(0))
        b.expand(p);
    return b;
}

}  // namespace

TEST_CASE("no background mask by default") {
    Document doc;
    Entity e{{}, sample()};
    DisplayList l = build(doc, e);
    REQUIRE_FALSE(l.items.empty());
    const auto* f = std::get_if<DisplayFill>(&l.items.front());
    REQUIRE(f);
    CHECK_FALSE(isBackgroundColor(f->style.color));
}

TEST_CASE("a background mask comes first, in the background colour, and is bigger than the frame") {
    Document doc;
    MTextData m = sample();
    m.bgMode = 1;
    m.bgScale = 1.5;
    Entity e{{}, m};
    DisplayList l = build(doc, e);
    REQUIRE(l.items.size() > 1);
    const auto* f = std::get_if<DisplayFill>(&l.items.front());
    REQUIRE(f);
    CHECK(isBackgroundColor(f->style.color));
    const BBox2 box = ringBox(*f);
    const BBox2 frame = layoutMText(doc, m).worldBounds();
    CHECK(box.min.x < frame.min.x);
    CHECK(box.max.x > frame.max.x);
    CHECK(box.min.y < frame.min.y);
    CHECK(box.max.y > frame.max.y);
    // width 100 plus half a text height on each side
    CHECK(box.max.x - box.min.x == doctest::Approx(110).epsilon(0.02));
}

TEST_CASE("a coloured background uses the palette colour and the scale sets the margin") {
    Document doc;
    MTextData m = sample();
    m.bgMode = 2;
    m.bgColor = 1;
    m.bgScale = 1.0;
    Entity e{{}, m};
    const DisplayList l1 = build(doc, e);
    const auto* f = std::get_if<DisplayFill>(&l1.items.front());
    REQUIRE(f);
    CHECK_FALSE(isBackgroundColor(f->style.color));
    CHECK(f->style.color.r == paletteColor(1).r);
    CHECK(ringBox(*f).max.x - ringBox(*f).min.x == doctest::Approx(100));
    m.bgScale = 3.0;
    Entity e2{{}, m};
    const DisplayList l2 = build(doc, e2);
    const auto* g = std::get_if<DisplayFill>(&l2.items.front());
    REQUIRE(g);
    CHECK(ringBox(*g).max.x - ringBox(*g).min.x == doctest::Approx(100 + 2 * 20));
}

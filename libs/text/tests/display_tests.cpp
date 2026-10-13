#include <algorithm>

#include <doctest/doctest.h>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

struct Reg {
    Reg() { registerDisplay(); }
};
const Reg kReg;

DisplayList build(const Document& doc, const Entity& e, double pixelMm = 0.01) {
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = pixelMm;
    DisplayList out;
    geom::buildDisplay(doc, e, ctx, out);
    return out;
}

double maxY(const DisplayList& l) {
    double m = -1e18;
    for (const auto& it : l.items)
        if (const auto* f = std::get_if<DisplayFill>(&it))
            for (const auto& r : f->rings)
                for (const Vec2& p : r)
                    m = std::max(m, p.y);
    return m;
}

TextData thai(const std::string& s) {
    TextData t;
    t.utf8 = s;
    t.height = 10;
    return t;
}

}  // namespace

TEST_CASE("text is registered with the display registry") {
    CHECK(geom::DisplayRegistry::has(EntityKind::Text));
    CHECK(geom::DisplayRegistry::has(EntityKind::MText));
}

TEST_CASE("Latin text displays as filled glyph rings") {
    Document doc;
    Entity e{{}, thai("TBCAD")};
    DisplayList l = build(doc, e);
    REQUIRE(l.items.size() >= 5);
    for (const auto& it : l.items) {
        const auto* f = std::get_if<DisplayFill>(&it);
        REQUIRE(f);
        CHECK_FALSE(f->rings.empty());
        for (const auto& r : f->rings)
            CHECK(r.size() >= 3);
    }
}

TEST_CASE("Thai 'ห้อง': tone mark and vowels are filled rings placed above the base glyphs") {
    Document doc;
    Entity withMark{{}, thai("ห้อง")};     // ห้อง
    Entity without{{}, thai("หอง")};            // หอง
    DisplayList a = build(doc, withMark);
    DisplayList b = build(doc, without);
    REQUIRE_FALSE(a.items.empty());
    CHECK(a.items.size() > b.items.size());  // the mark is its own filled glyph
    CHECK(maxY(a) > maxY(b) + 1e-6);         // and sits above the base glyphs

    TextLayout l = layoutEntity(doc, withMark);
    bool markAbove = false;
    for (const auto& g : l.glyphs)
        markAbove = markAbove || g.origin.y > 0;
    // The shaper may either offset the mark glyph or select a raised variant; either way the
    // outlines reach higher than the plain base glyphs (checked above).
    (void)markAbove;
    CHECK(l.box.max.y > 10);  // marks rise above the capital height
}

TEST_CASE("below 3 px the text becomes a box placeholder per line") {
    Document doc;
    MTextData m;
    m.height = 2;
    m.utf8 = "one\\Ptwo";
    Entity e{{}, m};
    DisplayList small = build(doc, e, /*pixelMm=*/1.0);  // 2 mm = 2 px
    REQUIRE(small.items.size() == 2);
    for (const auto& it : small.items) {
        const auto* pl = std::get_if<DisplayPolyline>(&it);
        REQUIRE(pl);
        CHECK(pl->closed);
        CHECK(pl->pts.size() == 4);
    }
    DisplayList big = build(doc, e, 0.01);
    for (const auto& it : big.items)
        CHECK(std::holds_alternative<DisplayFill>(it));
}

TEST_CASE("colour override and layer colour reach the fill style") {
    Document doc;
    MTextData m;
    m.height = 5;
    m.utf8 = "{\\C1;A}B";
    Entity e{{}, m};
    e.common.color = Color::rgb(0, 0, 255);
    DisplayList l = build(doc, e);
    REQUIRE(l.items.size() == 2);
    CHECK(std::get<DisplayFill>(l.items[0]).style.color == paletteColor(1));
    CHECK(std::get<DisplayFill>(l.items[1]).style.color == Rgba{0, 0, 255, 255});
}

TEST_CASE("bounds come from the layout and include Thai marks") {
    Document doc;
    Entity e{{}, thai("ห้อง")};
    BBox2 b = geom::bounds(e, doc);
    REQUIRE_FALSE(b.empty());
    CHECK(b.max.y > 10);
    CHECK(b.width() > 10);
}

TEST_CASE("hit testing uses the transformed layout box") {
    Document doc;
    TextData t = thai("TBCAD");
    t.position = t.alignPoint = {100, 100};
    t.rotation = kPi / 2;  // runs upward
    Entity e{{}, t};
    TextLayout l = layoutEntity(doc, e);
    REQUIRE_FALSE(l.empty());
    double w = l.lines[0].width;
    CHECK(hitTest(doc, e, {95, 100 + w / 2}, 0));       // over the text
    CHECK_FALSE(hitTest(doc, e, {120, 100 + w / 2}, 0));
    CHECK(hitTest(doc, e, {103, 100 + w / 2}, 3));      // below the baseline, within the aperture
    CHECK_FALSE(geom::hitTest(e, {500, 500}, 0.5, &doc));
}

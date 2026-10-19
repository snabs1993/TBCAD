#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/mline/mline.h"

using namespace tbcad;
using namespace tbcad::mline;

namespace {
MlineData straight() {
    MlineData d;
    d.v = {{0, 0}, {10, 0}};
    return d;
}
}  // namespace

TEST_CASE("the standard style gives two lines half a unit either side of the path") {
    Document doc;
    Strokes s = strokes(doc, straight());
    REQUIRE(s.lines.size() == 2);
    CHECK(s.lines[0][0].y == doctest::Approx(0.5));
    CHECK(s.lines[1][0].y == doctest::Approx(-0.5));
    CHECK(s.lines[0][1].x == doctest::Approx(10));
}

TEST_CASE("scale and justification move the lines") {
    Document doc;
    MlineData d = straight();
    d.scale = 4;
    Strokes zero = strokes(doc, d);
    CHECK(zero.lines[0][0].y == doctest::Approx(2));
    d.justification = 0;  // top: the highest line sits on the path
    Strokes top = strokes(doc, d);
    CHECK(top.lines[0][0].y == doctest::Approx(0));
    CHECK(top.lines[1][0].y == doctest::Approx(-4));
    d.justification = 2;  // bottom
    Strokes bottom = strokes(doc, d);
    CHECK(bottom.lines[1][0].y == doctest::Approx(0));
    CHECK(bottom.lines[0][0].y == doctest::Approx(4));
}

TEST_CASE("a right-angle corner is mitred") {
    Document doc;
    MlineData d;
    d.v = {{0, 0}, {10, 0}, {10, 10}};
    Strokes s = strokes(doc, d);
    REQUIRE(s.lines.size() == 2);
    // Left line: (0,.5) -> (9.5,.5) -> (9.5,10); right line: (0,-.5) -> (10.5,-.5) -> (10.5,10).
    CHECK(s.lines[0][1].x == doctest::Approx(9.5));
    CHECK(s.lines[0][1].y == doctest::Approx(0.5));
    CHECK(s.lines[1][1].x == doctest::Approx(10.5));
    CHECK(s.lines[1][1].y == doctest::Approx(-0.5));
}

TEST_CASE("a closed multiline wraps its corners - repeated points and short paths are tolerated") {
    Document doc;
    MlineData d;
    d.closed = true;
    d.v = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}};  // the last point repeats the first
    Strokes s = strokes(doc, d);
    REQUIRE(s.lines.size() == 2);
    CHECK(s.closed[0]);
    CHECK(s.lines[0].size() == 4);
    CHECK(s.lines[0][0].x == doctest::Approx(0.5));  // inner corner of the left line
    CHECK(s.lines[0][0].y == doctest::Approx(0.5));
    MlineData one;
    one.v = {{1, 1}, {1, 1}};
    CHECK(strokes(doc, one).lines.empty());
}

TEST_CASE("styles are stored in the header, found case-insensitively, and can have caps") {
    Document doc;
    CHECK(findStyle(doc, "standard").has_value());
    CHECK_FALSE(findStyle(doc, "Wall").has_value());
    Style wall;
    wall.name = "Wall";
    wall.offsets = {1.5, 0.5, -0.5, -1.5};
    wall.startCap = wall.endCap = true;
    wall.description = "Four lines, capped";
    {
        auto tx = doc.begin("style");
        REQUIRE(tx.modifyHeader([&](Header& h) { storeStyle(h, wall); }).ok());
        tx.commit();
    }
    auto found = findStyle(doc, "WALL");
    REQUIRE(found);
    CHECK(found->offsets.size() == 4);
    CHECK(found->description == "Four lines, capped");
    CHECK(found->startCap);
    CHECK(styleNames(doc).size() == 2);
    MlineData d = straight();
    d.style = "wall";
    Strokes s = strokes(doc, d);
    CHECK(s.lines.size() == 4 + 2);  // lines plus two caps
    CHECK(s.lines[4].size() == 2);
    CHECK(distance(s.lines[4][0], s.lines[4][1]) == doctest::Approx(3));
    {
        auto tx = doc.begin("remove");
        REQUIRE(tx.modifyHeader([&](Header& h) { CHECK(removeStyle(h, "Wall")); }).ok());
        tx.commit();
    }
    CHECK_FALSE(findStyle(doc, "Wall").has_value());
}

TEST_CASE("serialize and parse round-trip a multiline with its style") {
    MlineData d;
    d.style = "Wall";
    d.scale = 2.5;
    d.justification = 2;
    d.closed = true;
    d.v = {{0, 0}, {1.25, 3}, {-4, 5.5}};
    Style s;
    s.name = "Wall";
    s.offsets = {1, -1, 0};
    s.endCap = true;
    auto back = parse(serialize(d, s));
    REQUIRE(back);
    CHECK(back->data.style == "Wall");
    CHECK(back->data.scale == 2.5);
    CHECK(back->data.justification == 2);
    CHECK(back->data.closed);
    REQUIRE(back->data.v.size() == 3);
    CHECK(back->data.v[2].x == -4);
    CHECK(back->style.offsets == std::vector<double>{1, -1, 0});
    CHECK(back->style.endCap);
    CHECK_FALSE(back->style.startCap);
    CHECK_FALSE(parse("nonsense").has_value());
}

TEST_CASE("explode gives plain polylines that keep the properties") {
    Document doc;
    Entity e;
    e.data = Indirect<MlineData>(straight());
    e.common.color = Color::index(3);
    auto parts = explodeMline(doc, e);
    REQUIRE(parts);
    REQUIRE(parts.value().size() == 2);
    CHECK(parts.value()[0].common.color == Color::index(3));
    CHECK(kind(parts.value()[0]) == EntityKind::Polyline);
}

TEST_CASE("random paths, styles and cuts never give a non-finite or runaway point") {
    Document doc;
    std::uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
    };
    for (int round = 0; round < 3000; ++round) {
        MlineData d;
        const int n = 1 + static_cast<int>(rnd() * 8);
        for (int i = 0; i < n; ++i) {
            Vec2 p{std::floor(rnd() * 6) * 10,
                   std::floor(rnd() * 6) * 10};  // many repeats/collinear
            d.v.push_back(p);
            if (rnd() < 0.2)
                d.v.push_back(p);
        }
        d.closed = rnd() < 0.4;
        d.scale = rnd() < 0.1 ? 0 : 0.1 + rnd() * 5;
        d.justification = static_cast<int>(rnd() * 3);
        const int cuts = static_cast<int>(rnd() * 4);
        for (int c = 0; c < cuts; ++c) {
            MlineCut cut;
            cut.seg = static_cast<std::uint32_t>(rnd() * 12);
            cut.t0 = rnd() * 1.4 - 0.2;
            cut.t1 = rnd() * 1.4 - 0.2;
            cut.lo = static_cast<std::uint8_t>(rnd() * 4);
            cut.hi = static_cast<std::uint8_t>(rnd() * 6);
            d.cuts.push_back(cut);
        }
        Style st;
        st.offsets.clear();
        const int lines = 1 + static_cast<int>(rnd() * 5);
        for (int i = 0; i < lines; ++i)
            st.offsets.push_back(rnd() * 4 - 2);
        st.startCap = rnd() < 0.5;
        st.endCap = rnd() < 0.5;
        st.joints = rnd() < 0.5;
        st.fill = rnd() < 0.5 ? 3 : -1;
        const Strokes s = strokes(d, st);
        REQUIRE(s.lines.size() == s.closed.size());
        REQUIRE(s.lines.size() == s.element.size());
        for (const auto& line : s.lines)
            for (const Vec2& p : line) {
                REQUIRE(std::isfinite(p.x));
                REQUIRE(std::isfinite(p.y));
                REQUIRE(std::abs(p.x) < 5000);
                REQUIRE(std::abs(p.y) < 5000);
            }
        for (const auto& ring : s.fill)
            for (const Vec2& p : ring)
                REQUIRE(std::isfinite(p.x));
    }
}

TEST_CASE("a cut stays on its segment when repeated points are dropped") {
    MlineData d;
    d.v = {{0, 0}, {0, 0}, {10, 0}, {20, 0}};  // segment 0 is empty; segment 2 is 10..20
    MlineCut c;
    c.seg = 2;
    c.t0 = 0.25;
    c.t1 = 0.75;
    d.cuts.push_back(c);
    const Strokes s = strokes(d, standardStyle());
    // each of the two lines is in two pieces: 0..12.5 and 17.5..20
    REQUIRE(s.lines.size() == 4);
    CHECK(s.lines[0].back().x == doctest::Approx(12.5));
    CHECK(s.lines[1].front().x == doctest::Approx(17.5));
}

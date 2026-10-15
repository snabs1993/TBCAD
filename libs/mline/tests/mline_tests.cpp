#include <doctest/doctest.h>

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

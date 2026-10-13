#include "support.h"
#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::dim;
using namespace tbcad::dim::test;

namespace {

const Layout::Line* findExt(const Layout& L, Vec2 from) {
    for (const auto& l : L.lines)
        if (l.role == Role::ExtLine && near(l.pts.front(), from))
            return &l;
    return nullptr;
}

}  // namespace

TEST_CASE(
    "horizontal linear dimension: extension lines, dimension line, arrows inside, text above") {
    Document doc;
    Layout L = layoutDimension(doc, linear({0, 0}, {100, 0}, {50, 10}));
    CHECK(count(L, Role::ExtLine) == 2);
    const auto* e1 = findExt(L, {0, 0.625});
    REQUIRE(e1);
    CHECK(near(e1->pts.back(), {0, 11.25}));
    const auto* e2 = findExt(L, {100, 0.625});
    REQUIRE(e2);
    CHECK(near(e2->pts.back(), {100, 11.25}));

    // One dimension line from end to end, at y = 10; two arrows whose tips touch its ends.
    REQUIRE(count(L, Role::DimLine) == 1);
    for (const auto& l : L.lines)
        if (l.role == Role::DimLine) {
            CHECK(near(l.pts.front(), {0, 10}));
            CHECK(near(l.pts.back(), {100, 10}));
        }
    REQUIRE(L.arrows.size() == 2);
    BBox2 a = arrowBox(L.arrows[0]), b = arrowBox(L.arrows[1]);
    CHECK(a.min.x == doctest::Approx(0));
    CHECK(a.max.x == doctest::Approx(2.5));
    CHECK(b.max.x == doctest::Approx(100));
    CHECK(b.min.x == doctest::Approx(97.5));

    REQUIRE(L.text.valid());
    CHECK(L.text.utf8 == "100.00");
    CHECK(L.text.center.x == doctest::Approx(50));
    CHECK(L.text.center.y == doctest::Approx(10 + 0.625 + 1.25));  // gap + half the text height
    CHECK(L.text.rotation == doctest::Approx(0));
}

TEST_CASE("centered text breaks the dimension line") {
    Document doc;
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard,
                              [](DimStyle& s) { s.textPlacement = DimTextPlacement::Centered; }));
    tx.commit();
    Layout L = layoutDimension(doc, linear({0, 0}, {100, 0}, {50, 10}));
    CHECK(count(L, Role::DimLine) == 2);
    CHECK(L.text.center.y == doctest::Approx(10));
    for (const auto& l : L.lines)
        if (l.role == Role::DimLine) {
            double lo = std::min(l.pts.front().x, l.pts.back().x);
            double hi = std::max(l.pts.front().x, l.pts.back().x);
            CHECK((hi <= 50 - L.text.width / 2 + 1e-9 || lo >= 50 + L.text.width / 2 - 1e-9));
        }
}

TEST_CASE("fit: arrows move outside, then text, when the extension lines are close") {
    Document doc;
    // Text fits between the lines but the arrows do not fit beside it.
    const double tw = text::measureWidth(text::styleFor(doc, {}), "0.00", 2.5);
    const double mid_len = tw + 2.5 + 0.1;  // > tw + 2 * gap, < tw + 2 * gap + 2 * arrow
    Layout mid = layoutDimension(doc, linear({0, 0}, {mid_len, 0}, {mid_len / 2, 10}));
    REQUIRE(mid.arrows.size() == 2);
    CHECK(mid.text.center.x == doctest::Approx(mid_len / 2));  // text inside
    CHECK(arrowBox(mid.arrows[0]).max.x <= 1e-9);              // arrow at the left end sits outside
    CHECK(arrowBox(mid.arrows[1]).min.x >= mid_len - 1e-9);

    // 3 mm: neither fits; arrows outside and the text beyond the right-hand arrow.
    Layout small = layoutDimension(doc, linear({0, 0}, {3, 0}, {1.5, 10}));
    CHECK(arrowBox(small.arrows[0]).max.x <= 1e-9);
    CHECK(small.text.center.x - small.text.width / 2 > 3);

    // 6 mm: arrows fit inside, the text does not and goes outside.
    Layout arrows = layoutDimension(doc, linear({0, 0}, {6, 0}, {3, 10}));
    CHECK(arrowBox(arrows.arrows[0]).min.x >= -1e-9);
    CHECK(arrowBox(arrows.arrows[0]).max.x <= 6.0);
    CHECK(arrows.text.center.x - arrows.text.width / 2 > 6);
}

TEST_CASE("vertical and aligned dimensions") {
    Document doc;
    Layout v = layoutDimension(doc, linear({0, 0}, {0, 50}, {10, 25}, kPi / 2));
    CHECK(v.text.utf8 == "50.00");
    CHECK(v.text.rotation == doctest::Approx(kPi / 2));  // reads bottom to top

    Layout al = layoutDimension(doc, aligned({0, 0}, {30, 40}, {-24, 18}));
    CHECK(al.text.utf8 == "50.00");
    CHECK(al.text.rotation == doctest::Approx(std::atan2(40, 30)));
    // Dimension line is parallel to the points and passes the picked location.
    for (const auto& l : al.lines)
        if (l.role == Role::DimLine) {
            Vec2 dir = normalized(l.pts.back() - l.pts.front());
            CHECK(std::abs(cross(dir, Vec2{0.6, 0.8})) < 1e-9);
        }
}

TEST_CASE("text stays readable for dimension lines pointing left, and can stay horizontal") {
    Document doc;
    Layout l = layoutDimension(doc, linear({100, 0}, {0, 0}, {50, 10}));
    CHECK(l.text.rotation == doctest::Approx(0));
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard,
                              [](DimStyle& s) { s.textAlign = DimTextAlign::Horizontal; }));
    tx.commit();
    Layout v = layoutDimension(doc, linear({0, 0}, {0, 50}, {10, 25}, kPi / 2));
    CHECK(v.text.rotation == doctest::Approx(0));
    CHECK(count(v, Role::DimLine) == 2);  // horizontal text on a vertical line breaks it
}

TEST_CASE("radius and diameter") {
    Document doc;
    Layout r = layoutDimension(doc, radial(DimKind::Radius, {0, 0}, {10, 0}));
    CHECK(r.text.utf8 == "R10.00");
    CHECK(r.text.center.x > 10);
    REQUIRE(r.arrows.size() == 1);
    CHECK(arrowBox(r.arrows[0]).max.x == doctest::Approx(10));

    Layout d = layoutDimension(doc, radial(DimKind::Diameter, {-20, 0}, {20, 0}));
    CHECK(d.text.utf8 ==
          "\xC3\x98"
          "40.00");
    CHECK(std::abs(d.text.center.x) < 1e-9);  // inside, in the middle
    CHECK(d.arrows.size() == 2);

    Layout small = layoutDimension(doc, radial(DimKind::Diameter, {-3, 0}, {3, 0}));
    CHECK(small.text.center.x > 3);  // does not fit: outside
}

TEST_CASE("angular dimension: arc, extension lines, arrows, text on the arc") {
    Document doc;
    DimensionData d = angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {15, 15});
    Layout L = layoutDimension(doc, d);
    REQUIRE(L.arcs.size() == 1);
    const double r = std::sqrt(450.0);
    CHECK(L.arcs[0].radius == doctest::Approx(r));
    CHECK(L.arcs[0].startAngle == doctest::Approx(0));
    CHECK(L.arcs[0].endAngle == doctest::Approx(kPi / 2));
    CHECK(count(L, Role::ExtLine) == 2);
    CHECK(L.arrows.size() == 2);
    CHECK(L.text.utf8 == "90.00\xC2\xB0");
    CHECK(L.text.center.x > 0);
    CHECK(L.text.center.y > 0);

    // The arc is inside the lines: no extension lines.
    DimensionData inside = angular2({0, 0}, {20, 0}, {0, 0}, {0, 20}, {5, 5});
    CHECK(count(layoutDimension(doc, inside), Role::ExtLine) == 0);
}

TEST_CASE("DIMSCALE scales text and arrows but not the measurement") {
    Document doc;
    DimensionData d = linear({0, 0}, {10000, 0}, {5000, 1000});
    Layout one = layoutDimension(doc, d);
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) { s.overallScale = 100; }));
    tx.commit();
    Layout big = layoutDimension(doc, d);
    CHECK(big.text.utf8 == one.text.utf8);
    CHECK(big.text.utf8 == "10000.00");
    CHECK(big.text.height == doctest::Approx(one.text.height * 100));
    BBox2 a1 = arrowBox(one.arrows[0]), a2 = arrowBox(big.arrows[0]);
    CHECK(a2.width() == doctest::Approx(a1.width() * 100));
    CHECK(measurement(d) == doctest::Approx(10000));
}

TEST_CASE("arrow types") {
    Document doc;
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    auto with = [&](ArrowType t) {
        Transaction tx = doc.begin("style");
        REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& s) { s.arrowType = t; }));
        tx.commit();
        return layoutDimension(doc, d);
    };
    CHECK(with(ArrowType::ClosedFilled).arrows.size() == 2);
    Layout open = with(ArrowType::Open30);
    CHECK(open.arrows.empty());
    CHECK(open.lines.size() == 5);  // 2 extension lines, dimension line, 2 arrowheads
    Layout tick = with(ArrowType::ArchTick);
    CHECK(tick.arrows.empty());
    CHECK(tick.lines.size() == 5);
    CHECK(with(ArrowType::Dot).arrows.size() == 2);
    Layout none = with(ArrowType::None);
    CHECK(none.arrows.empty());
    CHECK(none.lines.size() == 3);
}

TEST_CASE("user-placed text outside the extension lines gets a short leader") {
    Document doc;
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    d.textPosUser = true;
    d.textPos = {140, 10};
    Layout L = layoutDimension(doc, d);
    CHECK(L.text.center.x == doctest::Approx(140));
    bool leader = false;
    for (const auto& l : L.lines)
        if (l.role == Role::DimLine && l.pts.back().x > 100)
            leader = true;
    CHECK(leader);
}

TEST_CASE("leader: arrow at the first vertex, landing, text") {
    Document doc;
    LeaderData ld;
    ld.vertices = {{0, 0}, {10, 10}, {20, 10}};
    ld.textPos = {23, 10};
    ld.utf8 = "Note";
    Layout L = layoutLeader(doc, ld);
    REQUIRE(L.arrows.size() == 1);
    CHECK(std::abs(arrowBox(L.arrows[0]).min.x) < 1e-9);
    CHECK(L.lines.size() == 2);  // the polyline and the landing
    CHECK(near(L.lines[1].pts.front(), {20, 10}));
    CHECK(near(L.lines[1].pts.back(), {22.5, 10}));
    REQUIRE(L.text.valid());
    CHECK(L.text.mtext);
    CHECK(L.text.utf8 == "Note");
    CHECK(near(L.text.center, {23, 10}));
    CHECK(!L.text.attachRight);

    ld.textPos = {-3, 10};
    CHECK(layoutLeader(doc, ld).text.attachRight);
    LeaderData tiny;
    tiny.vertices = {{0, 0}};
    CHECK(layoutLeader(doc, tiny).lines.empty());
}

TEST_CASE("refresh recomputes the measurement and the default text position") {
    Document doc;
    DimensionData d = linear({0, 0}, {100, 0}, {50, 10});
    refresh(doc, d);
    CHECK(d.measurement == doctest::Approx(100));
    CHECK(near(d.textPos, {50, 11.875}));
    d.textPosUser = true;
    d.textPos = {1, 2};
    d.defPts[1] = {200, 0};
    refresh(doc, d);
    CHECK(d.measurement == doctest::Approx(200));
    CHECK(near(d.textPos, {1, 2}));
}

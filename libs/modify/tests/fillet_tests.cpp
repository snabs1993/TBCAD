#include "test_support.h"

using namespace tbcad;
using namespace tbcad::modify;
using namespace tbcad::testing;

namespace {
FilletOptions fr(double r, bool trim = true) {
    FilletOptions o;
    o.radius = r;
    o.trim = trim;
    return o;
}
}  // namespace

TEST_CASE("fillet two perpendicular lines R=5") {
    Entity a = line({0, 0}, {10, 0}), b = line({0, 0}, {0, 10});
    auto r = filletEntities(a, {8, 0}, b, {0, 8}, fr(5));
    REQUIRE(r.ok());
    REQUIRE(r.value().modifiedA.has_value());
    REQUIRE(r.value().modifiedB.has_value());
    REQUIRE(r.value().added.size() == 1);
    const auto& la = as<LineData>(*r.value().modifiedA);
    CHECK(near(la.a, {5, 0}));
    CHECK(near(la.b, {10, 0}));
    const auto& lb = as<LineData>(*r.value().modifiedB);
    CHECK(near(lb.a, {0, 5}));
    CHECK(near(lb.b, {0, 10}));
    const auto& arc = as<ArcData>(r.value().added[0]);
    CHECK(near(arc.center, {5, 5}));
    CHECK(near(arc.radius, 5));
    CHECK(near(arc.startAngle, kPi));
    CHECK(near(arc.endAngle, 3 * kPi / 2));
}

TEST_CASE("fillet picks the solution on the side of the pick points") {
    Entity a = line({-10, 0}, {10, 0}), b = line({0, -10}, {0, 10});
    auto r = filletEntities(a, {-8, 0}, b, {0, 8}, fr(2));  // second quadrant
    REQUIRE(r.ok());
    const auto& arc = as<ArcData>(r.value().added[0]);
    CHECK(near(arc.center, {-2, 2}));
    // The kept parts are the ones holding the picks.
    const auto& la = as<LineData>(*r.value().modifiedA);
    CHECK(near(la.a, {-10, 0}));
    CHECK(near(la.b, {-2, 0}));
    const auto& lb = as<LineData>(*r.value().modifiedB);
    CHECK(near(lb.a, {0, 2}));
    CHECK(near(lb.b, {0, 10}));
}

TEST_CASE("fillet without trim only adds the arc; radius 0 makes a sharp corner") {
    Entity a = line({0, 0}, {10, 0}), b = line({0, 0}, {0, 10});
    auto nt = filletEntities(a, {8, 0}, b, {0, 8}, fr(5, false));
    REQUIRE(nt.ok());
    CHECK_FALSE(nt.value().modifiedA.has_value());
    CHECK_FALSE(nt.value().modifiedB.has_value());
    CHECK(nt.value().added.size() == 1);

    Entity c = line({0, 0}, {4, 0}), d = line({6, -3}, {6, 5});
    auto sharp = filletEntities(c, {3, 0}, d, {6, 3}, fr(0));
    REQUIRE(sharp.ok());
    CHECK(sharp.value().added.empty());
    CHECK(near(as<LineData>(*sharp.value().modifiedA).b, {6, 0}));
    CHECK(near(as<LineData>(*sharp.value().modifiedA).a, {0, 0}));
    CHECK(near(as<LineData>(*sharp.value().modifiedB).a, {6, 0}));
    CHECK(near(as<LineData>(*sharp.value().modifiedB).b, {6, 5}));
}

TEST_CASE("fillet parallel lines gives a semicircle") {
    Entity a = line({0, 0}, {10, 0}), b = line({0, 4}, {10, 4});
    auto r = filletEntities(a, {9, 0}, b, {9, 4}, fr(1));
    REQUIRE(r.ok());
    REQUIRE(r.value().added.size() == 1);
    const auto& arc = as<ArcData>(r.value().added[0]);
    CHECK(near(arc.center, {10, 2}));
    CHECK(near(arc.radius, 2));
    CHECK(near(arc.startAngle, 3 * kPi / 2));
    CHECK(near(arc.endAngle, kPi / 2));
    CHECK(near(as<LineData>(*r.value().modifiedB).b, {10, 4}));
    // Collinear lines have no corner.
    CHECK_FALSE(filletEntities(a, {9, 0}, line({12, 0}, {20, 0}), {13, 0}, fr(1)).ok());
}

TEST_CASE("fillet line and arc, arc and arc: tangent to both") {
    Entity l = line({-20, 0}, {20, 0});
    Entity c = arc({0, 8}, 5, 0, kTwoPi - 1e-3);  // nearly full circle above the line
    auto r = filletEntities(l, {-15, 0}, c, {-5, 8}, fr(3));
    REQUIRE(r.ok());
    REQUIRE(r.value().added.size() == 1);
    const auto& f = as<ArcData>(r.value().added[0]);
    CHECK(near(f.radius, 3));
    // Tangent to the line: centre at height R. Tangent to the circle: centres R + r apart or |R - r|.
    CHECK(near(std::abs(f.center.y), 3, 1e-6));
    double dc = distance(f.center, {0, 8});
    CHECK((near(dc, 8, 1e-6) || near(dc, 2, 1e-6)));
    CHECK(f.center.x < 0);

    Entity c1 = circle({0, 0}, 5), c2 = circle({12, 0}, 5);
    auto cc = filletEntities(c1, {5, 1}, c2, {7, 1}, fr(2));
    REQUIRE(cc.ok());
    const auto& f2 = as<ArcData>(cc.value().added[0]);
    CHECK(near(distance(f2.center, {0, 0}), 7, 1e-6));
    CHECK(near(distance(f2.center, {12, 0}), 7, 1e-6));
    bool changedCircle = cc.value().modifiedA.has_value();
    CHECK_FALSE(changedCircle);  // circles are never trimmed
}

TEST_CASE("fillet unsupported input and impossible radius") {
    Entity t = make(TextData{});
    CHECK_FALSE(filletEntities(t, {0, 0}, line({0, 0}, {1, 0}), {0, 0}, fr(1)).ok());
    Entity a = line({0, 0}, {10, 0}), b = line({0, 0}, {0, 10});
    auto big = filletEntities(a, {1, 0}, b, {0, 1}, fr(50));
    CHECK(big.ok());  // extends: an oversize fillet moves the tangent points past the ends
}

TEST_CASE("polyline vertex and whole-polyline fillet") {
    Entity rect = poly(pts({{0, 0}, {10, 0}, {10, 6}, {0, 6}}), true);
    auto one = filletPolylineVertex(rect, 1, 2);
    REQUIRE(one.ok());
    const auto& pl = as<PolylineData>(one.value());
    CHECK(pl.closed);
    CHECK(pl.v.size() == 5);
    CHECK(near(geom::length(pl), 32 - 4 + kPi, 1e-6));
    CHECK(near(geom::area(pl), 60 - (4 - kPi), 1e-6));

    auto all = filletPolyline(rect, 2);
    REQUIRE(all.ok());
    CHECK(as<PolylineData>(all.value()).v.size() == 8);
    CHECK(near(geom::length(as<PolylineData>(all.value())), 32 - 16 + 2 * kPi * 2, 1e-6));

    // A radius that does not fit one corner is an error; whole-polyline skips what does not fit.
    CHECK_FALSE(filletPolylineVertex(rect, 1, 7).ok());
    auto fits = filletPolyline(rect, 2.9);  // the 6 mm sides just hold two fillets
    REQUIRE(fits.ok());
    CHECK(as<PolylineData>(fits.value()).v.size() == 8);
    auto partly = filletPolyline(rect, 3.5);  // neighbours would overlap on the short sides
    REQUIRE(partly.ok());
    CHECK(as<PolylineData>(partly.value()).v.size() < 8);
    CHECK_FALSE(filletPolyline(rect, 50).ok());

    // Open polylines have no corner at their ends.
    Entity open = poly(pts({{0, 0}, {10, 0}, {10, 6}}), false);
    CHECK_FALSE(filletPolylineVertex(open, 0, 1).ok());
    auto mid = filletPolylineVertex(open, 1, 1);
    REQUIRE(mid.ok());
    CHECK(as<PolylineData>(mid.value()).v.size() == 4);
    CHECK(near(distance(verts(mid.value())[1], {9, 0}), 0));
    // Radius 0 leaves the polyline as it is.
    auto zero = filletPolyline(open, 0);
    REQUIRE(zero.ok());
    CHECK(as<PolylineData>(zero.value()).v.size() == 3);
}

TEST_CASE("segment helpers find the corner between two picks") {
    Entity rect = poly(pts({{0, 0}, {10, 0}, {10, 6}, {0, 6}}), true);
    const auto& pl = as<PolylineData>(rect);
    std::size_t s0 = nearestSegmentIndex(pl, {5, 0.1});
    std::size_t s1 = nearestSegmentIndex(pl, {9.9, 3});
    CHECK(s0 == 0);
    CHECK(s1 == 1);
    CHECK(sharedVertex(pl, s0, s1) == std::optional<std::size_t>{1});
    CHECK(sharedVertex(pl, 3, 0) == std::optional<std::size_t>{0});
    CHECK_FALSE(sharedVertex(pl, 0, 2).has_value());
}

TEST_CASE("chamfer 5 x 5 on two perpendicular lines") {
    Entity a = line({0, 0}, {20, 0}), b = line({0, 0}, {0, 20});
    ChamferOptions o;
    o.distance1 = 5;
    o.distance2 = 5;
    auto r = chamferEntities(a, {15, 0}, b, {0, 15}, o);
    REQUIRE(r.ok());
    REQUIRE(r.value().added.size() == 1);
    const auto& c = as<LineData>(r.value().added[0]);
    CHECK(near(c.a, {5, 0}));
    CHECK(near(c.b, {0, 5}));
    CHECK(near(as<LineData>(*r.value().modifiedA).a, {5, 0}));
    CHECK(near(as<LineData>(*r.value().modifiedB).a, {0, 5}));

    // Different distances: the first applies to the first line.
    o.distance1 = 3;
    o.distance2 = 6;
    auto r2 = chamferEntities(a, {15, 0}, b, {0, 15}, o);
    CHECK(near(as<LineData>(r2.value().added[0]).a, {3, 0}));
    CHECK(near(as<LineData>(r2.value().added[0]).b, {0, 6}));

    // Distance-angle: 5 along the first line at 45 degrees on a right angle gives 5 on the second.
    ChamferOptions ang;
    ang.byAngle = true;
    ang.distance1 = 5;
    ang.angle = kPi / 4;
    auto r3 = chamferEntities(a, {15, 0}, b, {0, 15}, ang);
    REQUIRE(r3.ok());
    CHECK(near(as<LineData>(r3.value().added[0]).b, {0, 5}));

    // No-trim, and parallel lines.
    o.trim = false;
    auto r4 = chamferEntities(a, {15, 0}, b, {0, 15}, o);
    CHECK_FALSE(r4.value().modifiedA.has_value());
    CHECK_FALSE(chamferEntities(a, {15, 0}, line({0, 3}, {20, 3}), {15, 3}, o).ok());
}

TEST_CASE("chamfer a polyline corner and all corners") {
    Entity rect = poly(pts({{0, 0}, {10, 0}, {10, 6}, {0, 6}}), true);
    ChamferOptions o;
    o.distance1 = 2;
    o.distance2 = 2;
    auto one = chamferPolylineVertex(rect, 2, o);
    REQUIRE(one.ok());
    CHECK(sameVertexSet(verts(one.value()),
                        {{0, 0}, {10, 0}, {10, 4}, {8, 6}, {0, 6}}));
    auto all = chamferPolyline(rect, o);
    REQUIRE(all.ok());
    CHECK(as<PolylineData>(all.value()).v.size() == 8);
    o.distance1 = o.distance2 = 8;
    CHECK_FALSE(chamferPolylineVertex(rect, 2, o).ok());
}

#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {

Vec2 snapped(World& w, Vec2 cursor, const char* modes, std::optional<Vec2> base = std::nullopt) {
    w.set(keys::osnap, "1");
    w.set(keys::osnapModes, modes);
    return w.hover(cursor, base, 1.0);
}

EntityId addEllipse(World& w, Vec2 c, Vec2 major, double ratio) {
    EllipseData e;
    e.center = c;
    e.majorAxis = major;
    e.ratio = ratio;
    return w.add(Entity{{}, Indirect<EllipseData>(e)});
}

EntityId addSpline(World& w) {
    SplineData s;
    s.degree = 2;
    s.knots = {0, 0, 0, 1, 1, 1};
    s.control = {{0, 0}, {10, 20}, {20, 0}};
    return w.add(Entity{{}, Indirect<SplineData>(s)});
}

EntityId addBulged(World& w) {
    PolylineData p;
    p.v = {{{0, 0}, 1, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 10}, 0, 0, 0}};
    return w.add(Entity{{}, p});
}

}  // namespace

TEST_CASE("ellipse: centre, quadrants and nearest") {
    World w;
    addEllipse(w, {0, 0}, {40, 0}, 0.5);
    CHECK(near(snapped(w, {3, 2}, "center"), {0, 0}));
    CHECK(near(snapped(w, {38, 3}, "quadrant"), {40, 0}));
    CHECK(near(snapped(w, {-3, 18}, "quadrant"), {0, 20}));
    CHECK(near(snapped(w, {-40, -4}, "quadrant"), {-40, 0}));
    CHECK(near(snapped(w, {0, 24}, "nearest"), {0, 20}, 1e-3));
}

TEST_CASE("rotated ellipse: quadrants follow the major axis") {
    World w;
    addEllipse(w, {0, 0}, {0, 40}, 0.5);
    CHECK(near(snapped(w, {2, 38}, "quadrant"), {0, 40}));
    CHECK(near(snapped(w, {18, 3}, "quadrant"), {20, 0}));
    CHECK(near(snapped(w, {-18, 3}, "quadrant"), {-20, 0}));
}

TEST_CASE("ellipse arc: endpoints, midpoint and no quadrant outside the arc") {
    World w;
    EllipseData e;
    e.center = {0, 0};
    e.majorAxis = {40, 0};
    e.ratio = 0.5;
    e.startParam = 0;
    e.endParam = 3.14159265358979323846 / 2;
    w.add(Entity{{}, Indirect<EllipseData>(e)});
    CHECK(near(snapped(w, {38, 2}, "endpoint"), {40, 0}, 1e-3));
    CHECK(near(snapped(w, {2, 18}, "endpoint"), {0, 20}, 1e-3));
    CHECK(near(snapped(w, {-38, 2}, "quadrant"), {-38, 2}));  // lies on the missing half
}

TEST_CASE("spline: endpoints and nearest follow the curve, not the control polygon") {
    World w;
    addSpline(w);
    CHECK(near(snapped(w, {2, 2}, "endpoint"), {0, 0}));
    CHECK(near(snapped(w, {19, 3}, "endpoint"), {20, 0}));
    // B(0.5) = 0.25 P0 + 0.5 P1 + 0.25 P2 = (10, 10)
    CHECK(near(snapped(w, {10, 12}, "nearest"), {10, 10}, 1e-2));
    // The control point (10, 20) is not on the curve.
    CHECK(near(snapped(w, {10, 21}, "endpoint,midpoint,nearest,quadrant"), {10, 21}));
}

TEST_CASE("polyline arc segment: endpoint, arc midpoint and nearest") {
    World w;
    addBulged(w);
    // bulge 1 from (0,0) to (10,0): a half circle below the chord, centre (5,0), radius 5.
    CHECK(near(snapped(w, {1, 2}, "endpoint"), {0, 0}));
    CHECK(near(snapped(w, {5, -7}, "midpoint"), {5, -5}, 1e-6));
    CHECK(near(snapped(w, {5, -8}, "nearest"), {5, -5}, 1e-6));
    CHECK(near(snapped(w, {10, 5}, "midpoint"), {10, 5}));  // straight segment midpoint
}

TEST_CASE("perpendicular lands on an ellipse and on a spline") {
    World w;
    addEllipse(w, {0, 0}, {40, 0}, 0.5);
    CHECK(near(snapped(w, {0, 21}, "perpendicular", Vec2{0, 60}), {0, 20}, 1e-3));
    World s;
    addSpline(s);
    // From a point straight above the apex the foot is the apex (10, 10).
    CHECK(near(snapped(s, {10, 11}, "perpendicular", Vec2{10, 60}), {10, 10}, 1e-2));
}

TEST_CASE("tangent from an external point touches the ellipse") {
    World w;
    addEllipse(w, {0, 0}, {40, 0}, 0.5);
    const Vec2 base{80, 0};
    const Vec2 t = snapped(w, {18, 16}, "tangent", base);
    // Tangent points of an ellipse x^2/a^2 + y^2/b^2 = 1 from (80,0): x = a^2/80 = 20.
    CHECK(near(t, {20, 17.3205080757}, 5e-2));
}

TEST_CASE("tangent from an external point touches a spline where the chord is tangent") {
    World w;
    addSpline(w);
    // The curve is y = 2x - x^2/10; the tangent through (30, 0) touches it at x = 30 - sqrt(300).
    const double x0 = 30 - std::sqrt(300.0);
    const Vec2 t = snapped(w, {13, 8}, "tangent", Vec2{30, 0});
    CHECK(near(t, {x0, 2 * x0 - x0 * x0 / 10}, 1e-2));
}

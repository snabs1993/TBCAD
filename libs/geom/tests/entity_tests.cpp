#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::geom;

namespace {

Entity make(EntityData d) {
    Entity e;
    e.data = std::move(d);
    return e;
}

Entity stadium() {
    // 3 long, radius-1 semicircular ends, counter-clockwise.
    return make(PolylineData{{{{0, 0}, 0}, {{3, 0}, 1.0}, {{3, 2}, 0}, {{0, 2}, 1.0}}, true, 0});
}

Entity ellipseEntity() {
    EllipseData e;
    e.center = {1, 2};
    e.majorAxis = {4, 0};
    e.ratio = 0.5;
    return make(e);
}

Entity splineEntity() {
    return make(interpolateSpline(std::vector<Vec2>{{0, 0}, {2, 3}, {5, 3}, {7, 0}}).value());
}

}  // namespace

TEST_CASE("bounds of every built-in kind") {
    CHECK(bounds(make(LineData{{3, 4}, {-1, 2}})).min.x == -1);
    BBox2 c = bounds(make(CircleData{{1, 1}, 2}));
    CHECK(c.min.x == -1);
    CHECK(c.max.y == 3);
    BBox2 arc = bounds(make(ArcData{{0, 0}, 5, 0, kPi / 2}));
    CHECK(arc.min.x == doctest::Approx(0).epsilon(1e-9));
    CHECK(arc.max.x == doctest::Approx(5));
    CHECK(arc.max.y == doctest::Approx(5));
    BBox2 st = bounds(stadium());
    CHECK(st.min.x == doctest::Approx(-1));
    CHECK(st.max.x == doctest::Approx(4));
    CHECK(st.min.y == doctest::Approx(0));
    BBox2 el = bounds(ellipseEntity());
    CHECK(el.min.x == doctest::Approx(-3));
    CHECK(el.max.y == doctest::Approx(4));
    BBox2 sp = bounds(splineEntity());
    CHECK(sp.min.x <= 0);
    CHECK(sp.max.x >= 7);
    CHECK(sp.max.y >= 3);
    CHECK(bounds(make(PointData{{2, 3}})).min.y == 3);
    CHECK(bounds(make(TextData{})).empty());
}

TEST_CASE("distance, closest point and hit test") {
    Entity line = make(LineData{{0, 0}, {10, 0}});
    CHECK(distance(line, {5, 3}) == doctest::Approx(3));
    CHECK(distance(line, {-3, 4}) == doctest::Approx(5));
    CHECK(hitTest(line, {5, 0.4}, 0.5));
    CHECK_FALSE(hitTest(line, {5, 0.6}, 0.5));
    CHECK(closestPoint(line, {20, 7}).value().x == 10);

    Entity circle = make(CircleData{{0, 0}, 5});
    CHECK(distance(circle, {8, 0}) == doctest::Approx(3));
    CHECK(distance(circle, {0, 0}) == doctest::Approx(5));
    CHECK(distance(circle, {1, 0}) == doctest::Approx(4));

    Entity arc = make(ArcData{{0, 0}, 5, 0, kPi / 2});
    CHECK(distance(arc, {6, 6}) == doctest::Approx(std::sqrt(72.0) - 5));
    CHECK(distance(arc, {5, -2}) == doctest::Approx(2));  // nearest is the start end point
    CHECK(distance(arc, {-3, 5}) == doctest::Approx(3));  // nearest is the end point (0, 5)
    CHECK(distance(arc, {0, 0}) == doctest::Approx(5));

    Entity st = stadium();
    CHECK(distance(st, {1.5, 1}) == doctest::Approx(1));
    CHECK(distance(st, {-2, 1}) == doctest::Approx(1));  // outside the left semicircle
    CHECK(distance(st, {4, 1}) == doctest::Approx(0).epsilon(1e-9));
    CHECK(hitTest(st, {1.5, -0.2}, 0.3));

    Entity el = ellipseEntity();
    CHECK(distance(el, {1, 2}) == doctest::Approx(2));  // centre to the nearest vertex
    CHECK(distance(el, {9, 2}) == doctest::Approx(4).epsilon(1e-6));

    Entity sp = splineEntity();
    auto curves = std::vector<CurveRef>();
    REQUIRE(curvesOf(sp, curves));
    Vec2 onCurve = curves[0].pointAt(0.4 * (curves[0].t0() + curves[0].t1()) + 0.1);
    CHECK(distance(sp, onCurve) < 1e-9);
    CHECK(hitTest(sp, onCurve + Vec2{0.1, 0.1}, 0.5));

    Entity pt = make(PointData{{1, 1}});
    CHECK(distance(pt, {4, 5}) == doctest::Approx(5));
}

TEST_CASE("window and crossing selection") {
    BBox2 w;
    w.expand({0, 0});
    w.expand({10, 10});
    Entity inside = make(LineData{{1, 1}, {9, 9}});
    Entity crossing = make(LineData{{-5, 5}, {5, 5}});
    Entity outside = make(LineData{{20, 20}, {30, 30}});
    Entity miss = make(LineData{{-5, 20}, {20, -5}});  // passes the corner region without touching
    CHECK(insideWindow(inside, w));
    CHECK_FALSE(insideWindow(crossing, w));
    CHECK(crossesWindow(inside, w));
    CHECK(crossesWindow(crossing, w));
    CHECK_FALSE(crossesWindow(outside, w));
    // The diagonal x + y = 15 does cross the box.
    CHECK(crossesWindow(miss, w));
    Entity farDiag = make(LineData{{-5, 20}, {20, -5.0}});
    (void)farDiag;
    Entity reallyMiss = make(LineData{{8, 20}, {20, 8}});  // x + y = 28 > 20
    CHECK_FALSE(crossesWindow(reallyMiss, w));

    // Circle bigger than the window around it: window inside the circle, nothing touched.
    Entity big = make(CircleData{{5, 5}, 50});
    CHECK_FALSE(crossesWindow(big, w));
    Entity ring = make(CircleData{{5, 5}, 6});  // passes through the window
    CHECK(crossesWindow(ring, w));
    CHECK_FALSE(insideWindow(ring, w));
    Entity small = make(CircleData{{5, 5}, 2});
    CHECK(insideWindow(small, w));
    // Arc whose chord lies outside but curve enters the window.
    Entity arc = make(ArcData{{5, 5}, 6, kPi / 4, kPi / 2});
    CHECK(crossesWindow(arc, w));
    Entity polyOut = make(PolylineData{{{{20, 20}, 0}, {{30, 20}, 0}, {{30, 30}, 0}}, false, 0});
    CHECK_FALSE(crossesWindow(polyOut, w));
    Entity polyIn = make(PolylineData{{{{-5, 5}, 0}, {{5, 5}, 0}}, false, 0});
    CHECK(crossesWindow(polyIn, w));
    CHECK(crossesWindow(make(PointData{{5, 5}}), w));
    CHECK_FALSE(crossesWindow(make(PointData{{15, 5}}), w));
}

TEST_CASE("snap helpers") {
    Entity line = make(LineData{{0, 0}, {4, 2}});
    CHECK(endpoints(line).size() == 2);
    CHECK(distance(midpoints(line)[0], Vec2{2, 1}) < 1e-12);

    Entity circle = make(CircleData{{1, 1}, 2});
    CHECK(centers(circle)[0] == Vec2{1, 1});
    auto q = quadrants(circle);
    REQUIRE(q.size() == 4);
    CHECK(distance(q[1], Vec2{1, 3}) < 1e-12);
    CHECK(endpoints(circle).empty());

    Entity arc = make(ArcData{{0, 0}, 1, 0, kPi});
    auto aq = quadrants(arc);
    CHECK(aq.size() == 3);  // 0, 90, 180 degrees
    CHECK(distance(midpoints(arc)[0], Vec2{0, 1}) < 1e-12);
    auto ae = endpoints(arc);
    CHECK(distance(ae[0], Vec2{1, 0}) < 1e-12);
    CHECK(distance(ae[1], Vec2{-1, 0}) < 1e-12);

    Entity st = stadium();
    CHECK(endpoints(st).size() == 4);
    CHECK(midpoints(st).size() == 4);
    CHECK(centers(st).size() == 2);
    auto mp = midpoints(st);
    CHECK(distance(mp[0], Vec2{1.5, 0}) < 1e-12);
    // Segment 1 is the right semicircle: its midpoint is (4, 1).
    CHECK(distance(mp[1], Vec2{4, 1}) < 1e-9);

    Entity el = ellipseEntity();
    CHECK(quadrants(el).size() == 4);
    CHECK(centers(el)[0] == Vec2{1, 2});
}

TEST_CASE("polyline area and length with bulges") {
    // Stadium: 3*2 rectangle + circle of radius 1.
    PolylineData st = *entityAs<PolylineData>(stadium());
    CHECK(signedArea(st) == doctest::Approx(6 + kPi).epsilon(1e-12));
    CHECK(area(st) == doctest::Approx(6 + kPi).epsilon(1e-12));
    CHECK(length(st) == doctest::Approx(6 + 2 * kPi).epsilon(1e-12));
    // Clockwise winding gives a negative signed area.
    PolylineData cw;
    cw.closed = true;
    cw.v = {{{0, 0}, -1.0}, {{0, 2}, 0}, {{3, 2}, -1.0}, {{3, 0}, 0}};
    CHECK(signedArea(cw) < 0);
    CHECK(area(cw) == doctest::Approx(6 + kPi).epsilon(1e-9));
    // Plain square, triangle, and a polyline whose arc subtracts (negative bulge on a CCW loop).
    PolylineData sq;
    sq.closed = true;
    sq.v = {{{0, 0}, 0}, {{2, 0}, 0}, {{2, 2}, 0}, {{0, 2}, 0}};
    CHECK(area(sq) == doctest::Approx(4));
    PolylineData notch = sq;
    notch.v[0].bulge = -1.0;  // bottom edge bows inward (up) by a semicircle of radius 1
    CHECK(area(notch) == doctest::Approx(4 - kPi / 2).epsilon(1e-12));
    // Open polyline: length excludes the closing chord, area closes with a chord.
    PolylineData open;
    open.v = {{{0, 0}, 0}, {{4, 0}, 0}, {{4, 3}, 0}};
    CHECK(length(open) == doctest::Approx(7));
    CHECK(area(open) == doctest::Approx(6));
    CHECK(length(make(CircleData{{0, 0}, 1})) == doctest::Approx(kTwoPi));
    CHECK(length(make(LineData{{0, 0}, {3, 4}})) == doctest::Approx(5));
}

TEST_CASE("polyline parameter point") {
    PolylineData pl = *entityAs<PolylineData>(stadium());
    CHECK(distance(polylinePointAt(pl, 0.5), Vec2{1.5, 0}) < 1e-12);
    CHECK(distance(polylinePointAt(pl, 1.5), Vec2{4, 1}) < 1e-9);
    CHECK(distance(polylinePointAt(pl, 3.999), Vec2{0, 0.0}) < 1e-2);
    CHECK(distance(polylinePointAt(pl, -2), Vec2{0, 0}) < 1e-12);
    Entity e = make(pl);
    CHECK(distance(pointAtParam(e, 2.5).value(), Vec2{1.5, 2}) < 1e-9);
    CHECK_FALSE(pointAtParam(make(TextData{}), 0).has_value());
}

TEST_CASE("grip points and moving grips") {
    Entity line = make(LineData{{0, 0}, {4, 0}});
    auto g = gripPoints(line);
    REQUIRE(g.size() == 3);
    CHECK(g[2] == Vec2{2, 0});
    auto m = moveGrip(line, 2, {2, 5});
    REQUIRE(m.ok());
    CHECK(entityAs<LineData>(m.value())->a == Vec2{0, 5});
    CHECK(entityAs<LineData>(m.value())->b == Vec2{4, 5});
    CHECK(entityAs<LineData>(moveGrip(line, 1, {9, 9}).value())->b == Vec2{9, 9});
    CHECK_FALSE(moveGrip(line, 3, {0, 0}).ok());
    CHECK_FALSE(moveGrip(line, -1, {0, 0}).ok());

    Entity circle = make(CircleData{{0, 0}, 5});
    CHECK(gripPoints(circle).size() == 5);
    CHECK(entityAs<CircleData>(moveGrip(circle, 2, {0, 8}).value())->radius == doctest::Approx(8));
    CHECK(entityAs<CircleData>(moveGrip(circle, 0, {3, 3}).value())->center == Vec2{3, 3});
    CHECK_FALSE(moveGrip(circle, 1, {0, 0}).ok());

    Entity arc = make(ArcData{{0, 0}, 5, 0, kPi});
    auto ag = gripPoints(arc);
    REQUIRE(ag.size() == 4);
    CHECK(distance(ag[2], Vec2{0, 5}) < 1e-12);
    // Dragging the mid grip keeps the end points and passes through the new point.
    auto am = moveGrip(arc, 2, {0, 3});
    REQUIRE(am.ok());
    CHECK(distance(Vec2{entityAs<ArcData>(am.value())->center}, Vec2{0, 0}) >
          1);                                        // different circle
    CHECK(distance(am.value(), Vec2{0, 3}) < 1e-9);  // passes through it
    CHECK(distance(am.value(), Vec2{5, 0}) < 1e-9);
    CHECK(distance(am.value(), Vec2{-5, 0}) < 1e-9);
    // Collinear target is rejected.
    CHECK_FALSE(moveGrip(arc, 2, {0, 0}).ok());

    Entity st = stadium();
    auto sg = gripPoints(st);
    CHECK(sg.size() == 8);  // 4 vertices + 4 segment midpoints
    auto mv = moveGrip(st, 0, {-1, -1});
    CHECK(entityAs<PolylineData>(mv.value())->v[0].p == Vec2{-1, -1});
    // Dragging the midpoint grip of a straight segment moves that edge: both ends follow.
    const PolylineData& before = *entityAs<PolylineData>(st);
    auto bg = moveGrip(st, 4, {1.5, -0.5});
    REQUIRE(bg.ok());
    const PolylineData& after = *entityAs<PolylineData>(bg.value());
    CHECK(after.v[0].bulge == 0);
    CHECK(after.v[0].p.y == doctest::Approx(before.v[0].p.y - 0.5));
    CHECK(after.v[1].p.y == doctest::Approx(before.v[1].p.y - 0.5));
    CHECK(after.v[0].p.x == doctest::Approx(before.v[0].p.x));
    // The midpoint of a curved segment still reshapes the curve through the dragged point.
    auto curved = moveGrip(st, 5, gripPoints(st)[5]);
    REQUIRE(curved.ok());
    CHECK(entityAs<PolylineData>(curved.value())->v[1].bulge ==
          doctest::Approx(before.v[1].bulge).epsilon(1e-6));

    Entity el = ellipseEntity();
    CHECK(gripPoints(el).size() == 5);
    auto e1 = moveGrip(el, 1, {7, 2});
    REQUIRE(e1.ok());
    CHECK(entityAs<EllipseData>(e1.value())->majorAxis == Vec2{6, 0});
    auto e2 = moveGrip(el, 2, {1, 3});
    CHECK(entityAs<EllipseData>(e2.value())->ratio == doctest::Approx(0.25));
    CHECK_FALSE(moveGrip(el, 2, {1, 2}).ok());

    Entity sp = splineEntity();
    auto spg = gripPoints(sp);
    CHECK(spg.size() == entityAs<SplineData>(sp)->fit.size());
    auto spm = moveGrip(sp, 1, {2, 6});
    REQUIRE(spm.ok());
    CHECK(distance(spm.value(), Vec2{2, 6}) < 1e-6);  // still interpolates the moved fit point

    Entity text = make(TextData{});
    CHECK(gripPoints(text).size() == 1);
    DimensionData dd;
    dd.defPts = {Vec2{0, 0}, Vec2{1, 0}, Vec2{2, 0}, Vec2{3, 0}};
    Entity dim = make(dd);
    CHECK(gripPoints(dim).size() == 5);
    auto dm = moveGrip(dim, 4, {9, 9});
    CHECK(entityAs<DimensionData>(dm.value())->textPosUser);
    CHECK(gripPoints(make(HatchData{})).empty());
    CHECK_FALSE(moveGrip(make(HatchData{}), 0, {0, 0}).ok());
}

TEST_CASE("multileader: transform keeps its shape, grips move lines, landing and note") {
    MLeaderData m;
    m.lines = {{{0, 0}, {10, 5}}, {{0, 10}, {10, 5}}};
    m.landing = {10, 5};
    m.contentPos = {12.5, 5};
    m.content = MLeaderData::Content::Block;
    m.blockScale = 2;
    Entity e = make(Indirect<MLeaderData>(m));

    auto moved = transformed(e, Transform2::translate({100, 0}));
    REQUIRE(moved.ok());
    const auto* a = entityAs<MLeaderData>(moved.value());
    CHECK(a->lines[1].front() == Vec2{100, 10});
    CHECK(a->landing == Vec2{110, 5});
    CHECK(a->contentPos == Vec2{112.5, 5});

    auto scaled = transformed(e, Transform2::scale(2, 2));
    REQUIRE(scaled.ok());
    CHECK(entityAs<MLeaderData>(scaled.value())->blockScale == doctest::Approx(4));
    CHECK_FALSE(transformed(e, Transform2::scale(2, 1)).ok());

    auto g = gripPoints(e);
    REQUIRE(g.size() == 6);  // 4 line points, the landing and the note
    auto landing = moveGrip(e, 4, {10, 8});
    REQUIRE(landing.ok());
    const auto* l = entityAs<MLeaderData>(landing.value());
    CHECK(l->landing == Vec2{10, 8});
    CHECK(l->lines[0].back() == Vec2{10, 8});  // lines that ended there follow
    CHECK(l->lines[0].front() == Vec2{0, 0});
    CHECK(entityAs<MLeaderData>(moveGrip(e, 5, {20, 5}).value())->contentPos == Vec2{20, 5});
    CHECK(entityAs<MLeaderData>(moveGrip(e, 0, {-1, -1}).value())->lines[0].front() ==
          Vec2{-1, -1});
    CHECK_FALSE(moveGrip(e, 6, {0, 0}).ok());
}

TEST_CASE("outlinePoints follows the travel direction and is finer than flatten's limit") {
    Entity circle;
    circle.data = CircleData{{1e6, 5}, 50};
    std::vector<Vec2> pts;
    REQUIRE(outlinePoints(circle, 1e-7, pts));
    CHECK(pts.size() > 4096);  // flatten() stops at 4096 segments
    double area = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Vec2 a = pts[i] - Vec2{1e6, 5}, b = pts[(i + 1) % pts.size()] - Vec2{1e6, 5};
        area += cross(a, b) / 2;
    }
    CHECK(area == doctest::Approx(kPi * 2500).epsilon(1e-8));

    // A clockwise polyline arc is walked from its start to its end.
    PolylineData pl;
    pl.v = {PolyVertex{{0, 0}, -1.0}, PolyVertex{{10, 0}, 0}};
    Entity e;
    e.data = pl;
    REQUIRE(outlinePoints(e, 1e-3, pts));
    CHECK(distance(pts.front(), {0, 0}) < 1e-9);
    CHECK(distance(pts.back(), {10, 0}) < 1e-9);
    CHECK(pts[pts.size() / 2].y > 0);  // a negative bulge from left to right bows upward
}

TEST_CASE("a closed CV spline has one grip per vertex and moving one keeps it closed") {
    SplineData sp;
    sp.degree = 3;
    sp.closed = true;
    sp.periodic = true;
    const std::vector<Vec2> pts{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    sp.control = pts;
    for (int i = 0; i < 3; ++i)
        sp.control.push_back(pts[static_cast<std::size_t>(i)]);
    for (int i = 0; i < 11; ++i)
        sp.knots.push_back(i);
    Entity e = make(Indirect<SplineData>(sp));
    CHECK(gripPoints(e).size() == 4);
    auto m = moveGrip(e, 0, {-3, -3});
    REQUIRE(m.ok());
    const auto& d = *entityAs<SplineData>(m.value());
    CHECK(d.control.front() == Vec2{-3, -3});
    CHECK(d.control[4] == Vec2{-3, -3});
    CHECK_FALSE(moveGrip(e, 4, {0, 0}).ok());
    std::vector<CurveRef> cs;
    REQUIRE(curvesOf(m.value(), cs));
    CHECK(distance(cs[0].startPoint(), cs[0].endPoint()) < 1e-9);
}

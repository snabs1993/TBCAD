#include "test_support.h"

using namespace tbcad;
using namespace tbcad::testing;
using namespace tbcad::modify;

TEST_CASE("lengthen a line at the end nearest the pick") {
    Entity l = line({0, 0}, {10, 0});
    auto r = lengthenEntity(l, {9, 1}, LengthenMode::Delta, 5);
    REQUIRE(r.ok());
    const auto& d = as<LineData>(*r.value().modified);
    CHECK(near(d.a, {0, 0}));
    CHECK(near(d.b, {15, 0}));
    r = lengthenEntity(l, {1, 1}, LengthenMode::Delta, -4);
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).a, {4, 0}));
    r = lengthenEntity(l, {9, 0}, LengthenMode::Percent, 150);
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).b, {15, 0}));
    r = lengthenEntity(l, {9, 0}, LengthenMode::Total, 3);
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).b, {3, 0}));
    CHECK_FALSE(lengthenEntity(l, {9, 0}, LengthenMode::Delta, -10).ok());
}

TEST_CASE("lengthen an arc changes its sweep, not its radius") {
    Entity a = arc({0, 0}, 10, 0, kPi / 2);
    double len = 10 * kPi / 2;
    auto r = lengthenEntity(a, {0, 10}, LengthenMode::Delta, 10 * kPi / 4);
    REQUIRE(r.ok());
    const auto& d = as<ArcData>(*r.value().modified);
    CHECK(near(d.radius, 10));
    CHECK(near(d.startAngle, 0));
    CHECK(near(d.endAngle, 3 * kPi / 4, 1e-6));
    CHECK(near(*lengthenableLength(a), len));
    CHECK_FALSE(lengthenEntity(a, {0, 10}, LengthenMode::Delta, 100).ok());
}

TEST_CASE("lengthen and shorten a polyline across segments") {
    Entity p = poly(pts({{0, 0}, {10, 0}, {10, 10}}), false);
    auto r = lengthenEntity(p, {10, 9}, LengthenMode::Delta, 5);
    REQUIRE(r.ok());
    const auto& d = as<PolylineData>(*r.value().modified);
    REQUIRE(d.v.size() == 3);
    CHECK(near(d.v[2].p, {10, 15}));
    r = lengthenEntity(p, {10, 9}, LengthenMode::Delta, -14);  // drops the whole last leg
    REQUIRE(r.ok());
    const auto& s = as<PolylineData>(*r.value().modified);
    REQUIRE(s.v.size() == 2);
    CHECK(near(s.v[1].p, {6, 0}));
    r = lengthenEntity(p, {1, 0}, LengthenMode::Delta, 2);  // start end
    REQUIRE(r.ok());
    CHECK(near(as<PolylineData>(*r.value().modified).v[0].p, {-2, 0}));
}

TEST_CASE("lengthen to a point") {
    Entity l = line({0, 0}, {10, 0});
    auto r = lengthenToPoint(l, {9, 0}, {14, 3});
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).b, {14, 0}));
    Entity a = arc({0, 0}, 10, 0, kPi / 2);
    r = lengthenToPoint(a, {0, 10}, {-5, 5});
    REQUIRE(r.ok());
    CHECK(near(as<ArcData>(*r.value().modified).endAngle, 3 * kPi / 4, 1e-6));
}

TEST_CASE("circles and closed polylines cannot be lengthened") {
    CHECK_FALSE(lengthenEntity(circle({0, 0}, 5), {5, 0}, LengthenMode::Delta, 1).ok());
    CHECK_FALSE(
        lengthenEntity(poly(pts({{0, 0}, {1, 0}, {1, 1}}), true), {0, 0}, LengthenMode::Delta, 1)
            .ok());
}

TEST_CASE("reverse a polyline keeps its shape") {
    std::vector<PolyVertex> v = pts({{0, 0}, {10, 0}, {10, 10}});
    v[1].bulge = 0.5;
    Entity p = poly(v, false);
    auto r = reverseEntity(p);
    REQUIRE(r.ok());
    const auto& d = as<PolylineData>(*r.value().modified);
    REQUIRE(d.v.size() == 3);
    CHECK(near(d.v[0].p, {10, 10}));
    CHECK(near(d.v[2].p, {0, 0}));
    CHECK(near(d.v[0].bulge, -0.5));
    CHECK(near(d.v[1].bulge, 0));
    CHECK(near(d.v[2].bulge, 0));
    // Reversing twice gives the original back.
    Entity back = *r.value().modified;
    auto r2 = reverseEntity(back);
    REQUIRE(r2.ok());
    const auto& e = as<PolylineData>(*r2.value().modified);
    CHECK(near(e.v[1].bulge, 0.5));
    CHECK(near(e.v[0].p, {0, 0}));
}

TEST_CASE("reverse a line and refuse an arc") {
    auto r = reverseEntity(line({1, 2}, {3, 4}));
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).a, {3, 4}));
    CHECK_FALSE(reverseEntity(arc({0, 0}, 1, 0, 1)).ok());
}

TEST_CASE("divide an open line, a circle and a polyline") {
    auto d = dividePoints(line({0, 0}, {10, 0}), 4);
    REQUIRE(d.ok());
    REQUIRE(d.value().size() == 3);
    CHECK(near(d.value()[0], {2.5, 0}));
    CHECK(near(d.value()[2], {7.5, 0}));
    auto c = dividePoints(circle({0, 0}, 2), 4);
    REQUIRE(c.ok());
    REQUIRE(c.value().size() == 4);
    CHECK(near(c.value()[0], {2, 0}, 1e-4));
    CHECK(near(c.value()[1], {0, 2}, 1e-4));
    auto sq = dividePoints(poly(pts({{0, 0}, {4, 0}, {4, 4}, {0, 4}}), true), 8);
    REQUIRE(sq.ok());
    REQUIRE(sq.value().size() == 8);
    CHECK(near(sq.value()[1], {2, 0}));
    CHECK(near(sq.value()[2], {4, 0}));
    CHECK_FALSE(dividePoints(line({0, 0}, {1, 0}), 1).ok());
}

TEST_CASE("measure along an arc and from the far end") {
    auto m = measurePoints(line({0, 0}, {10, 0}), 3);
    REQUIRE(m.ok());
    REQUIRE(m.value().size() == 3);
    CHECK(near(m.value()[2], {9, 0}));
    auto f = measurePoints(line({0, 0}, {10, 0}), 4, true);
    REQUIRE(f.ok());
    REQUIRE(f.value().size() == 2);
    CHECK(near(f.value()[0], {6, 0}));
    auto a = measurePoints(arc({0, 0}, 10, 0, kPi), 10 * kPi / 2);
    REQUIRE(a.ok());
    REQUIRE(a.value().size() == 2);
    CHECK(near(a.value()[0], {0, 10}, 1e-4));
    CHECK_FALSE(measurePoints(line({0, 0}, {1, 0}), 0).ok());
}

TEST_CASE("align with one and two point pairs") {
    auto t1 = alignTransform({1, 1}, {5, 6}, std::nullopt, std::nullopt, false);
    REQUIRE(t1.ok());
    CHECK(near(t1.value().apply({2, 1}), {6, 6}));
    auto t2 = alignTransform({0, 0}, {10, 10}, Vec2{1, 0}, Vec2{10, 12}, false);
    REQUIRE(t2.ok());
    CHECK(near(t2.value().apply({0, 0}), {10, 10}));
    CHECK(near(t2.value().apply({2, 0}), {10, 12}));
    auto t3 = alignTransform({0, 0}, {0, 0}, Vec2{1, 0}, Vec2{0, 4}, true);
    REQUIRE(t3.ok());
    CHECK(near(t3.value().apply({1, 0}), {0, 4}));
    CHECK_FALSE(alignTransform({0, 0}, {1, 1}, Vec2{0, 0}, Vec2{2, 2}, true).ok());
}

TEST_CASE("overkill removes duplicates and merges overlapping lines") {
    Entity a = line({0, 0}, {10, 0});
    Entity b = line({10, 0}, {0, 0});  // reversed duplicate
    Entity c = line({5, 0}, {20, 0});  // overlaps a
    Entity d = line({0, 5}, {1, 5});   // unrelated
    Entity e = circle({0, 0}, 3);
    Entity f = circle({0, 0}, 3);
    OverkillResult r = overkill({&a, &b, &c, &d, &e, &f});
    CHECK(r.duplicates == 2);
    CHECK(r.erased == std::vector<std::size_t>{0, 1, 2, 5});
    REQUIRE(r.created.size() == 1);
    const auto& l = as<LineData>(r.created[0]);
    CHECK(near(std::min(l.a.x, l.b.x), 0));
    CHECK(near(std::max(l.a.x, l.b.x), 20));
}

TEST_CASE("overkill respects properties and end-to-end option") {
    Entity a = line({0, 0}, {10, 0});
    Entity b = line({0, 0}, {10, 0});
    b.common.color = Color::byLayer();
    b.common.layer = LayerId{9};
    OverkillResult r = overkill({&a, &b});
    CHECK(r.erased.empty());
    OverkillOptions o;
    o.ignoreProperties = true;
    CHECK(overkill({&a, &b}, o).erased.size() == 1);

    Entity c = line({0, 0}, {5, 0});
    Entity d = line({5, 0}, {9, 0});
    OverkillOptions touch;
    touch.combineEndToEnd = false;
    CHECK(overkill({&c, &d}, touch).created.empty());
    CHECK(overkill({&c, &d}).created.size() == 1);
}

TEST_CASE("overkill merges arcs of one circle and drops duplicate polylines") {
    Entity a = arc({0, 0}, 5, 0, 1.0);
    Entity b = arc({0, 0}, 5, 0.5, 2.0);
    OverkillResult r = overkill({&a, &b});
    REQUIRE(r.created.size() == 1);
    const auto& m = as<ArcData>(r.created[0]);
    CHECK(near(m.startAngle, 0, 1e-6));
    CHECK(near(m.endAngle, 2.0, 1e-6));
    Entity p = poly(pts({{0, 0}, {1, 0}, {1, 1}}), false);
    Entity q = poly(pts({{1, 1}, {1, 0}, {0, 0}}), false);
    CHECK(overkill({&p, &q}).duplicates == 1);
}

TEST_CASE("polyline conversions") {
    auto pl = toPolylineEntity(line({0, 0}, {3, 4}));
    REQUIRE(pl.ok());
    CHECK(as<PolylineData>(pl.value()).v.size() == 2);
    auto ap = toPolylineEntity(arc({0, 0}, 5, 0, kPi / 2));
    REQUIRE(ap.ok());
    CHECK(near(as<PolylineData>(ap.value()).v[0].bulge, std::tan(kPi / 8)));
    CHECK_FALSE(toPolylineEntity(circle({0, 0}, 1)).ok());

    Entity p = poly(pts({{0, 0}, {5, 3}, {10, 0}, {15, 3}}), false);
    auto sp = polylineToSpline(p);
    REQUIRE(sp.ok());
    CHECK(kind(sp.value()) == EntityKind::Spline);
    auto back = splineToPolyline(sp.value(), 0.01);
    REQUIRE(back.ok());
    const auto& bp = as<PolylineData>(back.value());
    CHECK(bp.v.size() > 4);
    CHECK(near(bp.v.front().p, {0, 0}, 1e-6));
    CHECK(near(bp.v.back().p, {15, 3}, 1e-6));

    auto w = setPolylineWidth(p, 0.5);
    REQUIRE(w.ok());
    CHECK(near(as<PolylineData>(*w.value().modified).v[1].endWidth, 0.5));
    CHECK_FALSE(setPolylineWidth(line({0, 0}, {1, 1}), 1).ok());
}

TEST_CASE("extrim removes the part of a line beyond a cutting line") {
    Entity edge = line({5, -10}, {5, 10});
    Entity target = line({0, 0}, {10, 0});
    auto r = extrimEntity(target, edge, {8, 3});
    REQUIRE(r.ok());
    REQUIRE(r.value().modified.has_value());
    const auto& d = as<LineData>(*r.value().modified);
    CHECK(near(d.a, {0, 0}));
    CHECK(near(d.b, {5, 0}));
    CHECK(r.value().added.empty());
    // Pick on the other side keeps the right half.
    auto l = extrimEntity(target, edge, {1, 3});
    REQUIRE(l.ok());
    CHECK(near(as<LineData>(*l.value().modified).a, {5, 0}));
}

TEST_CASE("extrim with a circle edge cuts what is inside, keeping both outer ends") {
    Entity edge = circle({5, 0}, 2);
    Entity target = line({0, 0}, {10, 0});
    auto r = extrimEntity(target, edge, {5, 0});  // inside
    REQUIRE(r.ok());
    REQUIRE(r.value().modified.has_value());
    REQUIRE(r.value().added.size() == 1);
    std::vector<double> xs;
    for (const Entity* e : {&*r.value().modified, &r.value().added[0]}) {
        const auto& d = as<LineData>(*e);
        xs.push_back(std::min(d.a.x, d.b.x));
        xs.push_back(std::max(d.a.x, d.b.x));
    }
    std::sort(xs.begin(), xs.end());
    CHECK(near(xs[0], 0));
    CHECK(near(xs[1], 3));
    CHECK(near(xs[2], 7));
    CHECK(near(xs[3], 10));
}

TEST_CASE("extrim reports edges without two sides and misses") {
    Entity target = line({0, 0}, {10, 0});
    CHECK_FALSE(extrimEntity(target, poly(pts({{0, 5}, {1, 5}, {2, 6}}), false), {0, 0}).ok());
    auto none = extrimEntity(target, line({50, -1}, {50, 1}), {60, 0});
    REQUIRE(none.ok());
    CHECK_FALSE(none.value().modified.has_value());
    CHECK_FALSE(none.value().eraseSource);
    // Everything lies on the picked side: the target disappears.
    auto gone = extrimEntity(target, line({-5, -1}, {-5, 1}), {5, 0});
    REQUIRE(gone.ok());
    CHECK_FALSE(gone.value().eraseSource);  // never crosses the edge
}

TEST_CASE("blend two lines with a tangent spline") {
    Entity a = line({0, 0}, {10, 0});
    Entity b = line({30, 10}, {20, 10});
    auto r = blendCurves(a, {9, 0}, b, {21, 10});
    REQUIRE(r.ok());
    REQUIRE(kind(r.value()) == EntityKind::Spline);
    std::vector<geom::CurveRef> cs;
    REQUIRE(geom::curvesOf(r.value(), cs));
    const auto& c = cs.front();
    Vec2 s = c.pointAt(c.t0()), e = c.pointAt(c.t0() + c.sweep());
    CHECK(near(s, {10, 0}, 1e-6));
    CHECK(near(e, {20, 10}, 1e-6));
    Vec2 ts = c.tangentAt(c.t0()), te = c.tangentAt(c.t0() + c.sweep());
    CHECK(near(ts, {1, 0}, 1e-6));
    CHECK(near(te, {1, 0}, 1e-6));  // arrives travelling along +x, into b's end
    CHECK_FALSE(blendCurves(a, {9, 0}, line({10, 0}, {12, 3}), {10, 0}).ok());
    CHECK_FALSE(blendCurves(a, {9, 0}, circle({0, 0}, 1), {1, 0}).ok());
}

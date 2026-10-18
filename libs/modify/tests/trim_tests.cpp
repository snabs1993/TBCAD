#include <random>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::modify;
using namespace tbcad::testing;

namespace {
std::vector<const Entity*> ptrs(const std::vector<Entity>& v) {
    std::vector<const Entity*> out;
    for (const auto& e : v)
        out.push_back(&e);
    return out;
}
}  // namespace

TEST_CASE("trim a line crossing two others: the middle goes") {
    Entity target = line({0, 0}, {10, 0});
    std::vector<Entity> edges{line({3, -5}, {3, 5}), line({7, -5}, {7, 5})};
    auto r = trimEntity(target, {5, 0}, ptrs(edges));
    REQUIRE(r.ok());
    REQUIRE(r.value().modified.has_value());
    REQUIRE(r.value().added.size() == 1);
    CHECK_FALSE(r.value().eraseSource);
    CHECK(near(as<LineData>(*r.value().modified).a, {0, 0}));
    CHECK(near(as<LineData>(*r.value().modified).b, {3, 0}));
    CHECK(near(as<LineData>(r.value().added[0]).a, {7, 0}));
    CHECK(near(as<LineData>(r.value().added[0]).b, {10, 0}));
    CHECK(r.value().added[0].common.layer == LayerId{7});
}

TEST_CASE("trim at the end of a line") {
    Entity target = line({0, 0}, {10, 0});
    std::vector<Entity> edges{line({3, -5}, {3, 5}), line({7, -5}, {7, 5})};
    auto left = trimEntity(target, {1, 0}, ptrs(edges));
    REQUIRE(left.ok());
    CHECK(left.value().added.empty());
    CHECK(near(as<LineData>(*left.value().modified).a, {3, 0}));
    CHECK(near(as<LineData>(*left.value().modified).b, {10, 0}));
    auto right = trimEntity(target, {9, 0}, ptrs(edges));
    CHECK(near(as<LineData>(*right.value().modified).b, {7, 0}));
    CHECK(near(as<LineData>(*right.value().modified).a, {0, 0}));
}

TEST_CASE("trim errors: no cutting edge next to the pick") {
    Entity target = line({0, 0}, {10, 0});
    std::vector<Entity> edges{line({20, -5}, {20, 5})};
    CHECK_FALSE(trimEntity(target, {5, 0}, ptrs(edges)).ok());
    CHECK_FALSE(trimEntity(target, {5, 0}, {}).ok());
    // The edge only counts when it is extended.
    std::vector<Entity> short_edge{line({5, 1}, {5, 5})};
    CHECK_FALSE(trimEntity(target, {2, 0}, ptrs(short_edge)).ok());
    TrimOptions ext;
    ext.extendEdges = true;
    auto r = trimEntity(target, {2, 0}, ptrs(short_edge), ext);
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).a, {5, 0}));
}

TEST_CASE("trim a circle gives an arc") {
    Entity c = circle({0, 0}, 5);
    std::vector<Entity> edges{line({0, -10}, {0, 10})};
    auto r = trimEntity(c, {5, 0}, ptrs(edges));  // remove the right half
    REQUIRE(r.ok());
    REQUIRE(kind(*r.value().modified) == EntityKind::Arc);
    const auto& a = as<ArcData>(*r.value().modified);
    CHECK(near(a.startAngle, kPi / 2));
    CHECK(near(a.endAngle, 3 * kPi / 2));
    CHECK(near(a.radius, 5));
    // One cut is not enough.
    std::vector<Entity> one{line({0, 0}, {10, 0})};
    CHECK_FALSE(trimEntity(c, {0, 5}, ptrs(one)).ok());
}

TEST_CASE("trim an arc between two edges") {
    Entity a = arc({0, 0}, 5, 0, kPi);  // upper half
    std::vector<Entity> edges{line({-10, 2}, {10, 2})};
    auto r = trimEntity(a, {0, 5}, ptrs(edges));  // between the two crossings: removed
    REQUIRE(r.ok());
    REQUIRE(r.value().added.size() == 1);
    CHECK(kind(*r.value().modified) == EntityKind::Arc);
}

TEST_CASE("trim a polyline and open a closed one") {
    Entity pl = poly(pts({{0, 0}, {10, 0}, {10, 10}}), false);
    std::vector<Entity> edges{line({5, -1}, {5, 1}), line({9, 5}, {11, 5})};
    auto r = trimEntity(pl, {7, 0}, ptrs(edges));
    REQUIRE(r.ok());
    CHECK(r.value().added.size() == 1);
    CHECK(sameVertexSet(verts(*r.value().modified), {{0, 0}, {5, 0}}));
    CHECK(sameVertexSet(verts(r.value().added[0]), {{10, 5}, {10, 10}}));

    Entity closed = poly(pts({{0, 0}, {10, 0}, {10, 10}, {0, 10}}), true);
    std::vector<Entity> cut{line({5, -2}, {5, 2}), line({-2, 5}, {2, 5})};
    auto o = trimEntity(closed, {2, 0}, ptrs(cut));  // removes the corner at the origin
    REQUIRE(o.ok());
    REQUIRE(o.value().added.empty());
    const auto& res = as<PolylineData>(*o.value().modified);
    CHECK_FALSE(res.closed);
    CHECK(sameVertexSet(verts(*o.value().modified), {{5, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 5}}));
}

TEST_CASE("extend a line to a line, an arc, and a line to the nearest boundary") {
    Entity l = line({0, 0}, {5, 0});
    std::vector<Entity> edges{line({9, -3}, {9, 3}), line({12, -3}, {12, 3})};
    auto r = extendEntity(l, {4, 0}, ptrs(edges));
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).a, {0, 0}));
    CHECK(near(as<LineData>(*r.value().modified).b, {9, 0}));
    // The start is extended when the pick is near it.
    Entity l2 = line({3, 0}, {8, 0});
    std::vector<Entity> back{line({1, -3}, {1, 3})};
    auto r2 = extendEntity(l2, {3.5, 0}, ptrs(back));
    CHECK(near(as<LineData>(*r2.value().modified).a, {1, 0}));
    // Nothing ahead on the picked side.
    CHECK_FALSE(extendEntity(l, {0.5, 0}, ptrs(edges)).ok());

    std::vector<Entity> arcs{arc({0, 0}, 8, 7 * kPi / 4, kPi / 4)};
    auto r3 = extendEntity(l, {4, 0}, ptrs(arcs));
    REQUIRE(r3.ok());
    CHECK(near(as<LineData>(*r3.value().modified).b, {8, 0}));
}

TEST_CASE("extend an arc and a polyline end") {
    Entity a = arc({0, 0}, 5, 0, kPi / 4);
    std::vector<Entity> edges{line({-10, 5}, {10, 5})};
    auto r = extendEntity(a, {5 * std::cos(kPi / 4), 5 * std::sin(kPi / 4)}, ptrs(edges));
    REQUIRE(r.ok());
    const auto& ad = as<ArcData>(*r.value().modified);
    CHECK(near(ad.endAngle, kPi / 2));
    CHECK(near(ad.startAngle, 0));

    Entity pl = poly(pts({{0, 0}, {5, 0}, {5, 5}}), false);
    std::vector<Entity> wall{line({-5, 9}, {15, 9})};
    auto p = extendEntity(pl, {5, 4}, ptrs(wall));
    REQUIRE(p.ok());
    CHECK(sameVertexSet(verts(*p.value().modified), {{0, 0}, {5, 0}, {5, 9}}));
    Entity closed = poly(pts({{0, 0}, {5, 0}, {5, 5}}), true);
    CHECK_FALSE(extendEntity(closed, {5, 4}, ptrs(wall)).ok());
}

TEST_CASE("property: trimming never produces a zero-length entity") {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> coord(-20, 20);
    int trimmed = 0;
    for (int iter = 0; iter < 400; ++iter) {
        Entity target = (iter % 3 == 0) ? line({coord(rng), coord(rng)}, {coord(rng), coord(rng)})
                        : (iter % 3 == 1)
                            ? circle({coord(rng), coord(rng)}, 3 + std::abs(coord(rng)))
                            : arc({coord(rng), coord(rng)}, 3 + std::abs(coord(rng)), 0.3, 4.0);
        std::vector<Entity> edges;
        for (int k = 0; k < 4; ++k)
            edges.push_back(line({coord(rng), coord(rng)}, {coord(rng), coord(rng)}));
        std::vector<geom::CurveRef> cs;
        geom::curvesOf(target, cs);
        if (cs.empty() || cs[0].length() < 1e-3)
            continue;
        Vec2 pick = cs[0].pointAt(cs[0].t0() + 0.5 * (cs[0].isLine() ? 1.0 : cs[0].sweep()));
        auto r = trimEntity(target, pick, ptrs(edges));
        if (!r.ok())
            continue;
        ++trimmed;
        std::vector<const Entity*> out;
        if (r.value().modified)
            out.push_back(&*r.value().modified);
        for (const auto& e : r.value().added)
            out.push_back(&e);
        for (const Entity* e : out)
            CHECK(geom::length(*e) > tol::kLinear);
    }
    CHECK(trimmed > 50);
}

namespace {
Entity splineThrough(std::vector<Vec2> fit) {
    SplineData sp;
    sp.degree = 3;
    sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    sp.control = std::move(fit);
    return make(Indirect<SplineData>(sp));
}
}  // namespace

TEST_CASE("trim a spline at a line across it keeps the two ends as splines") {
    for (double ox : {0.0, 500123.456}) {
        const double oy = ox * 9;
        Entity target =
            splineThrough({{ox, oy}, {ox + 10, oy + 20}, {ox + 30, oy - 20}, {ox + 40, oy}});
        Entity edge = line({ox + 19, oy - 50}, {ox + 19, oy + 50});
        std::vector<Entity> edges{edge};
        // Pick near the middle of the curve: both sides cut by the one edge, the pick side goes.
        auto r = trimEntity(target, {ox + 25, oy - 5}, ptrs(edges));
        REQUIRE(r.ok());
        REQUIRE(r.value().modified.has_value());
        const auto& rest = as<SplineData>(*r.value().modified);
        CHECK(rest.control.size() >= 4);
        std::vector<geom::CurveRef> cs;
        REQUIRE(geom::curvesOf(*r.value().modified, cs));
        REQUIRE(cs.size() == 1);
        // What is left starts where the spline started and ends on the edge.
        CHECK(near(cs[0].startPoint(), {ox, oy}, 1e-6));
        CHECK(near(cs[0].endPoint().x, ox + 19, 1e-6));
    }
}

TEST_CASE("trim a ray: the stretch under the pick goes, a finite line stays towards the start") {
    for (double ox : {0.0, 500123.456}) {
        RayData ray;
        ray.origin = {ox, ox * 9};
        ray.dir = {1, 0};
        Entity target = make(ray);
        std::vector<Entity> edges{line({ox + 10, ox * 9 - 5}, {ox + 10, ox * 9 + 5}),
                                  line({ox + 30, ox * 9 - 5}, {ox + 30, ox * 9 + 5})};
        auto r = trimEntity(target, {ox + 20, ox * 9}, ptrs(edges));
        REQUIRE(r.ok());
        REQUIRE(r.value().modified.has_value());
        REQUIRE(r.value().added.size() == 1);
        CHECK(near(as<LineData>(*r.value().modified).a, {ox, ox * 9}));
        CHECK(near(as<LineData>(*r.value().modified).b, {ox + 10, ox * 9}));
        CHECK(near(as<RayData>(r.value().added[0]).origin, {ox + 30, ox * 9}));
        CHECK(near(as<RayData>(r.value().added[0]).dir, {1, 0}));
    }
}

TEST_CASE("trim a construction line: it becomes rays away from the cut") {
    RayData x;
    x.origin = {4, 4};
    x.dir = {0, 1};
    x.infinite = true;
    Entity target = make(x);
    std::vector<Entity> edges{line({-5, 10}, {5, 10})};
    auto r = trimEntity(target, {4, 15}, ptrs(edges));
    REQUIRE(r.ok());
    REQUIRE(r.value().modified.has_value());
    CHECK(r.value().added.empty());
    const auto& ray = as<RayData>(*r.value().modified);
    CHECK_FALSE(ray.infinite);
    CHECK(near(ray.origin, {4, 10}));
    CHECK(near(ray.dir, {0, -1}));  // the part below the cut stays, running down
}

TEST_CASE("trim a closed spline between two cuts leaves one open spline across the seam") {
    std::vector<Vec2> fit;
    for (int i = 0; i < 12; ++i)
        fit.push_back({50 * std::cos(2 * kPi * i / 12), 50 * std::sin(2 * kPi * i / 12)});
    geom::InterpolateOptions io;
    io.closed = true;
    auto done = geom::interpolateSpline(fit, io);
    REQUIRE(done.ok());
    Entity target = make(Indirect<SplineData>(done.value()));
    std::vector<Entity> edges{line({-60, 20}, {60, 20}), line({-60, -20}, {60, -20})};
    auto r = trimEntity(target, {50, 0}, ptrs(edges));  // the right-hand side between the lines
    REQUIRE(r.ok());
    REQUIRE(r.value().modified.has_value());
    std::vector<geom::CurveRef> cs;
    REQUIRE(geom::curvesOf(*r.value().modified, cs));
    REQUIRE(cs.size() == 1);
    CHECK(near(cs[0].startPoint().y, 20, 0.5));
    CHECK(near(cs[0].endPoint().y, -20, 0.5));
    CHECK(cs[0].pointAt(0.5 * (cs[0].t0() + cs[0].t1())).x < 0);  // the long way round, to the left
}

TEST_CASE("extend an elliptical arc along its ellipse, at both ends") {
    EllipseData el;
    el.center = {0, 0};
    el.majorAxis = {10, 0};
    el.ratio = 0.5;
    el.startParam = 0.3;
    el.endParam = 1.0;
    Entity e = make(el);
    // A wall at y = 4 meets the ellipse (minor radius 5) at asin(0.8) = 0.9273 and 2.2143.
    std::vector<Entity> wall{line({-20, 4}, {20, 4})};
    const Vec2 endPick{10 * std::cos(1.0), 5 * std::sin(1.0)};
    // The end already passed the first crossing, so it runs on to the second one.
    auto r = extendEntity(e, endPick, ptrs(wall));
    REQUIRE(r.ok());
    CHECK(near(as<EllipseData>(*r.value().modified).endParam, 2.2143, 1e-3));
    CHECK(near(as<EllipseData>(*r.value().modified).startParam, 0.3));
    // The start runs backwards round the ellipse to the crossing at 2.2143 - going away from the
    // arc.
    std::vector<Entity> low{line({-20, -4}, {20, -4})};
    auto s = extendEntity(e, {10 * std::cos(0.3), 5 * std::sin(0.3)}, ptrs(low));
    REQUIRE(s.ok());
    CHECK(near(as<EllipseData>(*s.value().modified).startParam, kTwoPi - 0.9273, 1e-3));
    CHECK(near(as<EllipseData>(*s.value().modified).endParam, 1.0));
    // A full ellipse has no ends.
    el.startParam = 0;
    el.endParam = kTwoPi;
    CHECK_FALSE(extendEntity(make(el), endPick, ptrs(wall)).ok());
    // Nothing ahead.
    std::vector<Entity> far{line({-20, 40}, {20, 40})};
    CHECK_FALSE(extendEntity(e, endPick, ptrs(far)).ok());
}

TEST_CASE("extend a spline along its end tangent, keeping the drawn part exactly") {
    for (double ox : {0.0, 500123.456}) {
        SplineData sp;
        sp.degree = 3;
        sp.knots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
        sp.control = {{ox, 0}, {ox + 10, 20}, {ox + 30, -20}, {ox + 50, 10}, {ox + 60, 0}};
        Entity e = make(Indirect<SplineData>(sp));
        std::vector<geom::CurveRef> before;
        geom::curvesOf(e, before);
        std::vector<Entity> wall{line({ox + 80, -200}, {ox + 80, 200}),
                                 line({ox - 10, -200}, {ox - 10, 200})};
        auto r = extendEntity(e, {ox + 59, 0.5}, ptrs(wall));
        REQUIRE(r.ok());
        std::vector<geom::CurveRef> after;
        REQUIRE(geom::curvesOf(*r.value().modified, after));
        REQUIRE(after.size() == 1);
        // The end tangent leaves (60, 0) towards (+10, -10): it meets x = 80 at (80, -20).
        CHECK(near(after[0].endPoint(), {ox + 80, -20}, 1e-6));
        CHECK(near(after[0].startPoint(), {ox, 0}, 1e-6));
        for (double t : {0.2, 0.7, 1.0, 1.6}) {
            const Vec2 p = before[0].pointAt(t);
            CHECK(after[0].distanceTo(p) < 1e-6);
        }
        auto s = extendEntity(e, {ox + 1, 2}, ptrs(wall));
        REQUIRE(s.ok());
        after.clear();
        REQUIRE(geom::curvesOf(*s.value().modified, after));
        CHECK(near(after[0].startPoint(), {ox - 10, -20}, 1e-6));
        CHECK(near(after[0].endPoint(), {ox + 60, 0}, 1e-6));
        for (double t : {0.2, 0.7, 1.0, 1.6})
            CHECK(after[0].distanceTo(before[0].pointAt(t)) < 1e-6);
        // Closed splines cannot be extended.
        SplineData cl = sp;
        cl.closed = true;
        CHECK_FALSE(extendEntity(make(Indirect<SplineData>(cl)), {ox, 0}, ptrs(wall)).ok());
    }
}

TEST_CASE("extend a polyline whose last segment is an arc") {
    // Quarter circle of radius 5 from (5,0) to (0,5) (counter-clockwise), then stop.
    std::vector<PolyVertex> v{
        {{10, 0}, 0, 0, 0}, {{5, 0}, std::tan(kPi / 8), 0, 0}, {{0, 5}, 0, 0, 0}};
    Entity pl = poly(v, false);
    std::vector<Entity> wall{line({-10, -3}, {10, -3})};  // reached by running on past (-5, 0)
    auto r = extendEntity(pl, {0.5, 4.9}, ptrs(wall));
    REQUIRE(r.ok());
    const auto& out = as<PolylineData>(*r.value().modified);
    REQUIRE(out.v.size() == 3);
    // The arc keeps its circle (centre origin, radius 5) and ends where it crosses y = -3.
    CHECK(near(out.v.back().p, {-4, -3}, 1e-6));
    CHECK(near(distance(out.v.back().p, {0, 0}), 5, 1e-6));
}

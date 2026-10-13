#include <random>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::modify;
using namespace tbcad::testing;

namespace {
Entity rect() {
    return poly(pts({{0, 0}, {10, 0}, {10, 6}, {0, 6}}), true);
}
Entity lShape() {
    return poly(pts({{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}}), true);
}
OffsetOptions sharp() {
    OffsetOptions o;
    o.roundCorners = false;
    return o;
}
}  // namespace

TEST_CASE("offset of a line, circle and arc is exact and keeps properties") {
    auto l = offsetEntity(line({0, 0}, {10, 0}), 2);
    REQUIRE(l.ok());
    REQUIRE(l.value().size() == 1);
    const auto& ld = as<LineData>(l.value()[0]);
    CHECK(near(ld.a, {0, 2}));
    CHECK(near(ld.b, {10, 2}));
    CHECK(l.value()[0].common.layer == LayerId{7});
    CHECK(l.value()[0].common.id == EntityId{});

    auto right = offsetEntity(line({0, 0}, {10, 0}), -2);
    CHECK(near(as<LineData>(right.value()[0]).a, {0, -2}));

    auto c = offsetEntity(circle({1, 1}, 5), -3);  // right of travel = outside
    CHECK(near(as<CircleData>(c.value()[0]).radius, 8));
    auto cin = offsetEntity(circle({1, 1}, 5), 3);
    CHECK(near(as<CircleData>(cin.value()[0]).radius, 2));
    CHECK_FALSE(offsetEntity(circle({1, 1}, 5), 6).ok());

    auto a = offsetEntity(arc({0, 0}, 5, 0, kPi / 2), -1);
    REQUIRE(a.ok());
    const auto& ad = as<ArcData>(a.value()[0]);
    CHECK(near(ad.radius, 6));
    CHECK(near(ad.startAngle, 0));
    CHECK(near(ad.endAngle, kPi / 2));
}

TEST_CASE("side selection: toward a point and through a point") {
    Entity l = line({0, 0}, {10, 0});
    CHECK(sideOfEntity(l, {5, 3}) == 1);
    CHECK(sideOfEntity(l, {5, -3}) == -1);
    CHECK(sideOfEntity(l, {5, 0}) == 0);
    auto up = offsetToward(l, {5, 100}, 2);
    CHECK(near(as<LineData>(up.value()[0]).a, {0, 2}));
    auto down = offsetToward(l, {5, -1}, 2);
    CHECK(near(as<LineData>(down.value()[0]).a, {0, -2}));
    auto thr = offsetThrough(l, {3, -4});
    REQUIRE(thr.ok());
    CHECK(near(as<LineData>(thr.value()[0]).b, {10, -4}));
    CHECK_FALSE(offsetThrough(l, {3, 0}).ok());

    Entity c = circle({0, 0}, 5);
    CHECK(sideOfEntity(c, {1, 0}) == 1);  // inside is left of the counter-clockwise travel
    auto cc = offsetThrough(c, {9, 0});
    CHECK(near(as<CircleData>(cc.value()[0]).radius, 9));
}

TEST_CASE("rectangle polyline outward: sharp corners") {
    auto r = offsetEntity(rect(), -1, sharp());
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 1);
    const Entity& e = r.value()[0];
    CHECK(as<PolylineData>(e).closed);
    CHECK(sameVertexSet(verts(e), {{-1, -1}, {11, -1}, {11, 7}, {-1, 7}}));
}

TEST_CASE("rectangle polyline outward: rounded corners") {
    auto r = offsetEntity(rect(), -1);
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 1);
    const Entity& e = r.value()[0];
    const auto& pl = as<PolylineData>(e);
    CHECK(pl.closed);
    CHECK(pl.v.size() == 8);
    CHECK(near(geom::length(pl), 2 * (10 + 6) + kTwoPi, 1e-6));
    int arcs = 0;
    for (const auto& v : pl.v) {
        if (std::abs(v.bulge) > 1e-9) {
            ++arcs;
            CHECK(near(std::abs(v.bulge), std::tan(kPi / 8), 1e-9));
        }
    }
    CHECK(arcs == 4);
    // Every point of the result is exactly 1 mm from the source.
    std::vector<Vec2> samples;
    geom::flatten(geom::CurveRef::line({0, 0}, {1, 0}), 0.01, samples);  // keeps flatten linked
    Entity src = rect();
    std::vector<geom::CurveRef> cs;
    REQUIRE(geom::curvesOf(e, cs));
    for (const auto& c : cs) {
        for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            Vec2 p = c.pointAt(c.t0() + t * c.sweep());
            if (c.isLine())
                p = c.pointAt(t);
            CHECK(near(geom::distance(src, p), 1.0, 1e-6));
        }
    }
}

TEST_CASE("rectangle polyline inward, and past collapse") {
    auto r = offsetEntity(rect(), 1);
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 1);
    CHECK(sameVertexSet(verts(r.value()[0]), {{1, 1}, {9, 1}, {9, 5}, {1, 5}}));
    CHECK(as<PolylineData>(r.value()[0]).closed);

    // Exactly half the height: nothing but a degenerate line is left.
    auto gone = offsetEntity(rect(), 4);
    CHECK_FALSE(gone.ok());
    CHECK_FALSE(gone.error().message.empty());
    auto way = offsetEntity(rect(), 100);
    CHECK_FALSE(way.ok());
}

TEST_CASE("concave L-shape offsets") {
    auto in = offsetEntity(lShape(), 1, sharp());
    REQUIRE(in.ok());
    REQUIRE(in.value().size() == 1);
    CHECK(sameVertexSet(verts(in.value()[0]),
                        {{1, 1}, {9, 1}, {9, 3}, {3, 3}, {3, 9}, {1, 9}}));

    auto out = offsetEntity(lShape(), -1, sharp());
    REQUIRE(out.ok());
    REQUIRE(out.value().size() == 1);
    CHECK(sameVertexSet(verts(out.value()[0]),
                        {{-1, -1}, {11, -1}, {11, 5}, {5, 5}, {5, 11}, {-1, 11}}));

    // Rounded inward: the reflex corner of the L is where the offset curves part, so it gets the arc.
    auto inRound = offsetEntity(lShape(), 1);
    REQUIRE(inRound.ok());
    REQUIRE(inRound.value().size() == 1);
    CHECK(as<PolylineData>(inRound.value()[0]).v.size() == 7);

    // Rounded outward: the reflex corner stays sharp, the five convex ones are rounded.
    auto rounded = offsetEntity(lShape(), -1);
    REQUIRE(rounded.ok());
    REQUIRE(rounded.value().size() == 1);
    const auto& pl = as<PolylineData>(rounded.value()[0]);
    int arcs = 0;
    for (const auto& v : pl.v)
        arcs += std::abs(v.bulge) > 1e-9 ? 1 : 0;
    CHECK(arcs == 5);
}

TEST_CASE("an offset that pinches off splits into several polylines") {
    // A dumbbell: two wide squares joined by a thin bridge. Offsetting inward by more than the
    // bridge half-width removes the bridge and leaves two squares.
    Entity d = poly(pts({{0, 0}, {4, 0}, {4, 1.8}, {6, 1.8}, {6, 0}, {10, 0}, {10, 4}, {6, 4},
                         {6, 2.2}, {4, 2.2}, {4, 4}, {0, 4}}),
                    true);
    auto r = offsetEntity(d, 0.5, sharp());
    REQUIRE(r.ok());
    CHECK(r.value().size() == 2);
    for (const auto& e : r.value())
        CHECK(as<PolylineData>(e).closed);
}

TEST_CASE("open polyline offset: square ends, rounded outside corner") {
    Entity p = poly(pts({{0, 0}, {10, 0}, {10, 10}}), false);  // turns left
    auto left = offsetEntity(p, 1, sharp());
    REQUIRE(left.ok());
    REQUIRE(left.value().size() == 1);
    CHECK(sameVertexSet(verts(left.value()[0]), {{0, 1}, {9, 1}, {9, 10}}));
    auto right = offsetEntity(p, -1, sharp());
    REQUIRE(right.ok());
    CHECK(sameVertexSet(verts(right.value()[0]), {{0, -1}, {11, -1}, {11, 10}}));
    CHECK_FALSE(as<PolylineData>(right.value()[0]).closed);
    auto round = offsetEntity(p, -1);
    REQUIRE(round.ok());
    CHECK(near(geom::length(as<PolylineData>(round.value()[0])), 10 + 10 + kPi / 2, 1e-6));
}

TEST_CASE("polyline with an arc segment") {
    // Half a stadium: line, semicircle, line.
    std::vector<PolyVertex> v{{{0, 0}, 0, 0, 0}, {{5, 0}, 1.0, 0, 0}, {{5, 4}, 0, 0, 0},
                              {{0, 4}, 0, 0, 0}};
    Entity p = poly(v, false);
    auto r = offsetEntity(p, -1);  // outside of the semicircle
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 1);
    // The semicircle (radius 2) grows to radius 3: its length goes from 2*pi to 3*pi.
    CHECK(near(geom::length(as<PolylineData>(r.value()[0])), 5 + 3 * kPi + 5, 1e-6));
}

TEST_CASE("ellipse and spline offsets stay within the tolerance") {
    EllipseData ed;
    ed.center = {0, 0};
    ed.majorAxis = {10, 0};
    ed.ratio = 0.5;
    Entity el = make(ed);
    for (double d : {-1.5, 1.5}) {
        auto r = offsetEntity(el, d);
        REQUIRE(r.ok());
        REQUIRE(r.value().size() == 1);
        REQUIRE(kind(r.value()[0]) == EntityKind::Spline);
        std::vector<geom::CurveRef> cs;
        REQUIRE(geom::curvesOf(r.value()[0], cs));
        std::vector<Vec2> samples;
        geom::flatten(cs[0], 0.005, samples);
        for (Vec2 p : samples)
            CHECK(std::abs(geom::distance(el, p) - std::abs(d)) < 0.01);
    }
}

TEST_CASE("property: offsets of random convex polygons") {
    std::mt19937 rng(777);
    std::uniform_real_distribution<double> ang(0, kTwoPi), rad(8, 12);
    for (int iter = 0; iter < 60; ++iter) {
        std::vector<double> a;
        int n = 3 + iter % 6;
        for (int i = 0; i < n; ++i)
            a.push_back(ang(rng));
        std::sort(a.begin(), a.end());
        std::vector<Vec2> p;
        for (double t : a)
            p.push_back({10 * std::cos(t), 10 * std::sin(t)});
        Entity src = poly(pts(p), true);
        double perimeter = geom::length(as<PolylineData>(src));
        if (geom::area(as<PolylineData>(src)) < 5)
            continue;
        // Outward (right of the counter-clockwise travel), rounded: perimeter + 2*pi*d.
        auto out = offsetEntity(src, -1.5);
        REQUIRE(out.ok());
        REQUIRE(out.value().size() == 1);
        CHECK(near(geom::length(as<PolylineData>(out.value()[0])), perimeter + kTwoPi * 1.5, 1e-6));
        // Inward: whatever survives stays at least d away from the source.
        auto in = offsetEntity(src, 0.5);
        if (!in.ok())
            continue;
        for (const auto& e : in.value()) {
            std::vector<geom::CurveRef> cs;
            geom::curvesOf(e, cs);
            for (const auto& c : cs) {
                for (double t : {0.0, 0.5, 1.0}) {
                    Vec2 q = c.pointAt(c.isLine() ? t : c.t0() + t * c.sweep());
                    CHECK(geom::distance(src, q) >= 0.5 - 1e-6);
                }
            }
        }
    }
}

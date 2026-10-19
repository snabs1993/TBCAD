#include <algorithm>
#include <chrono>
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

TEST_CASE("break a line between two points, either order") {
    Entity l = line({0, 0}, {10, 0});
    for (bool swapped : {false, true}) {
        auto r = swapped ? breakEntity(l, {7, 0}, {3, 0}) : breakEntity(l, {3, 0}, {7, 0});
        REQUIRE(r.ok());
        REQUIRE(r.value().added.size() == 1);
        CHECK(near(as<LineData>(*r.value().modified).b, {3, 0}));
        CHECK(near(as<LineData>(r.value().added[0]).a, {7, 0}));
        CHECK(r.value().added[0].common.layer == LayerId{7});
    }
    // A point beyond the end cuts that end off; both beyond removes everything.
    auto end = breakEntity(l, {4, 0}, {99, 0});
    REQUIRE(end.ok());
    CHECK(end.value().added.empty());
    CHECK(near(as<LineData>(*end.value().modified).b, {4, 0}));
    auto all = breakEntity(l, {-5, 0}, {99, 0});
    REQUIRE(all.ok());
    CHECK(all.value().eraseSource);
    CHECK_FALSE(breakEntity(l, {4, 0}, {4, 3}).ok());
}

TEST_CASE("break a circle: counter-clockwise from the first point to the second goes") {
    Entity c = circle({0, 0}, 5);
    auto r = breakEntity(c, {5, 0}, {0, 5});
    REQUIRE(r.ok());
    REQUIRE(kind(*r.value().modified) == EntityKind::Arc);
    const auto& a = as<ArcData>(*r.value().modified);
    CHECK(near(a.startAngle, kPi / 2));
    CHECK(near(a.endAngle, 0));
    CHECK_FALSE(breakEntity(c, {5, 0}, {5, 0}).ok());
    CHECK_FALSE(breakAtPoint(c, {5, 0}).ok());
}

TEST_CASE("break an arc and a polyline") {
    Entity a = arc({0, 0}, 5, 0, kPi);
    auto r = breakAtPoint(a, {0, 5});
    REQUIRE(r.ok());
    REQUIRE(r.value().added.size() == 1);
    CHECK(near(as<ArcData>(*r.value().modified).endAngle, kPi / 2));
    CHECK(near(as<ArcData>(r.value().added[0]).startAngle, kPi / 2));
    CHECK_FALSE(breakAtPoint(a, {5, 0}).ok());  // at the end

    Entity pl = poly(pts({{0, 0}, {10, 0}, {10, 10}}), false);
    auto b = breakEntity(pl, {5, 0}, {10, 5});
    REQUIRE(b.ok());
    REQUIRE(b.value().added.size() == 1);
    CHECK(sameVertexSet(verts(*b.value().modified), {{0, 0}, {5, 0}}));
    CHECK(sameVertexSet(verts(b.value().added[0]), {{10, 5}, {10, 10}}));
    auto at = breakAtPoint(pl, {10, 0});
    REQUIRE(at.ok());
    CHECK(sameVertexSet(verts(*at.value().modified), {{0, 0}, {10, 0}}));
    CHECK(sameVertexSet(verts(at.value().added[0]), {{10, 0}, {10, 10}}));

    Entity closed = poly(pts({{0, 0}, {10, 0}, {10, 10}, {0, 10}}), true);
    auto cb = breakEntity(closed, {5, 0}, {5, 10});
    REQUIRE(cb.ok());
    CHECK(cb.value().added.empty());
    CHECK_FALSE(as<PolylineData>(*cb.value().modified).closed);
    CHECK(sameVertexSet(verts(*cb.value().modified), {{5, 10}, {0, 10}, {0, 0}, {10, 0}, {5, 0}}) ==
          false);
    // Removed: the part from (5,0) to (5,10) along the way the vertices run (right side); what
    // is left runs from (5,10) over (0,10), (0,0) to (5,0).
    CHECK(sameVertexSet(verts(*cb.value().modified), {{5, 10}, {0, 10}, {0, 0}, {5, 0}}));
}

TEST_CASE("join three collinear lines, overlapping and touching") {
    std::vector<Entity> v{line({0, 0}, {4, 0}), line({3, 0}, {8, 0}), line({10, 0}, {8, 0})};
    auto r = joinEntities(ptrs(v));
    REQUIRE(r.created.size() == 1);
    CHECK(r.consumed.size() == 3);
    CHECK(kind(r.created[0]) == EntityKind::Line);
    const auto& l = as<LineData>(r.created[0]);
    CHECK(near(std::min(l.a.x, l.b.x), 0));
    CHECK(near(std::max(l.a.x, l.b.x), 10));
    // Lines on different lines stay apart.
    std::vector<Entity> apart{line({0, 0}, {4, 0}), line({0, 1}, {4, 1})};
    auto none = joinEntities(ptrs(apart));
    CHECK(none.created.empty());
    CHECK(none.consumed.empty());
}

TEST_CASE("join arcs on one circle, and a closing pair makes a circle") {
    std::vector<Entity> v{arc({0, 0}, 5, 0, kPi / 2), arc({0, 0}, 5, kPi / 2, kPi)};
    auto r = joinEntities(ptrs(v));
    REQUIRE(r.created.size() == 1);
    REQUIRE(kind(r.created[0]) == EntityKind::Arc);
    CHECK(near(as<ArcData>(r.created[0]).startAngle, 0));
    CHECK(near(as<ArcData>(r.created[0]).endAngle, kPi));

    std::vector<Entity> closing{arc({0, 0}, 5, 0, kPi), arc({0, 0}, 5, kPi, 0)};
    auto c = joinEntities(ptrs(closing));
    REQUIRE(c.created.size() == 1);
    CHECK(kind(c.created[0]) == EntityKind::Circle);
    CHECK(c.consumed.size() == 2);
}

TEST_CASE("join a chain of mixed lines, arcs and polylines into one polyline") {
    std::vector<Entity> v{
        line({10, 0}, {10, 4}),           // 2nd, reversed direction below
        line({0, 0}, {10, 0}),            // 1st
        arc({10, 6}, 2, 3 * kPi / 2, 0),  // wait: from (10,4) to (12,6)
        poly(pts({{12, 6}, {12, 10}, {20, 10}}), false),
    };
    auto r = joinEntities(ptrs(v));
    REQUIRE(r.created.size() == 1);
    CHECK(r.consumed.size() == 4);
    REQUIRE(kind(r.created[0]) == EntityKind::Polyline);
    const auto& pl = as<PolylineData>(r.created[0]);
    CHECK_FALSE(pl.closed);
    CHECK(pl.v.size() == 6);
    // Length: 10 + 4 + quarter circle (r=2) + 4 + 8.
    CHECK(near(geom::length(pl), 10 + 4 + kPi + 4 + 8, 1e-6));
    CHECK(r.created[0].common.layer == LayerId{7});
}

TEST_CASE("join a loop closes the polyline - other entities are left alone") {
    std::vector<Entity> v{line({0, 0}, {5, 0}), line({5, 0}, {5, 5}), line({5, 5}, {0, 0}),
                          circle({50, 50}, 3)};
    auto r = joinEntities(ptrs(v));
    REQUIRE(r.created.size() == 1);
    CHECK(as<PolylineData>(r.created[0]).closed);
    CHECK(as<PolylineData>(r.created[0]).v.size() == 3);
    CHECK(r.consumed == std::vector<std::size_t>{0, 1, 2});
    // Gap bigger than the tolerance: nothing joins; within a given tolerance: joins.
    std::vector<Entity> gap{line({0, 0}, {5, 0}), line({5.01, 0}, {5, 5})};
    CHECK(joinEntities(ptrs(gap)).created.empty());
    CHECK(joinEntities(ptrs(gap), 0.05).created.size() == 1);
}

TEST_CASE("join of thousands of pieces finds every partner, in any order") {
    // A zig-zag of 3000 segments, and 400 collinear pieces on another line, shuffled together
    // with 1500 lines at all angles that touch nothing.
    std::vector<Entity> v;
    for (int i = 0; i < 3000; ++i)
        v.push_back(line({i * 2.0, (i % 2) * 1.5}, {(i + 1) * 2.0, ((i + 1) % 2) * 1.5}));
    for (int i = 0; i < 400; ++i)
        v.push_back(line({i * 3.0, -50}, {i * 3.0 + 1.0, -50}));  // gaps between: bridged
    for (int i = 0; i < 1500; ++i) {
        const double a = i * 0.0137;
        v.push_back(line({10000.0 + i * 40.0, 100.0},
                         {10000.0 + i * 40.0 + 9 * std::cos(a), 100.0 + 9 * std::sin(a)}));
    }
    std::mt19937 rng(5);
    std::shuffle(v.begin(), v.end(), rng);
    auto r = joinEntities(ptrs(v), 1e-3, true);
    // one polyline for the zig-zag, one line for the collinear pieces
    std::size_t polys = 0, lines = 0, pieces = 0;
    for (const Entity& e : r.created) {
        if (kind(e) == EntityKind::Polyline) {
            ++polys;
            pieces = as<PolylineData>(e).v.size();
        } else if (kind(e) == EntityKind::Line) {
            ++lines;
        }
    }
    CHECK(polys == 1);
    CHECK(pieces == 3001);
    CHECK(lines == 1);
    CHECK(r.consumed.size() == 3400);
}

TEST_CASE("joining 30000 separate parallel lines fuses only the overlapping ones, quickly") {
    std::vector<Entity> v;
    // 30000 short lines in one direction, none touching another.
    for (int i = 0; i < 30000; ++i) {
        const double x = (i % 300) * 30.0, y = (i / 300) * 30.0;
        v.push_back(line({x, y}, {x + 20, y + 10}));
    }
    // 100 collinear pairs that overlap, and 100 that only touch, far from the rest.
    for (int k = 0; k < 200; ++k) {
        const double y = 100000.0 + k * 50.0;
        v.push_back(line({0, y}, {10, y + 5}));
        v.push_back(k < 100 ? line({6, y + 3}, {20, y + 10}) : line({10, y + 5}, {20, y + 10}));
    }
    const auto t = std::chrono::steady_clock::now();
    const auto bridged = joinEntities(ptrs(v), 1e-4, true);
    const auto plain = joinEntities(ptrs(v), 1e-4, false);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    CHECK(plain.consumed.size() == 400);
    CHECK(plain.created.size() == 200);
    // Bridging also fuses lines on one line with a gap between them, which the grid has plenty of.
    CHECK(bridged.consumed.size() > 400);
#ifdef NDEBUG
    CHECK(secs < 5.0);
#else
    (void)secs;
#endif
}

TEST_CASE("a spline absorbs lines at its ends, fit-only splines included") {
    SplineData ctrl;
    ctrl.degree = 3;
    ctrl.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    ctrl.control = {{0, 0}, {5, 10}, {10, -10}, {20, 0}};
    SplineData fit;
    fit.degree = 3;
    fit.fit = {{0, 0}, {5, 6}, {10, -4}, {15, 5}, {20, 0}};
    for (const SplineData& s : {ctrl, fit}) {
        Entity sp;
        sp.data = Indirect<SplineData>(s);
        // Lines at both ends, and an unrelated one.
        std::vector<Entity> v = {sp, line({20, 0}, {30, 5}), line({-8, 3}, {0, 0}),
                                 line({100, 100}, {110, 100})};
        auto r = joinEntities(ptrs(v));
        REQUIRE(r.created.size() == 1);
        CHECK(r.consumed == std::vector<std::size_t>{0, 1, 2});
        const auto* joined = std::get_if<Indirect<SplineData>>(&r.created[0].data);
        REQUIRE(joined);
        const SplineData& j = **joined;
        CHECK(near(j.control.front(), {-8, 3}));
        CHECK(near(j.control.back(), {30, 5}));
        CHECK(j.knots.size() == j.control.size() + static_cast<std::size_t>(j.degree) + 1);
    }
    // Two collinear-chain lines touching a spline join each other first, as before.
    Entity sp;
    sp.data = Indirect<SplineData>(ctrl);
    std::vector<Entity> v = {sp, line({40, 0}, {50, 0}), line({50, 0}, {60, 0})};
    auto r = joinEntities(ptrs(v));
    CHECK(r.consumed == std::vector<std::size_t>{1, 2});
}

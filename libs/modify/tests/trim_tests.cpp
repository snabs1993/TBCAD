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

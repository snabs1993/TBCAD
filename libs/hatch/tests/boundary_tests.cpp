#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "helpers.h"
#include "tbcad/hatch/boundary.h"

using namespace tbcad;
using namespace tbcad::hatch;
using namespace tbcad::hatch::testing;

namespace {

template <class T>
std::size_t countEdges(const HatchLoop& loop) {
    return static_cast<std::size_t>(
        std::count_if(loop.edges.begin(), loop.edges.end(),
                      [](const HatchEdge& e) { return std::holds_alternative<T>(e); }));
}

// Signed area of a loop made of line edges only.
double lineLoopArea(const HatchLoop& loop) {
    double a = 0;
    for (const HatchEdge& e : loop.edges) {
        const auto& l = std::get<HatchLineEdge>(e);
        a += cross(l.a, l.b);
    }
    return a * 0.5;
}

}  // namespace

TEST_CASE("pick inside a rectangle gives one loop of four edges") {
    Scene s;
    s.rectLines({0, 0}, {100, 60});
    auto r = findBoundary(s.candidates(), {50, 30});
    REQUIRE(r.ok());
    const Boundary& b = r.value();
    REQUIRE(b.loops.size() == 1);
    CHECK(b.loops[0].outer);
    CHECK(b.loops[0].edges.size() == 4);
    CHECK(countEdges<HatchLineEdge>(b.loops[0]) == 4);
    CHECK(lineLoopArea(b.loops[0]) == doctest::Approx(6000));  // counter-clockwise
    CHECK(b.sources.size() == 4);
    // The loop is a chain: every edge starts where the previous one ended.
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& a = std::get<HatchLineEdge>(b.loops[0].edges[i]);
        const auto& n = std::get<HatchLineEdge>(b.loops[0].edges[(i + 1) % 4]);
        CHECK(distance(a.b, n.a) < 1e-9);
    }
}

TEST_CASE("a closed polyline entity is a boundary on its own") {
    Scene s;
    s.add(rectPolyline({0, 0}, {10, 10}));
    auto r = findBoundary(s.candidates(), {5, 5});
    REQUIRE(r.ok());
    CHECK(r.value().loops.size() == 1);
    CHECK(r.value().loops[0].edges.size() == 4);
    CHECK(r.value().sources == std::vector<EntityId>{EntityId{1}});
}

TEST_CASE("a circle inside a rectangle is an island - picking inside the circle fills the circle") {
    Scene s;
    s.rectLines({0, 0}, {100, 100});
    s.add(circle({50, 50}, 20));
    auto ring = findBoundary(s.candidates(), {10, 10});
    REQUIRE(ring.ok());
    REQUIRE(ring.value().loops.size() == 2);
    CHECK(ring.value().loops[0].outer);
    CHECK(ring.value().loops[0].edges.size() == 4);
    CHECK_FALSE(ring.value().loops[1].outer);
    REQUIRE(ring.value().loops[1].edges.size() == 1);
    const auto* a = std::get_if<HatchArcEdge>(&ring.value().loops[1].edges[0]);
    REQUIRE(a != nullptr);
    CHECK(a->radius == doctest::Approx(20));
    CHECK(a->startAngle == doctest::Approx(a->endAngle));  // full circle

    auto disc = findBoundary(s.candidates(), {50, 50});
    REQUIRE(disc.ok());
    CHECK(disc.value().loops.size() == 1);
    CHECK(disc.value().sources == std::vector<EntityId>{EntityId{5}});
}

TEST_CASE("concentric rings alternate hole and filled (Normal style)") {
    Scene s;
    s.add(circle({0, 0}, 30));
    s.add(circle({0, 0}, 20));
    s.add(circle({0, 0}, 10));
    auto r = findBoundary(s.candidates(), {25, 0});
    REQUIRE(r.ok());
    REQUIRE(r.value().loops.size() == 3);
    CHECK(r.value().loops[0].outer);
    CHECK_FALSE(r.value().loops[1].outer);  // r = 20: a hole
    CHECK(r.value().loops[2].outer);        // r = 10: filled again
    // Between the middle and the inner circle the answer is the middle ring with the inner hole.
    auto mid = findBoundary(s.candidates(), {15, 0});
    REQUIRE(mid.ok());
    CHECK(mid.value().loops.size() == 2);
}

TEST_CASE("overlapping shapes: the smallest face around the point wins") {
    Scene s;
    s.rectLines({0, 0}, {60, 60});
    s.rectLines({40, 40}, {100, 100});
    auto lens = findBoundary(s.candidates(), {50, 50});
    REQUIRE(lens.ok());
    REQUIRE(lens.value().loops.size() == 1);
    CHECK(lens.value().loops[0].edges.size() == 4);
    CHECK(std::abs(lineLoopArea(lens.value().loops[0])) == doctest::Approx(400));  // 20 x 20

    auto l = findBoundary(s.candidates(), {10, 10});
    REQUIRE(l.ok());
    REQUIRE(l.value().loops.size() == 1);
    CHECK(l.value().loops[0].edges.size() == 6);  // an L shape
    CHECK(lineLoopArea(l.value().loops[0]) == doctest::Approx(3600 - 400));
}

TEST_CASE("a line that divides a rectangle makes two faces") {
    Scene s;
    s.rectLines({0, 0}, {100, 50});
    s.add(line({40, -10}, {40, 60}));  // overshoots both sides
    auto left = findBoundary(s.candidates(), {10, 10});
    auto right = findBoundary(s.candidates(), {90, 10});
    REQUIRE(left.ok());
    REQUIRE(right.ok());
    CHECK(lineLoopArea(left.value().loops[0]) == doctest::Approx(40 * 50));
    CHECK(lineLoopArea(right.value().loops[0]) == doctest::Approx(60 * 50));
}

TEST_CASE("T junctions, overshoot and antennas do not disturb the region") {
    Scene s;
    // The bottom side is made of two lines that meet in the middle.
    s.add(line({0, 0}, {50, 0}));
    s.add(line({50, 0}, {100, 0}));
    s.add(line({100, 0}, {100, 100}));
    s.add(line({100, 100}, {0, 100}));
    s.add(line({0, 100}, {0, -20}));  // overshoots below the corner
    s.add(line({30, 30}, {60, 30}));  // a dangling line inside
    s.add(line({0, 0}, {-30, 0}));    // and one outside
    auto r = findBoundary(s.candidates(), {70, 70});
    REQUIRE(r.ok());
    REQUIRE(r.value().loops.size() == 1);
    CHECK(lineLoopArea(r.value().loops[0]) == doctest::Approx(10000));
}

TEST_CASE("a circle joined to the frame by a bridge is still an island") {
    Scene s;
    s.rectLines({0, 0}, {100, 100});
    s.add(circle({50, 50}, 20));
    s.add(line({50, 70}, {50, 100}));  // bridge from the circle to the top side
    auto r = findBoundary(s.candidates(), {10, 10});
    REQUIRE(r.ok());
    REQUIRE(r.value().loops.size() == 2);
    CHECK(r.value().loops[0].edges.size() == 5);  // the top side is split by the bridge
    CHECK_FALSE(r.value().loops[1].outer);
}

TEST_CASE("arcs and lines: half disc, and internally tangent circles") {
    Scene s;
    s.add(arc({0, 0}, 10, 0, kPi));
    s.add(line({-10, 0}, {10, 0}));
    auto half = findBoundary(s.candidates(), {0, 5});
    REQUIRE(half.ok());
    CHECK(half.value().loops[0].edges.size() == 2);
    CHECK(countEdges<HatchArcEdge>(half.value().loops[0]) == 1);
    CHECK_FALSE(findBoundary(s.candidates(), {0, -5}).ok());

    Scene t;
    t.add(circle({0, 0}, 10));
    t.add(circle({5, 0}, 5));  // touches the big circle from inside at (10, 0)
    auto crescent = findBoundary(t.candidates(), {-5, 0});
    REQUIRE(crescent.ok());
    REQUIRE(crescent.value().loops.size() == 2);
    CHECK(crescent.value().loops[0].outer);
    CHECK_FALSE(crescent.value().loops[1].outer);
    auto small = findBoundary(t.candidates(), {5, 0});
    REQUIRE(small.ok());
    CHECK(small.value().loops.size() == 1);
}

TEST_CASE("a polyline with bulges contributes arc edges") {
    Scene s;
    PolylineData pl;
    pl.closed = true;
    pl.v = {{{0, 0}, 0, 0, 0}, {{20, 0}, 1.0, 0, 0}, {{20, 20}, 0, 0, 0}, {{0, 20}, 0, 0, 0}};
    s.add(ent(std::move(pl)));  // the right side bulges outwards by a half circle
    auto r = findBoundary(s.candidates(), {10, 10});
    REQUIRE(r.ok());
    CHECK(r.value().loops[0].edges.size() == 4);
    CHECK(countEdges<HatchArcEdge>(r.value().loops[0]) == 1);
    auto out = findBoundary(s.candidates(), {25, 10});  // inside the bulge
    REQUIRE(out.ok());
}

TEST_CASE("errors: nothing closed, no geometry, too many edges") {
    Scene open;
    open.add(line({0, 0}, {10, 0}));
    open.add(line({10, 0}, {10, 10}));
    open.add(line({10, 10}, {0, 10}));  // the fourth side is missing
    auto r = findBoundary(open.candidates(), {5, 5});
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("closed boundary") != std::string::npos);

    Scene point;
    point.add(ent(PointData{{1, 1}}));
    CHECK_FALSE(findBoundary(point.candidates(), {0, 0}).ok());
    CHECK_FALSE(findBoundary({}, {0, 0}).ok());

    Scene many;
    for (int i = 0; i < 300; ++i)
        many.add(line({0.0, i * 1.0}, {100.0, i * 1.0}));
    BoundaryOptions opt;
    opt.maxEdges = 200;
    auto guard = findBoundary(many.candidates(), {50, 50.5}, opt);
    REQUIRE_FALSE(guard.ok());
    CHECK(guard.error().code == ErrorCode::OutOfRange);
    CHECK(guard.error().message.find("zoom in") != std::string::npos);
    // The same drawing is fine with the default limit (and has no closed region).
    CHECK(findBoundary(many.candidates(), {50, 50.5}).error().code == ErrorCode::Generic);
}

TEST_CASE("the edge guard counts edges after splitting, not entities") {
    Scene s;
    for (int i = 0; i <= 12; ++i) {  // 13 x 13 lines make 2 * 13 * 12 = 312 edges
        s.add(line({0.0, i * 10.0}, {120.0, i * 10.0}));
        s.add(line({i * 10.0, 0.0}, {i * 10.0, 120.0}));
    }
    BoundaryOptions opt;
    opt.maxEdges = 300;
    CHECK_FALSE(findBoundary(s.candidates(), {5, 5}, opt).ok());
    opt.maxEdges = 400;
    auto r = findBoundary(s.candidates(), {5, 5}, opt);
    REQUIRE(r.ok());
    CHECK(r.value().graphEdges == 312);
    CHECK(lineLoopArea(r.value().loops[0]) == doctest::Approx(100));
}

TEST_CASE("select-objects mode: closed entities, nesting and separate shapes") {
    Scene s;
    s.add(circle({0, 0}, 10));
    auto one = boundaryFromObjects(s.candidates());
    REQUIRE(one.ok());
    REQUIRE(one.value().size() == 1);
    CHECK(one.value()[0].loops.size() == 1);

    Scene nested;
    nested.add(rectPolyline({0, 0}, {100, 100}));
    nested.add(circle({50, 50}, 20));
    auto n = boundaryFromObjects(nested.candidates());
    REQUIRE(n.ok());
    REQUIRE(n.value().size() == 1);
    REQUIRE(n.value()[0].loops.size() == 2);
    CHECK(n.value()[0].loops[0].outer);
    CHECK_FALSE(n.value()[0].loops[1].outer);

    Scene two;
    two.add(rectPolyline({0, 0}, {10, 10}));
    two.add(rectPolyline({50, 0}, {60, 10}));
    auto t = boundaryFromObjects(two.candidates());
    REQUIRE(t.ok());
    REQUIRE(t.value().size() == 2);
    CHECK(t.value()[0].sources == std::vector<EntityId>{EntityId{1}});  // sorted left to right
    CHECK(t.value()[1].sources == std::vector<EntityId>{EntityId{2}});

    Scene lines;  // four loose lines that close up
    lines.rectLines({0, 0}, {10, 10});
    auto l = boundaryFromObjects(lines.candidates());
    REQUIRE(l.ok());
    CHECK(l.value().size() == 1);

    Scene none;
    none.add(line({0, 0}, {1, 1}));
    CHECK_FALSE(boundaryFromObjects(none.candidates()).ok());
}

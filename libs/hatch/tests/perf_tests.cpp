#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <random>

#include "helpers.h"
#include "tbcad/hatch/boundary.h"

using namespace tbcad;
using namespace tbcad::hatch;
using namespace tbcad::hatch::testing;

namespace {

double ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace

// Boundary detection among 5,000 visible entities. The budget is 300 ms in an optimised build;
// debug builds (and sanitizers) are several times slower, so they only get a sanity limit.
TEST_CASE("boundary detection among 5,000 visible entities") {
    Scene s;
    s.rectLines({-10, -10}, {5010, 2510});  // the frame: 4 entities
    // 2,498 cells: a closed square with a circle inside it = 4,996 entities.
    for (int ix = 0; ix < 71; ++ix) {
        for (int iy = 0; iy < 36; ++iy) {
            if (s.size() >= 5000 - 2)
                break;
            double x = ix * 70.0, y = iy * 70.0;
            s.add(rectPolyline({x, y}, {x + 60, y + 60}));
            s.add(circle({x + 30, y + 30}, 20));
        }
    }
    s.add(line({0, 2600}, {5000, 2600}));  // loose lines that cross nothing
    s.add(line({0, 2700}, {5000, 2700}));
    REQUIRE(s.size() == 5000);
    auto cands = s.candidates();

    // Pick in the ring of a cell in the middle (square with circle hole).
    Vec2 pick{35 * 70.0 + 5, 18 * 70.0 + 5};
    double best = 1e9;
    std::size_t edges = 0;
    for (int run = 0; run < 3; ++run) {
        auto t0 = std::chrono::steady_clock::now();
        auto r = findBoundary(cands, pick);
        auto t1 = std::chrono::steady_clock::now();
        REQUIRE(r.ok());
        REQUIRE(r.value().loops.size() == 2);
        CHECK(r.value().loops[0].edges.size() == 4);
        CHECK_FALSE(r.value().loops[1].outer);
        edges = r.value().graphEdges;
        best = std::min(best, ms(t0, t1));
    }
    std::printf("[hatch perf] boundary among %zu entities (%zu graph edges): %.1f ms\n", s.size(),
                edges, best);
#ifdef NDEBUG
    CHECK(best < 300.0);
#else
    CHECK(best < 5000.0);
#endif
}

TEST_CASE("5,000 random short lines in a crowded view hit the edge guard instead of hanging") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> pos(0, 1000), ang(0, kTwoPi);
    Scene s;
    for (int i = 0; i < 5000; ++i) {
        Vec2 a{pos(rng), pos(rng)};
        double t = ang(rng);
        s.add(line(a, a + Vec2{std::cos(t), std::sin(t)} * 80));
    }
    auto t0 = std::chrono::steady_clock::now();
    auto r = findBoundary(s.candidates(), {500, 500});
    auto t1 = std::chrono::steady_clock::now();
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::OutOfRange);
    std::printf("[hatch perf] crowded view rejected in %.1f ms\n", ms(t0, t1));
    CHECK(ms(t0, t1) < 10000.0);
}

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

// A city of 200,000 lines at survey coordinates: picking a block touches only the lines near the
// pick, whatever the size of the drawing.
TEST_CASE("pick-point boundary in a 200,000-line drawing at survey coordinates") {
    const Vec2 o{500123.456, 4500987.654};
    const int n = 224;  // 224 x 224 blocks of 4 lines
    const double pitch = 120, size = 100;
    std::vector<Entity> lines;
    lines.reserve(static_cast<std::size_t>(n) * n * 4 + 100);
    std::vector<std::vector<std::size_t>> bucket(static_cast<std::size_t>(n) * n);
    for (int ix = 0; ix < n; ++ix) {
        for (int iy = 0; iy < n; ++iy) {
            const Vec2 lo = o + Vec2{ix * pitch, iy * pitch}, hi = lo + Vec2{size, size};
            const Vec2 c[] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
            for (int k = 0; k < 4; ++k) {
                bucket[static_cast<std::size_t>(ix * n + iy)].push_back(lines.size());
                lines.push_back(line(c[k], c[(k + 1) % 4]));
            }
        }
    }
    auto window = [&](const BBox2& box) {
        std::vector<Candidate> out;
        const int x0 = std::max(0, static_cast<int>(std::floor((box.min.x - o.x) / pitch)) - 1);
        const int x1 = std::min(n - 1, static_cast<int>(std::floor((box.max.x - o.x) / pitch)) + 1);
        const int y0 = std::max(0, static_cast<int>(std::floor((box.min.y - o.y) / pitch)) - 1);
        const int y1 = std::min(n - 1, static_cast<int>(std::floor((box.max.y - o.y) / pitch)) + 1);
        for (int ix = x0; ix <= x1; ++ix)
            for (int iy = y0; iy <= y1; ++iy)
                for (std::size_t i : bucket[static_cast<std::size_t>(ix * n + iy)])
                    out.push_back({EntityId{i + 1}, &lines[i]});
        return out;
    };
    const BBox2 full{o - Vec2{10, 10}, o + Vec2{n * pitch + 10, n * pitch + 10}};
    std::mt19937 rng(11);
    double worst = 0;
    for (int k = 0; k < 20; ++k) {
        const int ix = static_cast<int>(rng() % n), iy = static_cast<int>(rng() % n);
        const Vec2 pick = o + Vec2{ix * pitch + 33.3, iy * pitch + 71.7};
        auto t0 = std::chrono::steady_clock::now();
        auto r = findBoundaryNear(window, full, pick);
        auto t1 = std::chrono::steady_clock::now();
        REQUIRE(r.result.ok());
        REQUIRE(r.result.value().loops.size() == 1);
        CHECK(r.result.value().loops[0].edges.size() == 4);
        worst = std::max(worst, ms(t0, t1));
    }
    // Between the blocks there is no closed region: the search must still end quickly.
    auto t0 = std::chrono::steady_clock::now();
    auto none = findBoundaryNear(window, full, o + Vec2{110, 110});
    auto t1 = std::chrono::steady_clock::now();
    CHECK_FALSE(none.result.ok());
    std::printf("[hatch perf] 200k lines: worst pick %.1f ms, pick in the street %.1f ms\n", worst,
                ms(t0, t1));
#ifdef NDEBUG
    CHECK(worst < 100.0);
    CHECK(ms(t0, t1) < 2000.0);
#else
    CHECK(worst < 3000.0);
    CHECK(ms(t0, t1) < 30000.0);
#endif
}

// A survey sheet full of loose ends (contour pieces, symbols) with a gap tolerance set: every
// free end looks for a curve within the gap, which must not compare it with every other piece.
TEST_CASE("gap tolerance among 20,000 loose pieces stays quick") {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> pos(0, 4000), ang(0, kTwoPi);
    Scene s;
    for (int i = 0; i < 20000; ++i) {
        const Vec2 a{pos(rng), pos(rng)};
        const double t = ang(rng);
        s.add(line(a, a + Vec2{std::cos(t), std::sin(t)} * 3));
    }
    // A room with a 0.5 gap in one corner, away from the loose pieces.
    s.add(line({5000, 0}, {5100, 0}));
    s.add(line({5100, 0}, {5100, 100}));
    s.add(line({5100, 100}, {5000, 100}));
    s.add(line({5000, 100}, {5000, 0.5}));
    BoundaryOptions bo;
    bo.gapTolerance = 1;
    bo.maxEdges = 100000;
    auto t0 = std::chrono::steady_clock::now();
    auto r = findBoundary(s.candidates(), {5050, 50}, bo);
    auto t1 = std::chrono::steady_clock::now();
    REQUIRE(r.ok());
    CHECK(r.value().loops.size() == 1);
    std::printf("[hatch perf] gap tolerance among 20k loose pieces: %.1f ms\n", ms(t0, t1));
#ifdef NDEBUG
    CHECK(ms(t0, t1) < 300.0);
#endif
}

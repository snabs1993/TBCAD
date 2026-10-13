// Performance gates from the workstream spec (see TBCAD_FULL_SCALE in test_util.h); the numbers
// are printed so they can be recorded in the PR.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

#include "tbcad/index/spatial_index.h"
#include "test_util.h"

using namespace tbcad;
using namespace tbcad::index;
using Clock = std::chrono::steady_clock;

namespace {
double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
}  // namespace

TEST_CASE("bulk load of 1 M boxes and a viewport query") {
    constexpr int kN = 1000000;
    // Drawing-like data: 1 M small boxes spread over a 1 km x 1 km sheet (about 1 box per m^2,
    // 1-2 m wide). A 100 m x 100 m viewport then holds about 10,000 of them.
    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<double> pos(0, 1e6);      // mm
    std::uniform_real_distribution<double> size(200, 2000);  // mm
    std::vector<IdBox> items;
    items.reserve(kN);
    for (int i = 1; i <= kN; ++i) {
        double x = pos(rng), y = pos(rng), w = size(rng), h = size(rng);
        items.emplace_back(EntityId{static_cast<std::uint64_t>(i)}, BBox2{{x, y}, {x + w, y + h}});
    }

    double bestLoad = 1e9;
    SpatialIndex idx;
    for (int rep = 0; rep < 3; ++rep) {
        auto t0 = Clock::now();
        idx.bulkLoad(items);
        bestLoad = std::min(bestLoad, ms(t0, Clock::now()));
    }
    REQUIRE(idx.size() == static_cast<std::size_t>(kN));
    CHECK(idx.validate());

    BBox2 view{{450000, 450000}, {550000, 550000}};  // 100 m x 100 m
    std::size_t hits = 0;
    double bestQuery = 1e9;
    for (int rep = 0; rep < 20; ++rep) {
        std::size_t count = 0;
        auto t0 = Clock::now();
        idx.query(view, [&](EntityId, const BBox2&) { ++count; });
        bestQuery = std::min(bestQuery, ms(t0, Clock::now()));
        hits = count;
    }
    std::printf("[index perf] bulkLoad 1M: %.1f ms, viewport query (%zu hits): %.3f ms\n", bestLoad,
                hits, bestQuery);
    CHECK(hits > 8000);
    CHECK(hits < 12000);
#ifdef TBCAD_FULL_SCALE
    CHECK(bestLoad < 300.0);
    CHECK(bestQuery < 2.0);
#endif
}

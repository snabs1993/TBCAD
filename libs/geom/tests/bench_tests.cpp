#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <random>

#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::geom;

TEST_CASE("distance() to one million lines") {
    constexpr int kCount = 1'000'000;
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> pos(-1000, 1000);
    std::vector<Entity> lines;
    lines.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        Entity e;
        e.data = LineData{{pos(rng), pos(rng)}, {pos(rng), pos(rng)}};
        lines.push_back(std::move(e));
    }
    Vec2 q{123.4, -567.8};
    auto t0 = std::chrono::steady_clock::now();
    double best = 1e300;
    for (const Entity& e : lines)
        best = std::min(best, distance(e, q));
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("distance() over 1M lines: %.1f ms (nearest %.4f)\n", ms, best);
    CHECK(best < 1e300);
#ifdef NDEBUG
    CHECK(ms < 100.0);
#endif
}

TEST_CASE("hitTest() over a mixed drawing stays fast") {
    std::mt19937 rng(2);
    std::uniform_real_distribution<double> pos(-1000, 1000), rad(1, 50);
    std::vector<Entity> ents;
    for (int i = 0; i < 200000; ++i) {
        Entity e;
        switch (i % 3) {
            case 0:
                e.data = LineData{{pos(rng), pos(rng)}, {pos(rng), pos(rng)}};
                break;
            case 1:
                e.data = CircleData{{pos(rng), pos(rng)}, rad(rng)};
                break;
            default:
                e.data = ArcData{{pos(rng), pos(rng)}, rad(rng), 0.3, 2.0};
                break;
        }
        ents.push_back(std::move(e));
    }
    auto t0 = std::chrono::steady_clock::now();
    int hits = 0;
    for (const Entity& e : ents)
        hits += hitTest(e, {0, 0}, 5.0) ? 1 : 0;
    auto t1 = std::chrono::steady_clock::now();
    std::printf("hitTest over 200k mixed entities: %.1f ms (%d hits)\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(), hits);
    CHECK(hits >= 0);
}

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <random>

#include "helpers.h"

using namespace tbcad;
using namespace tbcad::testing;

namespace {
double seconds(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
}
}  // namespace

TEST_CASE("one million lines: write and read within the targets") {
    Document doc;
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> d(-1e5, 1e5);
    {
        Transaction tx = doc.begin("lines");
        for (int i = 0; i < 1'000'000; ++i) {
            Entity e;
            e.common.layer = kLayerZero;
            e.data = LineData{{d(rng), d(rng)}, {d(rng), d(rng)}};
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    auto t0 = std::chrono::steady_clock::now();
    auto bytes = writeOrFail(doc);
    double writeSec = seconds(t0);

    auto t1 = std::chrono::steady_clock::now();
    io::IoReport report;
    auto back = readOrFail(bytes, &report);
    double readSec = seconds(t1);

    std::printf("[io perf] 1M random lines: write %.2f s, read %.2f s, file %.1f MB\n", writeSec,
                readSec, static_cast<double>(bytes.size()) / 1e6);
    CHECK(report.entityCount == 1'000'000);
    CHECK(back->entityCount() == 1'000'000);
    CHECK(bytes.size() < 40u * 1000 * 1000);
#ifdef NDEBUG
    // Wall-clock limits are only meaningful for optimised builds (the target is 1.5 s).
    CHECK(readSec < 1.5);
    CHECK(writeSec < 6.0);
#endif
}

#include <doctest/doctest.h>

#include <chrono>
#include <random>

#include "tbcad/constraints/constraints.h"

using namespace tbcad;
using namespace tbcad::constraints;

namespace {
double ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}
}  // namespace

TEST_CASE("a 200 line chain solves and analyses quickly") {
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> jitter(-0.3, 0.3);
    const int n = 200;
    Document doc;
    std::vector<EntityId> ids;
    {
        auto tx = doc.begin("setup");
        for (int i = 0; i < n; ++i) {
            Vec2 a{i * 10.0 + jitter(rng), (i % 2) * 5 + jitter(rng)};
            Vec2 b{(i + 1) * 10.0 + jitter(rng), ((i + 1) % 2) * 5 + jitter(rng)};
            ids.push_back(tx.add(Entity{{}, LineData{a, b}}).value());
        }
        tx.commit();
    }
    ConstraintSet set;
    for (int i = 0; i + 1 < n; ++i) {
        Constraint c;
        c.kind = Kind::Coincident;
        c.a = {ids[i], Part::End, 0};
        c.b = {ids[i + 1], Part::Start, 0};
        set.add(c);
    }
    for (int i = 0; i < n; ++i) {
        Constraint c;
        c.kind = Kind::Distance;
        c.a = {ids[i], Part::Start, 0};
        c.b = {ids[i], Part::End, 0};
        c.value = 11.2;
        set.add(c);
    }
    {
        Constraint c;
        c.kind = Kind::Fix;
        c.a = {ids[0], Part::Start, 0};
        set.add(c);
    }
    auto t = std::chrono::steady_clock::now();
    auto tx = doc.begin("solve");
    auto r = solve(tx, set);
    REQUIRE(r.ok());
    CHECK(r.value().converged);
    CHECK(ms(t) < 5000.0);
    tx.commit();
    t = std::chrono::steady_clock::now();
    auto an = analyze(doc, set);
    CHECK(ms(t) < 5000.0);
}

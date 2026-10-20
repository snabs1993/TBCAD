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

namespace {
// A sketch-like scene: rectangles sharing corners, tilted squares, circles touching lines and each
// other, arcs, with a little noise so tolerances matter.
std::vector<EntityId> sketchScene(Document& doc, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<EntityId> ids;
    auto tx = doc.begin("setup");
    const auto line = [&](Vec2 a, Vec2 b) {
        ids.push_back(tx.add(Entity{{}, LineData{a, b}}).value());
    };
    const int side = static_cast<int>(std::sqrt(n / 4.0)) + 1;
    for (int k = 0; static_cast<int>(ids.size()) < n; ++k) {
        const double x = (k % side) * 20.0, y = (k / side) * 20.0;
        const double e = u(rng) < 0.5 ? 0.0 : 0.004;
        switch (k % 5) {
            case 0:
            case 1:
                line({x, y}, {x + 10, y + e});
                line({x + 10, y + e}, {x + 10, y + 7});
                line({x + 10, y + 7}, {x, y + 7});
                line({x, y + 7}, {x, y});
                break;
            case 2: {
                const double a = u(rng) * 1.5;
                const Vec2 d{std::cos(a) * 6, std::sin(a) * 6}, p{-d.y, d.x};
                line({x, y}, Vec2{x, y} + d);
                line(Vec2{x, y} + d, Vec2{x, y} + d + p);
                line({x + 1, y + 12}, Vec2{x + 1, y + 12} + d * 0.5);
                break;
            }
            case 3:
                line({x, y}, {x + 12, y});
                ids.push_back(tx.add(Entity{{}, CircleData{{x + 6, y + 3}, 3}}).value());
                ids.push_back(tx.add(Entity{{}, CircleData{{x + 12, y + 3}, 3}}).value());
                break;
            default:
                ids.push_back(tx.add(Entity{{}, ArcData{{x + 5, y + 5}, 4, 0.1, 2.0}}).value());
                ids.push_back(tx.add(Entity{{}, CircleData{{x + 5, y + 5}, 4}}).value());
                ids.push_back(tx.add(Entity{{}, PointData{{x + 5, y + 5}}}).value());
                break;
        }
    }
    tx.commit();
    return ids;
}

}  // namespace

TEST_CASE("autoConstrain scales to thousands of objects") {
    Document doc;
    const auto ids = sketchScene(doc, 5000, 3);
    const auto t = std::chrono::steady_clock::now();
    const auto found = autoConstrain(doc, ids, ConstraintSet{});
    const double took = ms(t);
    MESSAGE("5000 objects: " << found.size() << " constraints in " << took << " ms");
    CHECK(found.size() > 5000);
#ifdef NDEBUG
    CHECK(took < 600.0);  // every pair tested took about 1 s here
#endif
    // Proposing again with what was found already in place gives nothing new.
    ConstraintSet have;
    for (const Constraint& c : found)
        have.add(c);
    CHECK(autoConstrain(doc, ids, have).empty());
}

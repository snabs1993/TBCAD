#include <doctest/doctest.h>

#include <algorithm>
#include <random>

#include "helpers.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/boundary.h"

using namespace tbcad;
using namespace tbcad::hatch;
using namespace tbcad::hatch::testing;

namespace {

BoundaryOptions gap(double g) {
    BoundaryOptions o;
    o.gapTolerance = g;
    return o;
}

}  // namespace

TEST_CASE("a gap between two line ends is closed with a connector when within tolerance") {
    Scene s;
    s.add(line({0, 0}, {100, 0}));
    s.add(line({100, 0}, {100, 60}));
    s.add(line({100, 60}, {0, 60}));
    s.add(line({0, 60}, {0, 0.5}));  // stops 0.5 short of the first line's end
    CHECK_FALSE(findBoundary(s.candidates(), {50, 30}).ok());
    CHECK_FALSE(findBoundary(s.candidates(), {50, 30}, gap(0.1)).ok());
    auto r = findBoundary(s.candidates(), {50, 30}, gap(1));
    REQUIRE(r.ok());
    CHECK(r.value().loops.size() == 1);
    CHECK(r.value().loops[0].edges.size() == 5);
    CHECK(r.value().sources.size() == 4);  // the connector has no entity
}

TEST_CASE("a free end that stops short of the side of another line is joined to it") {
    Scene s;
    s.add(line({-50, 0}, {100, 0}));
    s.add(line({100, 0}, {100, 60}));
    s.add(line({100, 60}, {0, 60}));
    s.add(line({0, 60}, {0, 10}));
    CHECK_FALSE(findBoundary(s.candidates(), {50, 30}).ok());
    CHECK_FALSE(findBoundary(s.candidates(), {50, 30}, gap(5)).ok());
    auto r = findBoundary(s.candidates(), {50, 30}, gap(12));
    REQUIRE(r.ok());
    CHECK(r.value().loops[0].edges.size() == 5);
    CHECK(r.value().sources.size() == 4);
}

TEST_CASE("gap tolerance does not change a closed boundary") {
    Scene s;
    s.rectLines({0, 0}, {100, 60});
    auto a = findBoundary(s.candidates(), {50, 30});
    auto b = findBoundary(s.candidates(), {50, 30}, gap(2));
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.value().loops[0].edges.size() == b.value().loops[0].edges.size());
}

TEST_CASE("BoundaryFinder answers several picks from one graph") {
    Scene s;
    s.rectLines({0, 0}, {100, 60});
    s.rectLines({10, 10}, {30, 30});
    BoundaryFinder f(s.candidates());
    REQUIRE(f.ok());
    auto outer = f.find({60, 40});
    REQUIRE(outer.ok());
    CHECK(outer.value().loops.size() == 2);  // island
    auto inner = f.find({20, 20});
    REQUIRE(inner.ok());
    CHECK(inner.value().loops.size() == 1);
    CHECK_FALSE(f.find({500, 500}).ok());
    auto roots = f.outermost();
    REQUIRE(roots.ok());
    CHECK(roots.value().size() == 1);
}

TEST_CASE("sources of a boundary with islands include the island entities") {
    Scene s;
    s.rectLines({0, 0}, {100, 60});
    EntityId c = s.add(circle({50, 30}, 10));
    auto r = findBoundary(s.candidates(), {10, 10});
    REQUIRE(r.ok());
    CHECK(r.value().loops.size() == 2);
    CHECK(r.value().sources.size() == 5);
    CHECK(std::find(r.value().sources.begin(), r.value().sources.end(), c) !=
          r.value().sources.end());
}

// The windowed search must agree with a search over everything.
TEST_CASE("findBoundaryNear gives the same answer as findBoundary on random drawings") {
    std::mt19937_64 rng(4242);
    auto u = [&](double lo, double hi) {
        return lo + (hi - lo) * static_cast<double>(rng() % 100000) / 100000.0;
    };
    int same = 0, found = 0;
    for (int round = 0; round < 150; ++round) {
        Scene scene;
        for (int i = 0, n = 10 + static_cast<int>(rng() % 60); i < n; ++i) {
            switch (rng() % 4) {
                case 0:
                    scene.add(line({u(0, 400), u(0, 400)}, {u(0, 400), u(0, 400)}));
                    break;
                case 1:
                    scene.add(circle({u(0, 400), u(0, 400)}, u(5, 80)));
                    break;
                case 2: {
                    const double x = u(0, 350), y = u(0, 350);
                    scene.add(rectPolyline({x, y}, {x + u(5, 60), y + u(5, 60)}));
                    break;
                }
                default:
                    scene.add(arc({u(0, 400), u(0, 400)}, u(5, 80), u(0, 3), u(3.1, 6.2)));
            }
        }
        const auto all = scene.candidates();
        BBox2 full;
        for (const Candidate& c : all)
            full.merge(geom::bounds(*c.entity));
        const Vec2 pick{u(0, 400), u(0, 400)};
        auto ref = findBoundary(all, pick);
        auto near = findBoundaryNear(
            [&](const BBox2& w) {
                std::vector<Candidate> out;
                for (const Candidate& c : all)
                    if (w.intersects(geom::bounds(*c.entity)))
                        out.push_back(c);
                return out;
            },
            full, pick);
        if (ref.ok()) {
            ++found;
            REQUIRE(near.result.ok());
            CHECK(near.result.value().loops.size() == ref.value().loops.size());
            CHECK(near.result.value().sources.size() == ref.value().sources.size());
            same += near.result.value().loops.size() == ref.value().loops.size();
        } else {
            CHECK_FALSE(near.result.ok());
        }
    }
    CHECK(found > 20);
    CHECK(same == found);
}

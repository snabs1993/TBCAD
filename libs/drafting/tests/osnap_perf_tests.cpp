// Object snap on a big drawing: speed of one mouse move, and a property check that every snap point
// really lies on the geometry it claims (near the origin and at survey coordinates).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "tbcad/geom/entity.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;
using Clock = std::chrono::steady_clock;

namespace {

// A city-block style drawing of about `n` objects in a square `extent` mm wide starting at
// (`ox`, `oy`): mostly short lines, circles, arcs and bulged polylines, like a topographic survey.
void fill(Document& doc, std::size_t n, double ox, double oy, double extent, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> pos(0, extent), len(500, 6000), ang(0, kTwoPi),
        rad(100, 3000);
    auto tx = doc.begin("fill");
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 p{ox + pos(rng), oy + pos(rng)};
        const double a = ang(rng), l = len(rng);
        Entity e;
        switch (i % 12) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
                e.data = LineData{p, p + Vec2{std::cos(a), std::sin(a)} * l};
                break;
            case 5:
                e.data = CircleData{p, rad(rng)};
                break;
            case 6: {
                const double s = ang(rng);
                e.data = ArcData{p, rad(rng), s, s + 0.3 + ang(rng) * 0.4};
                break;
            }
            case 8: {
                const double r = rad(rng);
                e.data = EllipseData{p, Vec2{std::cos(a), std::sin(a)} * r, 0.5, 0, kTwoPi};
                break;
            }
            case 9: {
                SplineData sp;  // clamped cubic with five control points
                Vec2 q = p;
                for (int k = 0; k < 5; ++k) {
                    sp.control.push_back(q);
                    q += Vec2{std::cos(a + 0.3 * k), std::sin(a + 0.3 * k)} * (l * 0.25);
                }
                sp.knots = {0, 0, 0, 0, 0.5, 1, 1, 1, 1};
                e.data = std::move(sp);
                break;
            }
            case 10: {
                HatchLoop loop;  // a small box, like a hatched lot or building
                loop.edges.push_back(HatchLineEdge{p, p + Vec2{l, 0}});
                loop.edges.push_back(HatchLineEdge{p + Vec2{l, 0}, p + Vec2{l, l}});
                loop.edges.push_back(HatchLineEdge{p + Vec2{l, l}, p + Vec2{0, l}});
                loop.edges.push_back(HatchLineEdge{p + Vec2{0, l}, p});
                HatchData h;
                h.loops.push_back(std::move(loop));
                e.data = std::move(h);
                break;
            }
            default: {
                PolylineData pl;
                Vec2 q = p;
                for (int k = 0; k < 5; ++k) {
                    pl.v.push_back({q, k % 2 ? 0.0 : 0.4, 0, 0});
                    q += Vec2{std::cos(a + k), std::sin(a + k)} * (l * 0.3);
                }
                e.data = pl;
                break;
            }
        }
        REQUIRE(tx.add(std::move(e)).ok());
    }
    tx.commit();
}

double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Distance from `p` to the curves of entity `id`.
double distanceToEntity(const Document& doc, EntityId id, Vec2 p) {
    SnapGeometry g;  // what the entity offers to snap to (a hatch: its boundary)
    SnapGeometryRegistry::instance().collect(doc, *doc.find(id), g);
    double best = 1e300;
    for (const auto& c : g.curves)
        best = std::min(best, distance(c.closestPoint(p), p));
    return best;
}

void runBench(double ox, double oy, const char* label, double extent,
              std::initializer_list<double> pixels) {
    World w;
    constexpr std::size_t kN = 393000;
    fill(w.doc, kN, ox, oy, extent, 7);
    REQUIRE(w.doc.modelSpaceCount() == kN);
#if defined(NDEBUG)
    const int defaultMoves = 400;
#else
    const int defaultMoves = 60;
#endif
    const int kMoves =
        std::getenv("OSNAP_MOVES") ? std::atoi(std::getenv("OSNAP_MOVES")) : defaultMoves;
    for (const double pixel : pixels) {
        std::mt19937_64 rng(11);
        std::uniform_real_distribution<double> pos(0, extent);
        SnapRequest rq;
        rq.apertureMm = 10 * pixel;
        rq.modes = kAllSnapModes;
        double total = 0, worst = 0;
        std::size_t hits = 0;
        for (int i = 0; i < kMoves; ++i) {
            rq.cursor = {ox + pos(rng), oy + pos(rng)};
            rq.base = Vec2{ox + pos(rng), oy + pos(rng)};
            const auto t0 = Clock::now();
            const auto found = findSnapCandidates(w.doc, w.idx, rq);
            const double t = ms(t0, Clock::now());
            total += t;
            worst = std::max(worst, t);
            hits += found.size();
            // Every candidate is inside the aperture and lies on the geometry it names (an
            // intersection on both objects); coordinates up to 1e9 leave about 1e-7 of precision.
            const double tol = 1e-7 * (1 + std::abs(ox) + std::abs(oy)) + 1e-6;
            for (const auto& c : found) {
                REQUIRE(c.distance <= rq.apertureMm + 1e-6);
                // A centre, an insertion point and an apparent intersection are not on the curve.
                if (c.mode == SnapMode::Center || c.mode == SnapMode::Insertion ||
                    c.mode == SnapMode::ApparentIntersection)
                    continue;
                CHECK(distanceToEntity(w.doc, c.entity, c.point) < tol);
                if (c.mode == SnapMode::Intersection)
                    CHECK(distanceToEntity(w.doc, c.entity2, c.point) < tol);
            }
        }
        std::printf(
            "[osnap perf] %s (pixel %.0f mm): %zu objects, all modes: mean %.3f ms, worst "
            "%.3f ms per move (%.1f candidates)\n",
            label, pixel, kN, total / kMoves, worst, double(hits) / kMoves);
#if defined(NDEBUG)
        // The aim is under 1 ms on a spread-out drawing and under 5 ms on a pathologically dense
        // one (hundreds of objects per aperture, ellipses, splines and hatches among them); the
        // gates leave room for a busy CI machine.
        CHECK(total / kMoves < (extent > 1.0e6 ? 2.0 : 10.0));
#endif
    }
}

}  // namespace

TEST_CASE("object snap on 393k objects near the origin: speed and points on the geometry") {
    runBench(0, 0, "sparse, 2 km", 2.0e6, {100});
    runBench(0, 0, "dense, 200 m", 2.0e5, {100, 1000});
}

TEST_CASE("object snap on 393k objects at survey coordinates") {
    runBench(1.0e6, 2.5e6, "survey 1e6, 2 km", 2.0e6, {100});
    runBench(1.0e9, 4.0e9, "survey 1e9, dense", 2.0e5, {300});
}

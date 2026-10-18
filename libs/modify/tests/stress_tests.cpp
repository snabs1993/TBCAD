#include <chrono>
#include <random>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::modify;
using namespace tbcad::testing;

namespace {
std::vector<const Entity*> ptrs(const std::vector<Entity>& v) {
    std::vector<const Entity*> out;
    for (const auto& e : v)
        out.push_back(&e);
    return out;
}

// Every number of an entity's geometry is finite and no segment collapsed to nothing.
bool sane(const Entity& e) {
    std::vector<geom::CurveRef> cs;
    geom::curvesOf(e, cs);
    for (const auto& c : cs) {
        const Vec2 a = c.pointAt(c.t0());
        const Vec2 b = c.pointAt(c.isLine() ? 1.0 : c.t0() + c.sweep());
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) ||
            !std::isfinite(b.y))
            return false;
    }
    return std::isfinite(geom::length(e));
}

// A polyline that is deliberately awkward: repeated vertices, tiny segments, huge and tiny
// bulges, self-crossings.
Entity nastyPolyline(std::mt19937& rng, bool closed, bool degenerate = true) {
    std::uniform_real_distribution<double> coord(-30, 30), unit(0, 1);
    std::uniform_int_distribution<int> count(2, 9), pick(0, 5);
    std::vector<PolyVertex> v;
    const int n = count(rng);
    for (int i = 0; i < n; ++i) {
        PolyVertex p;
        p.p = {coord(rng), coord(rng)};
        if (degenerate && i > 0 && pick(rng) == 0)
            p.p = v.back().p;  // repeated vertex
        if (degenerate && i > 0 && pick(rng) == 1)
            p.p = v.back().p + Vec2{unit(rng) * 1e-5, 0};  // sliver
        switch (pick(rng)) {
            case 0:
                p.bulge = 0;
                break;
            case 1:
                p.bulge = (unit(rng) - 0.5) * 8;  // up to four times a semicircle
                break;
            case 2:
                p.bulge = 1e-9;
                break;
            case 3:
                p.bulge = unit(rng) - 0.5;
                break;
            default:
                break;
        }
        v.push_back(p);
    }
    return poly(std::move(v), closed);
}
}  // namespace

TEST_CASE("stress: offsetting awkward polylines never fails badly or hangs") {
    std::mt19937 rng(2024);
    std::uniform_real_distribution<double> dist(0.01, 12);
    int produced = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int iter = 0; iter < 600; ++iter) {
        const bool degenerate = iter % 2 == 1;
        const Entity src = nastyPolyline(rng, iter % 4 < 2, degenerate);
        for (double sign : {1.0, -1.0}) {
            OffsetOptions opt;
            opt.roundCorners = iter % 3 != 0;
            const double d = dist(rng);
            auto r = offsetEntity(src, sign * d, opt);
            if (!r.ok())
                continue;
            ++produced;
            for (const auto& e : r.value()) {
                CHECK(sane(e));
                CHECK(geom::length(e) > 0);
                // Whatever survives keeps its distance from the source.
                std::vector<geom::CurveRef> cs;
                geom::curvesOf(e, cs);
                for (const auto& c : cs)
                    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                        if (degenerate)
                            break;
                        const Vec2 q = c.pointAt(c.isLine() ? t : c.t0() + t * c.sweep());
                        const double far = geom::distance(src, q);
                        CHECK_MESSAGE(far >= d - 1e-4, "offset point ("
                                                           << q.x << ", " << q.y << ") is only "
                                                           << far << " from the source, wanted "
                                                           << d << " (iteration " << iter << ")");
                    }
            }
        }
    }
    CHECK(produced > 200);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    CHECK(ms < 20000);
}

TEST_CASE("stress: trim and extend against awkward edges stay finite") {
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> coord(-25, 25);
    int worked = 0;
    for (int iter = 0; iter < 500; ++iter) {
        Entity target;
        switch (iter % 4) {
            case 0:
                target = nastyPolyline(rng, iter % 8 == 0);
                break;
            case 1:
                target = arc({coord(rng), coord(rng)}, 2 + std::abs(coord(rng)), 0.2, 5.5);
                break;
            case 2:
                target = circle({coord(rng), coord(rng)}, 2 + std::abs(coord(rng)));
                break;
            default:
                target = line({coord(rng), coord(rng)}, {coord(rng), coord(rng)});
                break;
        }
        std::vector<Entity> edges;
        for (int k = 0; k < 3; ++k)
            edges.push_back(nastyPolyline(rng, k == 0));
        edges.push_back(circle({coord(rng), coord(rng)}, 3 + std::abs(coord(rng))));
        edges.push_back(arc({coord(rng), coord(rng)}, 3 + std::abs(coord(rng)), 0.1, 3.0));
        std::vector<geom::CurveRef> cs;
        geom::curvesOf(target, cs);
        if (cs.empty())
            continue;
        const Vec2 pick = cs[0].pointAt(cs[0].t0() + 0.4 * (cs[0].isLine() ? 1.0 : cs[0].sweep()));
        for (bool extendEdges : {false, true}) {
            TrimOptions opt;
            opt.extendEdges = extendEdges;
            for (int mode = 0; mode < 2; ++mode) {
                auto r = mode == 0 ? trimEntity(target, pick, ptrs(edges), opt)
                                   : extendEntity(target, pick, ptrs(edges), opt);
                if (!r.ok())
                    continue;
                ++worked;
                if (r.value().modified)
                    CHECK(sane(*r.value().modified));
                for (const auto& e : r.value().added)
                    CHECK(sane(e));
            }
        }
    }
    CHECK(worked > 100);
}

TEST_CASE("stress: offsets of simple star-shaped polylines keep their distance") {
    std::mt19937 rng(31337);
    std::uniform_real_distribution<double> unit(0, 1);
    int checked = 0, bad = 0, badStraight = 0, checkedStraight = 0;
    for (int iter = 0; iter < 400; ++iter) {
        const int n = 3 + iter % 8;
        std::vector<double> a;
        for (int i = 0; i < n; ++i)
            a.push_back(unit(rng) * 2 * 3.14159265358979);
        std::sort(a.begin(), a.end());
        const bool closed = iter % 4 != 3;
        {
            // A closed outline around the origin needs every gap between neighbours under half
            // a turn, otherwise the closing edge cuts across the others.
            double gap = a.front() + 2 * 3.14159265358979 - a.back();
            for (std::size_t i = 1; i < a.size(); ++i)
                gap = std::max(gap, a[i] - a[i - 1]);
            if (closed && gap > 3.0)
                continue;
        }
        std::vector<PolyVertex> v;
        for (double t : a) {
            const double r = 6 + 8 * unit(rng);
            PolyVertex p;
            p.p = {r * std::cos(t), r * std::sin(t)};
            p.bulge = (iter % 2 == 0) ? 0 : (unit(rng) - 0.5) * 0.4;  // gentle arcs
            v.push_back(p);
        }
        const Entity src = poly(v, closed);
        if (closed && std::abs(geom::area(as<PolylineData>(src))) < 10)
            continue;
        for (double sign : {1.0, -1.0}) {
            const double d = 0.3 + 2.5 * unit(rng);
            OffsetOptions opt;
            opt.roundCorners = iter % 3 != 0;
            auto r = offsetEntity(src, sign * d, opt);
            if (!r.ok())
                continue;
            ++checked;
            if (iter % 2 == 0)
                ++checkedStraight;
            bool ok = true;
            for (const auto& e : r.value()) {
                std::vector<geom::CurveRef> cs;
                geom::curvesOf(e, cs);
                for (const auto& c : cs)
                    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                        const Vec2 q = c.pointAt(c.isLine() ? t : c.t0() + t * c.sweep());
                        if (geom::distance(src, q) < d - 1e-4)
                            ok = false;
                    }
            }
            if (!ok) {
                ++bad;
                if (iter % 2 == 0)
                    ++badStraight;
                MESSAGE("iteration " << iter << " side " << sign << " distance " << d << " closed "
                                     << closed << " vertices " << v.size());
            }
        }
    }
    MESSAGE("checked " << checked << " bad " << bad << "; straight: checked " << checkedStraight
                       << " bad " << badStraight);
    CHECK(checked > 100);
    CHECK(bad == 0);
}

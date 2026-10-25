// Property sweep: the region a pick finds is never crossed by a frame chord or a circle.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>

#include "helpers.h"
#include "tbcad/geom/entity.h"
#include "tbcad/hatch/fill.h"
using namespace tbcad;
using namespace tbcad::hatch;
using namespace tbcad::hatch::testing;
namespace {
bool inRing(const std::vector<Vec2>& r, Vec2 p) {
    bool in = false;
    size_t n = r.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        if (((r[i].y > p.y) != (r[j].y > p.y)) &&
            (p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x))
            in = !in;
    }
    return in;
}
bool inRegion(const std::vector<std::vector<Vec2>>& rs, Vec2 p) {
    int c = 0;
    for (auto& r : rs)
        if (inRing(r, p))
            c++;
    return c % 2 == 1;
}
}  // namespace
TEST_CASE("property: hatch pick") {
    std::uint64_t s = 4242;
    auto nx = [&] {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (double)((s >> 33) & 0xFFFFFF) / 16777216.0;
    };
    int bad = 0, tot = 0, fail = 0;
    for (int it = 0; it < 3000; ++it) {
        Scene sc;
        sc.rectLines({0, 0}, {100, 100});
        std::vector<Entity> probes;
        int nl = (int)(nx() * 6), nc = (int)(nx() * 4), na = 0;
        for (int i = 0; i < nl; i++) {  // chord from one frame side to another
            auto side = [&](int k) {
                double t = nx() * 100;
                switch (k) {
                    case 0:
                        return Vec2{t, 0};
                    case 1:
                        return Vec2{100, t};
                    case 2:
                        return Vec2{t, 100};
                    default:
                        return Vec2{0, t};
                }
            };
            int k1 = (int)(nx() * 4), k2 = (k1 + 1 + (int)(nx() * 3)) % 4;
            Entity l = line(side(k1), side(k2));
            sc.add(l);
            probes.push_back(l);
        }
        for (int i = 0; i < nc; i++) {
            Entity c = circle({nx() * 100, nx() * 100}, 3 + nx() * 25);
            sc.add(c);
            probes.push_back(c);
        }
        for (int i = 0; i < na; i++) {
            double a0 = nx() * 6.28;
            Entity a = arc({nx() * 100, nx() * 100}, 5 + nx() * 25, a0, a0 + 1 + nx() * 4);
            sc.add(a);
            probes.push_back(a);
        }
        Vec2 pick{1 + nx() * 98, 1 + nx() * 98};
        auto r = findBoundary(sc.candidates(), pick);
        if (!r.ok()) {
            fail++;
            continue;
        }
        tot++;
        HatchData h;
        h.loops = r.value().loops;
        auto rings = loopRings(h, 0.001);
        bool ok = inRing(rings[0], pick);
        // pick must be filled unless it is in an island hole (ignore)
        for (auto& pe : probes) {
            std::vector<Vec2> o;
            geom::outlinePoints(pe, 0.0005, o);
            // sample along densified outline
            for (size_t i = 0; i + 1 <= o.size() && ok; i++) {
                Vec2 a = o[i], b = o[(i + 1) % o.size()];
                if (!std::holds_alternative<CircleData>(pe.data) && i + 1 == o.size())
                    break;
                for (int k = 1; k < 8; k++) {
                    double t = k / 8.0;
                    Vec2 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                    if (p.x < 0.05 || p.x > 99.95 || p.y < 0.05 || p.y > 99.95)
                        continue;
                    if (inRegion(rings, p) &&
                        !(std::holds_alternative<CircleData>(pe.data) && false)) {
                        // distance to boundary rings must be > 0.02 to count
                        double md = 1e9;
                        for (auto& rg : rings)
                            for (size_t q = 0; q < rg.size(); q++) {
                                Vec2 u = rg[q], v = rg[(q + 1) % rg.size()];
                                Vec2 uv{v.x - u.x, v.y - u.y};
                                double L = uv.x * uv.x + uv.y * uv.y;
                                double tt =
                                    L > 0
                                        ? std::clamp(((p.x - u.x) * uv.x + (p.y - u.y) * uv.y) / L,
                                                     0.0, 1.0)
                                        : 0;
                                md = std::min(md, std::hypot(p.x - (u.x + tt * uv.x),
                                                             p.y - (u.y + tt * uv.y)));
                            }
                        if (md > 0.02) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
        }
        if (!ok) {
            bad++;
            if (bad <= 5)
                std::printf("HATCH BAD it=%d pick=(%.3f,%.3f) nl=%d nc=%d na=%d loops=%zu\n", it,
                            pick.x, pick.y, nl, nc, na, r.value().loops.size());
        }
    }
    std::printf("hatch picks %d bad %d nofind %d\n", tot, bad, fail);
    CHECK(bad == 0);
}

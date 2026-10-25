// Property sweeps on random input: offsets stay at the offset distance from the source, fillets are
// tangent to both inputs, a trim keeps only parts of the target and removes the pick.
#include <cstdint>
#include <cstdio>

#include "test_support.h"
using namespace tbcad;
using namespace tbcad::modify;
using namespace tbcad::testing;
namespace {
double segDist(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab{b.x - a.x, b.y - a.y};
    double L = ab.x * ab.x + ab.y * ab.y;
    double t = L > 0 ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / L : 0;
    t = std::clamp(t, 0.0, 1.0);
    return distance(p, Vec2{a.x + t * ab.x, a.y + t * ab.y});
}
double distTo(const std::vector<Vec2>& o, bool closed, Vec2 p) {
    double m = 1e18;
    size_t n = o.size();
    for (size_t i = 0; i + 1 < n; i++)
        m = std::min(m, segDist(p, o[i], o[i + 1]));
    if (closed && n > 1)
        m = std::min(m, segDist(p, o[n - 1], o[0]));
    return m;
}
}  // namespace
TEST_CASE("property: offset polyline distance") {
    std::uint64_t s = 12345;
    auto nx = [&] {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (double)((s >> 33) & 0xFFFFFF) / 16777216.0;
    };
    int bad = 0, tot = 0;
    for (int it = 0; it < 1000; ++it) {
        int n = 3 + (int)(nx() * 5);
        std::vector<PolyVertex> v;
        for (int i = 0; i < n; i++) {
            double b = nx() < 0.4 ? (nx() - 0.5) * 1.6 : 0;
            v.push_back({{nx() * 100, nx() * 100}, 0, 0, b});
        }
        bool closed = nx() < 0.5;
        Entity e = poly(v, closed);
        double d = (nx() - 0.5) * 30;
        if (std::abs(d) < 0.5)
            continue;
        OffsetOptions o;
        o.roundCorners = true;
        auto r = offsetEntity(e, d, o);
        if (!r.ok())
            continue;
        std::vector<Vec2> src;
        geom::outlinePoints(e, 0.001, src);
        for (auto& oe : r.value()) {
            std::vector<Vec2> pts2;
            if (!geom::outlinePoints(oe, 0.01, pts2))
                continue;
            double mn = 1e18;
            for (Vec2 p : pts2)
                mn = std::min(mn, distTo(src, closed, p));
            tot++;
            if (mn < std::abs(d) - 0.05) {
                bad++;
                if (bad <= 8) {
                    std::printf("BAD it=%d d=%.3f min=%.3f closed=%d n=%d\n", it, d, mn,
                                (int)closed, n);
                    for (auto& pv : v)
                        std::printf("  (%.3f,%.3f b=%.3f)\n", pv.p.x, pv.p.y, pv.bulge);
                }
            }
        }
    }
    std::printf("offset results %d bad %d\n", tot, bad);
    CHECK(bad == 0);
}

namespace {
// distance from point to entity curve (full curve for circle/arc extended to full circle; line
// infinite)
double dFull(const Entity& e, Vec2 p) {
    if (auto* l = entityAs<LineData>(e)) {
        Vec2 d{l->b.x - l->a.x, l->b.y - l->a.y};
        double L = std::hypot(d.x, d.y);
        return std::abs((p.x - l->a.x) * d.y - (p.y - l->a.y) * d.x) / L;
    }
    if (auto* c = entityAs<CircleData>(e))
        return std::abs(distance(p, c->center) - c->radius);
    if (auto* a = entityAs<ArcData>(e))
        return std::abs(distance(p, a->center) - a->radius);
    return 0;
}
}  // namespace
TEST_CASE("property: fillet tangency") {
    std::uint64_t s = 777;
    auto nx = [&] {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (double)((s >> 33) & 0xFFFFFF) / 16777216.0;
    };
    int bad = 0, tot = 0;
    for (int it = 0; it < 4000; ++it) {
        auto mk = [&]() -> Entity {
            int k = (int)(nx() * 3);
            if (k == 0)
                return line({nx() * 100, nx() * 100}, {nx() * 100, nx() * 100});
            if (k == 1)
                return circle({nx() * 100, nx() * 100}, 5 + nx() * 30);
            double a0 = nx() * 6.28;
            return arc({nx() * 100, nx() * 100}, 5 + nx() * 30, a0, a0 + 0.5 + nx() * 5);
        };
        Entity a = mk(), b = mk();
        double r = 0.5 + nx() * 10;
        auto pk = [&](const Entity& e) {
            std::vector<Vec2> o;
            geom::outlinePoints(e, 0.1, o);
            return o[(size_t)(nx() * o.size()) % o.size()];
        };
        Vec2 pa = pk(a), pb = pk(b);
        FilletOptions fo;
        fo.radius = r;
        auto res = filletEntities(a, pa, b, pb, fo);
        if (!res.ok())
            continue;
        for (auto& ad : res.value().added) {
            auto* fa = entityAs<ArcData>(ad);
            if (!fa)
                continue;
            tot++;
            Vec2 p0{fa->center.x + fa->radius * std::cos(fa->startAngle),
                    fa->center.y + fa->radius * std::sin(fa->startAngle)};
            Vec2 p1{fa->center.x + fa->radius * std::cos(fa->endAngle),
                    fa->center.y + fa->radius * std::sin(fa->endAngle)};
            bool ok = std::abs(fa->radius - r) < 1e-6;
            // endpoints lie one on each curve
            double e0 = std::min(dFull(a, p0) + dFull(b, p1), dFull(a, p1) + dFull(b, p0));
            ok = ok && e0 < 1e-5;
            if (!ok) {
                bad++;
                if (bad <= 6)
                    std::printf("FILLET BAD it=%d r=%.3f arcR=%.3f e0=%g\n", it, r, fa->radius, e0);
            }
        }
    }
    std::printf("fillet %d bad %d\n", tot, bad);
    CHECK(bad == 0);
}

TEST_CASE("property: trim") {
    std::uint64_t s = 99;
    auto nx = [&] {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (double)((s >> 33) & 0xFFFFFF) / 16777216.0;
    };
    int bad = 0, tot = 0;
    for (int it = 0; it < 4000; ++it) {
        auto mk = [&]() -> Entity {
            int k = (int)(nx() * 5);
            if (k == 0 || k == 4)
                return line({nx() * 100, nx() * 100}, {nx() * 100, nx() * 100});
            if (k == 1)
                return circle({nx() * 100, nx() * 100}, 5 + nx() * 30);
            if (k == 2) {
                double a0 = nx() * 6.28;
                return arc({nx() * 100, nx() * 100}, 5 + nx() * 30, a0, a0 + 0.5 + nx() * 5);
            }
            std::vector<PolyVertex> v;
            int n = 3 + (int)(nx() * 4);
            for (int i = 0; i < n; i++)
                v.push_back({{nx() * 100, nx() * 100}, 0, 0, nx() < 0.3 ? (nx() - 0.5) * 1.5 : 0});
            return poly(v, nx() < 0.4);
        };
        Entity t = mk();
        std::vector<Entity> es;
        int ne = 1 + (int)(nx() * 3);
        for (int i = 0; i < ne; i++)
            es.push_back(mk());
        std::vector<const Entity*> ep;
        for (auto& e : es)
            ep.push_back(&e);
        std::vector<Vec2> o;
        geom::outlinePoints(t, 0.1, o);
        Vec2 pick = o[(size_t)(nx() * o.size()) % o.size()];
        auto res = trimEntity(t, pick, ep);
        if (!res.ok())
            continue;
        tot++;
        std::vector<Vec2> src;
        geom::outlinePoints(t, 0.0005, src);
        bool closed = false;
        if (auto* p = entityAs<PolylineData>(t))
            closed = p->closed;
        if (entityAs<CircleData>(t))
            closed = true;
        std::vector<Entity> pieces = res.value().added;
        if (res.value().modified)
            pieces.push_back(*res.value().modified);
        bool ok = true;
        double lenK = 0;
        for (auto& pc : pieces) {
            lenK += geom::length(pc);
            std::vector<Vec2> po;
            if (!geom::outlinePoints(pc, 0.01, po))
                continue;
            for (Vec2 p : po)
                if (distTo(src, closed, p) > 0.02)
                    ok = false;
            // pick must not be kept
            if (distTo(po, false, pick) < 1e-4)
                ok = false;
        }
        double L = geom::length(t);
        if (lenK >= L - 1e-6)
            ok = false;
        if (!ok) {
            bad++;
            if (bad <= 6)
                std::printf("TRIM BAD it=%d kind=%d pieces=%zu lenK=%g L=%g\n", it,
                            (int)t.data.index(), pieces.size(), lenK, L);
        }
    }
    std::printf("trim %d bad %d\n", tot, bad);
    CHECK(bad == 0);
}

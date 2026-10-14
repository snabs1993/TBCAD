// Robustness sweep: random (and deliberately nasty) inputs through every modify algorithm. The
// results do not matter here; they must not crash, hang or come back with NaN / infinite numbers.
#include <cstdint>

#include "tbcad/modify/extra.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::modify;
using namespace tbcad::testing;

namespace {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    std::uint32_t next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(s >> 33);
    }
    double uni(double lo, double hi) { return lo + (hi - lo) * (next() / 2147483648.0); }
    int pick(int n) { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
    // Mostly ordinary numbers; sometimes values that sit on grid lines or repeat exactly.
    double coord() {
        switch (pick(6)) {
            case 0:
                return 0;
            case 1:
                return static_cast<double>(pick(5)) * 10;
            default:
                return uni(-50, 50);
        }
    }
    Vec2 point() { return {coord(), coord()}; }
};

bool finite(Vec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) < 1e9 && std::abs(p.y) < 1e9;
}

bool finite(const Entity& e) {
    if (auto* l = entityAs<LineData>(e))
        return finite(l->a) && finite(l->b);
    if (auto* c = entityAs<CircleData>(e))
        return finite(c->center) && std::isfinite(c->radius);
    if (auto* a = entityAs<ArcData>(e))
        return finite(a->center) && std::isfinite(a->radius) && std::isfinite(a->startAngle) &&
               std::isfinite(a->endAngle);
    if (auto* p = entityAs<PolylineData>(e)) {
        for (const auto& v : p->v)
            if (!finite(v.p) || !std::isfinite(v.bulge))
                return false;
    }
    return true;
}

bool finite(const EntityEdit& ed) {
    if (ed.modified && !finite(*ed.modified))
        return false;
    for (const Entity& e : ed.added)
        if (!finite(e))
            return false;
    return true;
}

Entity randomEntity(Rng& r) {
    switch (r.pick(5)) {
        case 0:
            return line(r.point(), r.pick(8) == 0 ? Vec2{0, 0} : r.point());
        case 1:
            return circle(r.point(), r.pick(8) == 0 ? 0.0 : r.uni(0.5, 40));
        case 2: {
            double a0 = r.uni(-7, 7);
            return arc(r.point(), r.uni(0.5, 40), a0, a0 + r.uni(0.01, 6.2));
        }
        default: {
            std::vector<PolyVertex> v;
            int n = 2 + r.pick(5);
            for (int i = 0; i < n; ++i)
                v.push_back({r.pick(6) == 0 && i ? v.back().p : r.point(),
                             r.pick(3) == 0 ? r.uni(-2, 2) : 0.0, 0, 0});
            return poly(std::move(v), r.pick(3) == 0);
        }
    }
}

}  // namespace

TEST_CASE("modify algorithms survive random and degenerate input") {
    int produced = 0;
    for (std::uint64_t seed = 1; seed <= 1500; ++seed) {
        Rng r(seed);
        Entity a = randomEntity(r), b = randomEntity(r), c = randomEntity(r);
        std::vector<const Entity*> edges{&b, &c};
        Vec2 p = r.point(), q = r.point();

        for (double d : {-3.0, 0.0, 2.5, 1e6}) {
            auto o = offsetEntity(a, d);
            if (o.ok())
                for (const Entity& e : o.value()) {
                    ++produced;
                    CHECK(finite(e));
                }
        }
        if (auto o = offsetToward(a, p, 4.0); o.ok())
            for (const Entity& e : o.value())
                CHECK(finite(e));
        if (auto o = offsetThrough(a, p); o.ok())
            for (const Entity& e : o.value())
                CHECK(finite(e));

        if (auto t = trimEntity(a, p, edges); t.ok()) {
            ++produced;
            CHECK(finite(t.value()));
        }
        if (auto t = extendEntity(a, p, edges); t.ok()) {
            ++produced;
            CHECK(finite(t.value()));
        }
        if (auto br = breakEntity(a, p, q); br.ok()) {
            ++produced;
            CHECK(finite(br.value()));
        }
        if (auto br = breakAtPoint(a, p); br.ok()) {
            ++produced;
            CHECK(finite(br.value()));
        }

        for (double rad : {0.0, 1.0, 7.5}) {
            FilletOptions fo;
            fo.radius = rad;
            if (auto f = filletEntities(a, p, b, q, fo); f.ok()) {
                ++produced;
                if (f.value().modifiedA)
                    CHECK(finite(*f.value().modifiedA));
                if (f.value().modifiedB)
                    CHECK(finite(*f.value().modifiedB));
                for (const Entity& e : f.value().added)
                    CHECK(finite(e));
            }
            if (auto f = filletPolyline(a, rad); f.ok())
                CHECK(finite(f.value()));
            if (auto f = filletPolylineVertex(a, 1, rad); f.ok())
                CHECK(finite(f.value()));
        }
        ChamferOptions co;
        co.distance1 = 2;
        co.distance2 = 3;
        if (auto f = chamferEntities(a, p, b, q, co); f.ok())
            ++produced;
        if (auto f = chamferPolyline(a, co); f.ok())
            CHECK(finite(f.value()));

        for (LengthenMode m : {LengthenMode::Delta, LengthenMode::Percent, LengthenMode::Total})
            for (double v : {-5.0, 0.0, 30.0})
                if (auto l = lengthenEntity(a, p, m, v); l.ok())
                    CHECK(finite(l.value()));
        if (auto l = lengthenToPoint(a, p, q); l.ok())
            CHECK(finite(l.value()));
        if (auto v = reverseEntity(a); v.ok())
            CHECK(finite(v.value()));
        for (int n : {2, 5})
            if (auto d = dividePoints(a, n); d.ok())
                for (Vec2 pt : d.value())
                    CHECK(finite(pt));
        for (double seg : {0.0, 1.0, 13.0})
            if (auto m = measurePoints(a, seg); m.ok())
                for (Vec2 pt : m.value())
                    CHECK(finite(pt));
        if (auto t = toPolylineEntity(a); t.ok())
            CHECK(finite(t.value()));
        if (auto t = extrimEntity(a, b, p); t.ok())
            CHECK(finite(t.value()));
        if (auto t = blendCurves(a, p, b, q); t.ok())
            CHECK(finite(t.value()));
        if (auto t = insertPolylineVertex(a, p); t.ok())
            CHECK(finite(t.value()));
        if (auto t = removePolylineVertex(a, p); t.ok())
            CHECK(finite(t.value()));
        if (auto t = movePolylineVertex(a, p, q); t.ok())
            CHECK(finite(t.value()));
        if (auto t = setPolylineWidth(a, 2); t.ok())
            CHECK(finite(t.value()));
        if (auto sp = polylineToSpline(a); sp.ok()) {
            if (auto t = editSplineFit(sp.value(), SplineFitOp::Add, p); t.ok())
                CHECK(finite(t.value()));
            if (auto t = editSplineFit(sp.value(), SplineFitOp::Delete, p); t.ok())
                CHECK(finite(t.value()));
            if (auto t = splineToPolyline(sp.value(), 0.01); t.ok())
                CHECK(finite(t.value()));
        }

        std::vector<const Entity*> all{&a, &b, &c};
        for (const Entity& e : overkill(all).created)
            CHECK(finite(e));
        JoinResult j = joinEntities(all, 1e-3);
        for (const Entity& e : j.created)
            CHECK(finite(e));
    }
    CHECK(produced > 500);  // the sweep must actually exercise the algorithms
}

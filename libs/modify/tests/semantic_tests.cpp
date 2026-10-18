// Invariants for BREAK, BREAK AT POINT, JOIN and LENGTHEN on every curve type: lengths are
// conserved, split pieces rejoin into the original, lengthen changes the length by the amount
// asked.
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
    Vec2 point() { return {uni(-50, 50), uni(-50, 50)}; }
};

// An open, non-degenerate curve of a random kind.
Entity openCurve(Rng& r) {
    switch (r.pick(6)) {
        case 0:
            return line(r.point(), r.point() + Vec2{r.uni(5, 40), r.uni(-20, 20)});
        case 1: {
            const double a0 = r.uni(-3, 3);
            return arc(r.point(), r.uni(3, 40), a0, a0 + r.uni(0.5, 5.0));
        }
        case 2: {
            EllipseData d;
            d.center = r.point();
            const double a = r.uni(5, 30), t = r.uni(0, 6.28);
            d.majorAxis = {a * std::cos(t), a * std::sin(t)};
            d.ratio = r.uni(0.2, 1);
            d.startParam = r.uni(0, 2);
            d.endParam = d.startParam + r.uni(0.8, 4.5);
            return make(std::move(d));
        }
        case 3: {
            SplineData d;
            d.degree = 3;
            d.knots = {0, 0, 0, 0, 1, 2, 3, 3, 3, 3};
            Vec2 p = r.point();
            for (int i = 0; i < 6; ++i) {
                d.control.push_back(p);
                p = p + Vec2{r.uni(2, 15), r.uni(-12, 12)};
            }
            return make(std::move(d));
        }
        default: {
            std::vector<PolyVertex> v;
            Vec2 p = r.point();
            const int n = 3 + r.pick(5);
            for (int i = 0; i < n; ++i) {
                v.push_back({p, r.pick(3) == 0 ? r.uni(-0.35, 0.35) : 0.0, 0, 0});
                p = p + Vec2{r.uni(8, 20), r.uni(-6, 6)};
            }
            return poly(std::move(v), false);
        }
    }
}

double sumLength(const EntityEdit& ed) {
    double t = ed.modified ? geom::length(*ed.modified) : 0.0;
    for (const Entity& e : ed.added)
        t += geom::length(e);
    return t;
}

// A point on the curve at fraction f of its length, in the direction of travel.
Vec2 pointAtFraction(const Entity& e, double f) {
    if (const auto* pl = entityAs<PolylineData>(e)) {
        const auto segs = polylineSegs(*pl);
        double total = 0;
        for (const Seg& sg : segs)
            total += segLength(sg);
        double s = f * total;
        for (const Seg& sg : segs) {
            const double L = segLength(sg);
            if (s <= L)
                return segPoint(sg, L > 0 ? s / L : 0);
            s -= L;
        }
        return segs.back().p1;
    }
    std::vector<geom::CurveRef> cs;
    geom::curvesOf(e, cs);
    const auto& c = cs.at(0);
    return c.pointAt(c.paramAtLength(f * c.length()));
}

}  // namespace

TEST_CASE("break at point keeps the whole length in two pieces, on every open curve type") {
    int checked = 0;
    for (std::uint64_t seed = 1; seed <= 400; ++seed) {
        Rng r(seed);
        Entity e = openCurve(r);
        const double L = geom::length(e);
        const double f = r.uni(0.1, 0.9);
        auto br = breakAtPoint(e, pointAtFraction(e, f));
        REQUIRE_MESSAGE(br.ok(), "seed " << seed << " kind " << static_cast<int>(kind(e)));
        const EntityEdit& ed = br.value();
        REQUIRE(ed.modified.has_value());
        CHECK(ed.added.size() == 1);
        CHECK_MESSAGE(near(sumLength(ed), L, 1e-3 * (1 + L)),
                      "seed " << seed << " kind " << static_cast<int>(kind(e)) << " len " << L
                              << " got " << sumLength(ed));
        ++checked;
    }
    CHECK(checked == 400);
}

TEST_CASE("break removes exactly the stretch between the two points") {
    for (std::uint64_t seed = 1; seed <= 400; ++seed) {
        Rng r(seed);
        Entity e = openCurve(r);
        const double L = geom::length(e);
        double f1 = r.uni(0.05, 0.45), f2 = r.uni(0.55, 0.95);
        auto br = breakEntity(e, pointAtFraction(e, f1), pointAtFraction(e, f2));
        REQUIRE_MESSAGE(br.ok(), "seed " << seed << " kind " << static_cast<int>(kind(e)));
        CHECK_MESSAGE(near(sumLength(br.value()), L * (1 - (f2 - f1)), 2e-3 * (1 + L)),
                      "seed " << seed << " kind " << static_cast<int>(kind(e)) << " L " << L
                              << " f " << f1 << " " << f2 << " got " << sumLength(br.value())
                              << " want " << L * (1 - (f2 - f1)) << " pieces "
                              << br.value().added.size());
        // The two points given in the other order cut the same stretch.
        auto rev = breakEntity(e, pointAtFraction(e, f2), pointAtFraction(e, f1));
        REQUIRE(rev.ok());
        CHECK(near(sumLength(rev.value()), sumLength(br.value()), 2e-3 * (1 + L)));
    }
}

TEST_CASE("pieces made by break at point join back into the original length") {
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        Rng r(seed);
        Entity e = openCurve(r);
        const double L = geom::length(e);
        auto br = breakAtPoint(e, pointAtFraction(e, r.uni(0.2, 0.8)));
        REQUIRE(br.ok());
        std::vector<Entity> parts;
        parts.push_back(*br.value().modified);
        parts.push_back(br.value().added.at(0));
        std::vector<const Entity*> in{&parts[r.pick(2)], &parts[0]};
        if (in[0] == in[1])
            in[1] = &parts[1];
        JoinResult j = joinEntities(in, 1e-3);
        REQUIRE_MESSAGE(j.created.size() == 1,
                        "seed " << seed << " kind " << static_cast<int>(kind(e)));
        CHECK(j.consumed.size() == 2);
        CHECK_MESSAGE(near(geom::length(j.created[0]), L, 2e-3 * (1 + L)),
                      "seed " << seed << " kind " << static_cast<int>(kind(e)));
    }
}

TEST_CASE("a polyline cut into many pieces joins back whatever the order") {
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        Rng r(seed);
        std::vector<PolyVertex> v;
        Vec2 p = r.point();
        for (int i = 0; i < 12; ++i) {
            v.push_back({p, 0, 0, 0});
            p = p + Vec2{r.uni(2, 12), r.uni(-10, 10)};
        }
        Entity whole = poly(v, false);
        const double L = geom::length(whole);
        std::vector<Entity> pieces;
        Entity rest = whole;
        for (int cut = 0; cut < 4; ++cut) {
            const double f = 0.2 + 0.2 * cut;
            // Cut the remaining part at fraction f of the original length.
            const double remLen = geom::length(rest);
            const double want = std::min(0.9, (f * L) / std::max(remLen, 1e-9));
            auto br = breakAtPoint(rest, pointAtFraction(rest, std::max(0.05, want)));
            if (!br.ok())
                break;
            pieces.push_back(*br.value().modified);
            rest = br.value().added.at(0);
        }
        pieces.push_back(rest);
        // Shuffle.
        for (std::size_t i = pieces.size(); i > 1; --i)
            std::swap(pieces[i - 1], pieces[static_cast<std::size_t>(r.pick(static_cast<int>(i)))]);
        std::vector<const Entity*> in;
        for (const Entity& e : pieces)
            in.push_back(&e);
        JoinResult j = joinEntities(in, 1e-3);
        REQUIRE_MESSAGE(j.created.size() == 1, "seed " << seed << " made " << j.created.size());
        CHECK(j.consumed.size() == pieces.size());
        CHECK(near(geom::length(j.created[0]), L, 2e-3 * (1 + L)));
    }
}

TEST_CASE("join with a fuzz distance bridges small gaps and refuses large ones") {
    Entity a = line({0, 0}, {10, 0}), b = line({10.4, 0.2}, {20, 5});
    std::vector<const Entity*> in{&a, &b};
    CHECK(joinEntities(in, 0.1).created.empty());
    JoinResult j = joinEntities(in, 0.6);
    REQUIRE(j.created.size() == 1);
    CHECK(kind(j.created[0]) == EntityKind::Polyline);
    CHECK(j.consumed.size() == 2);
}

TEST_CASE("lengthen by delta, percent and total changes the length as asked") {
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        Rng r(seed);
        Entity e = openCurve(r);
        const double L = geom::length(e);
        const Vec2 pick = pointAtFraction(e, r.pick(2) ? 0.95 : 0.05);
        const double d = r.uni(-0.3, 0.5) * L;
        auto ld = lengthenEntity(e, pick, LengthenMode::Delta, d);
        if (ld.ok() && ld.value().modified)
            CHECK_MESSAGE(near(geom::length(*ld.value().modified), L + d, 5e-3 * (1 + L)),
                          "delta seed " << seed << " kind " << static_cast<int>(kind(e)) << " L "
                                        << L << " d " << d << " got "
                                        << geom::length(*ld.value().modified));
        else if (!ld.ok())
            MESSAGE("delta refused seed " << seed << " kind " << static_cast<int>(kind(e)));
        auto lt = lengthenEntity(e, pick, LengthenMode::Total, 0.7 * L);
        if (lt.ok() && lt.value().modified)
            CHECK_MESSAGE(near(geom::length(*lt.value().modified), 0.7 * L, 5e-3 * (1 + L)),
                          "total seed " << seed << " kind " << static_cast<int>(kind(e)));
        auto lp = lengthenEntity(e, pick, LengthenMode::Percent, 150);
        if (lp.ok() && lp.value().modified)
            CHECK_MESSAGE(near(geom::length(*lp.value().modified), 1.5 * L, 5e-3 * (1 + L)),
                          "percent seed " << seed << " kind " << static_cast<int>(kind(e)) << " L "
                                          << L << " got " << geom::length(*lp.value().modified));
    }
}

TEST_CASE("two elliptical arcs that close up join into a full ellipse") {
    auto part = [](double s, double e) {
        EllipseData d;
        d.center = {5, 5};
        d.majorAxis = {20, 5};
        d.ratio = 0.5;
        d.startParam = s;
        d.endParam = e;
        return make(std::move(d));
    };
    Entity a = part(0.5, 3.0), b = part(3.0, 0.5 + kTwoPi);
    std::vector<const Entity*> in{&a, &b};
    JoinResult j = joinEntities(in, 1e-3);
    REQUIRE(j.created.size() == 1);
    const auto& d = as<EllipseData>(j.created[0]);
    CHECK(near(d.endParam - d.startParam, kTwoPi, 1e-9));
    CHECK(j.consumed.size() == 2);
    // A different ellipse is left alone.
    Entity c = part(3.0, 4.0);
    EllipseData other = as<EllipseData>(c);
    other.ratio = 0.4;
    Entity c2 = make(other);
    std::vector<const Entity*> in2{&a, &c2};
    CHECK(joinEntities(in2, 1e-3).created.empty());
}

TEST_CASE("splines joined end to end keep their shape; either direction, any order") {
    Rng r(7);
    for (int rep = 0; rep < 50; ++rep) {
        Entity whole = openCurve(r);
        while (kind(whole) != EntityKind::Spline)
            whole = openCurve(r);
        const auto& sd = as<SplineData>(whole);
        const double mid = 0.5 * (sd.knots.front() + sd.knots.back());
        auto s1 = geom::splineSubcurve(sd, sd.knots.front(), mid);
        auto s2 = geom::splineSubcurve(sd, mid, sd.knots.back());
        REQUIRE(s1.ok());
        REQUIRE(s2.ok());
        Entity a = make(Indirect<SplineData>(s1.value())),
               b = make(Indirect<SplineData>(s2.value()));
        if (rep % 2)
            b = reverseEntity(b).value().modified.value();
        std::vector<const Entity*> in{rep % 3 ? &a : &b, rep % 3 ? &b : &a};
        JoinResult j = joinEntities(in, 1e-3);
        REQUIRE(j.created.size() == 1);
        REQUIRE(kind(j.created[0]) == EntityKind::Spline);
        const double L = geom::length(whole);
        CHECK(near(geom::length(j.created[0]), L, 2e-3 * (1 + L)));
    }
}

TEST_CASE("fit curve passes through every vertex, with a continuous tangent, as arcs only") {
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        Rng r(seed);
        std::vector<PolyVertex> v;
        Vec2 p = r.point();
        const int n = 3 + r.pick(6);
        for (int i = 0; i < n; ++i) {
            v.push_back({p, 0, 0, 0});
            p = p + Vec2{r.uni(5, 20), r.uni(-12, 12)};
        }
        const bool closed = seed % 3 == 0;
        Entity src = poly(v, closed);
        auto fit = fitPolylineCurve(src);
        REQUIRE_MESSAGE(fit.ok(), "seed " << seed);
        const auto& pl = as<PolylineData>(fit.value());
        // Every original vertex is still a vertex of the result, in order.
        std::size_t at = 0;
        for (const PolyVertex& ov : v) {
            while (at < pl.v.size() && !near(pl.v[at].p, ov.p, 1e-9))
                ++at;
            CHECK_MESSAGE(at < pl.v.size(), "seed " << seed << " lost a vertex");
            ++at;
        }
        CHECK(pl.closed == closed);
        // The curve is smooth: at each joint the two arcs share a tangent direction.
        const auto segs = polylineSegs(pl);
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (!closed && i + 1 == segs.size())
                break;
            const Seg &a = segs[i], &b = segs[(i + 1) % segs.size()];
            const Vec2 ta = segTangent(a, 1.0), tb = segTangent(b, 0.0);
            CHECK_MESSAGE(dot(ta, tb) > 1 - 1e-6, "seed " << seed << " kink at seg " << i);
        }
    }
}

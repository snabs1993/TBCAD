// Pick-point boundaries on messy drawings: overlapping and duplicated lines, split arcs, splines,
// gaps, nested islands and survey-sized coordinates.
#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "helpers.h"
#include "tbcad/geom/curve.h"
#include "tbcad/hatch/boundary.h"

using namespace tbcad;
using namespace tbcad::hatch;
using namespace tbcad::hatch::testing;

namespace {

const Vec2 kOrigins[] = {{0, 0}, {500123.456, 4500987.654}};

// Area enclosed by a loop (edges in order), arcs and splines sampled finely.
double loopArea(const HatchLoop& loop) {
    std::vector<Vec2> pts;
    for (const HatchEdge& e : loop.edges) {
        std::vector<Vec2> seg;
        if (const auto* l = std::get_if<HatchLineEdge>(&e)) {
            seg = {l->a, l->b};
        } else if (const auto* a = std::get_if<HatchArcEdge>(&e)) {
            double sweep = a->ccw ? normalizeAngle(a->endAngle - a->startAngle)
                                  : -normalizeAngle(a->startAngle - a->endAngle);
            if (std::abs(sweep) < 1e-12)
                sweep = a->ccw ? kTwoPi : -kTwoPi;
            for (int i = 0; i <= 4000; ++i) {
                double t = a->startAngle + sweep * i / 4000.0;
                seg.push_back(a->center + Vec2{std::cos(t), std::sin(t)} * a->radius);
            }
        } else if (const auto* s = std::get_if<HatchSplineEdge>(&e)) {
            auto c = geom::CurveRef::spline(s->spline);
            if (c)
                geom::flatten(c.value(), 1e-7, seg);
        } else if (const auto* p = std::get_if<HatchPolylineEdge>(&e)) {
            for (const auto& v : p->v)
                seg.push_back(v.p);
        }
        if (!pts.empty() && !seg.empty() && distance(pts.back(), seg.front()) > 1e-6)
            std::reverse(seg.begin(), seg.end());
        pts.insert(pts.end(), seg.begin(), seg.end());
    }
    if (pts.size() < 3)
        return 0;
    double a = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        Vec2 p = pts[i] - pts[0], q = pts[(i + 1) % pts.size()] - pts[0];
        a += cross(p, q);
    }
    return 0.5 * std::abs(a);
}

Vec2 at(Vec2 o, double x, double y) {
    return {o.x + x, o.y + y};
}

}  // namespace

TEST_CASE("duplicated and overlapping collinear lines make the same region as clean ones") {
    for (Vec2 o : kOrigins) {
        Scene s;
        // Each side drawn as two overlapping pieces plus a full duplicate.
        const Vec2 c[] = {at(o, 0, 0), at(o, 100, 0), at(o, 100, 60), at(o, 0, 60)};
        for (int i = 0; i < 4; ++i) {
            Vec2 a = c[i], b = c[(i + 1) % 4];
            Vec2 m1 = a + (b - a) * 0.6, m2 = a + (b - a) * 0.4;
            s.add(line(a, m1));
            s.add(line(m2, b));
            s.add(line(a, b));
        }
        auto r = findBoundary(s.candidates(), at(o, 50, 30));
        REQUIRE(r.ok());
        REQUIRE(r.value().loops.size() == 1);
        CHECK(std::abs(loopArea(r.value().loops[0]) - 6000) < 1e-5);
    }
}

TEST_CASE("a circle drawn as two arcs and a rectangle with arcs for corners") {
    for (Vec2 o : kOrigins) {
        Scene s;
        s.add(arc(at(o, 50, 50), 30, 0, kPi));
        s.add(arc(at(o, 50, 50), 30, kPi, kTwoPi));
        auto r = findBoundary(s.candidates(), at(o, 55, 45));
        REQUIRE(r.ok());
        REQUIRE(r.value().loops.size() == 1);
        CHECK(std::abs(loopArea(r.value().loops[0]) - kPi * 900) < 1e-3);

        // Quarter arcs make a rounded square.
        Scene q;
        const double R = 10;
        q.add(line(at(o, R, 0), at(o, 100 - R, 0)));
        q.add(arc(at(o, 100 - R, R), R, 3 * kPi / 2, kTwoPi));
        q.add(line(at(o, 100, R), at(o, 100, 100 - R)));
        q.add(arc(at(o, 100 - R, 100 - R), R, 0, kPi / 2));
        q.add(line(at(o, 100 - R, 100), at(o, R, 100)));
        q.add(arc(at(o, R, 100 - R), R, kPi / 2, kPi));
        q.add(line(at(o, 0, 100 - R), at(o, 0, R)));
        q.add(arc(at(o, R, R), R, kPi, 3 * kPi / 2));
        auto rr = findBoundary(q.candidates(), at(o, 50, 50));
        REQUIRE(rr.ok());
        CHECK(std::abs(loopArea(rr.value().loops[0]) - (10000 - (4 - kPi) * R * R)) < 1e-3);
    }
}

TEST_CASE("a closed spline inside a frame is an island; picking inside it fills it") {
    for (Vec2 o : kOrigins) {
        Scene s;
        s.rectLines(at(o, 0, 0), at(o, 200, 200));
        SplineData sp;
        sp.degree = 3;
        sp.closed = true;
        sp.periodic = true;
        const std::vector<Vec2> pts{at(o, 80, 80), at(o, 120, 80), at(o, 120, 120), at(o, 80, 120)};
        sp.control = pts;
        for (int i = 0; i < 3; ++i)
            sp.control.push_back(pts[static_cast<std::size_t>(i)]);
        for (int i = 0; i < 11; ++i)
            sp.knots.push_back(i);
        const EntityId id = s.add(ent(Indirect<SplineData>(sp)));
        (void)id;
        auto outer = findBoundary(s.candidates(), at(o, 20, 20));
        REQUIRE(outer.ok());
        CHECK(outer.value().loops.size() == 2);
        auto inner = findBoundary(s.candidates(), at(o, 100, 100));
        REQUIRE(inner.ok());
        REQUIRE(inner.value().loops.size() == 1);
        const double a = loopArea(inner.value().loops[0]);
        CHECK(a > 900);
        CHECK(a < 1700);
        // The frame minus the spline is what the outer pick encloses.
        CHECK(std::abs(loopArea(outer.value().loops[0]) - 40000) < 1e-4);
    }
}

TEST_CASE("three levels of nesting alternate filled and empty") {
    for (Vec2 o : kOrigins) {
        Scene s;
        s.rectLines(at(o, 0, 0), at(o, 300, 300));
        s.add(ent([&] {
            PolylineData p;
            p.closed = true;
            for (Vec2 v : {at(o, 50, 50), at(o, 250, 50), at(o, 250, 250), at(o, 50, 250)}) {
                PolyVertex pv;
                pv.p = v;
                p.v.push_back(pv);
            }
            return p;
        }()));
        s.add(circle(at(o, 150, 150), 60));
        s.add(circle(at(o, 150, 150), 20));
        auto ring = findBoundary(s.candidates(), at(o, 20, 20));
        REQUIRE(ring.ok());
        CHECK(ring.value().loops.size() == 4);  // frame, hole, filled again, hole
        auto mid = findBoundary(s.candidates(), at(o, 80, 80));
        REQUIRE(mid.ok());
        CHECK(mid.value().loops.size() == 3);  // square, hole, filled again
        auto disc = findBoundary(s.candidates(), at(o, 150, 190));
        REQUIRE(disc.ok());
        CHECK(disc.value().loops.size() == 2);  // big circle with the small one as a hole
        auto core = findBoundary(s.candidates(), at(o, 150, 150));
        REQUIRE(core.ok());
        CHECK(core.value().loops.size() == 1);
        CHECK(std::abs(loopArea(core.value().loops[0]) - kPi * 400) < 1e-3);
    }
}

TEST_CASE("HPGAPTOL: gaps at every corner close within the tolerance and not beyond it") {
    for (Vec2 o : kOrigins) {
        for (double gap : {0.05, 0.5, 2.0}) {
            Scene s;
            const double g = gap / 2;
            // Each side stops g short of both corners.
            s.add(line(at(o, g, 0), at(o, 100 - g, 0)));
            s.add(line(at(o, 100, g), at(o, 100, 60 - g)));
            s.add(line(at(o, 100 - g, 60), at(o, g, 60)));
            s.add(line(at(o, 0, 60 - g), at(o, 0, g)));
            BoundaryOptions strict;  // no tolerance
            CHECK_FALSE(findBoundary(s.candidates(), at(o, 50, 30), strict).ok());
            BoundaryOptions loose;
            loose.gapTolerance = gap * 1.6;  // the corner gap is the diagonal g*sqrt(2) * 2
            auto r = findBoundary(s.candidates(), at(o, 50, 30), loose);
            CHECK_MESSAGE(r.ok(), "gap " << gap);
            if (r.ok())
                CHECK(std::abs(loopArea(r.value().loops[0]) - 6000) < 6000 * 0.01);
            BoundaryOptions tight;
            tight.gapTolerance = gap * 0.4;
            CHECK_FALSE(findBoundary(s.candidates(), at(o, 50, 30), tight).ok());
        }
    }
}

TEST_CASE("a gap closes against the side of a line, a spline end and an arc") {
    for (Vec2 o : kOrigins) {
        Scene s;
        s.add(line(at(o, 0, 0), at(o, 100, 0)));
        s.add(line(at(o, 100, 0), at(o, 100, 50)));
        s.add(line(at(o, 0, 50), at(o, 0, 0)));
        // The top is an arc that stops 0.3 short of the right wall.
        s.add(arc(at(o, 50, 50), 50, 0.006 * kPi, kPi));
        BoundaryOptions opt;
        opt.gapTolerance = 1.0;
        auto r = findBoundary(s.candidates(), at(o, 50, 20), opt);
        CHECK(r.ok());
        BoundaryOptions none;
        CHECK_FALSE(findBoundary(s.candidates(), at(o, 50, 20), none).ok());
    }
}

TEST_CASE("a grid of duplicated cells gives the right cell for every pick, at survey coordinates") {
    std::mt19937 rng(7);
    for (Vec2 o : kOrigins) {
        Scene s;
        const int n = 12;
        const double w = 25.5;
        for (int i = 0; i <= n; ++i) {
            s.add(line(at(o, i * w, 0), at(o, i * w, n * w)));
            s.add(line(at(o, 0, i * w), at(o, n * w, i * w)));
            if (rng() % 2)  // duplicate with reversed direction
                s.add(line(at(o, i * w, n * w), at(o, i * w, 0)));
        }
        BoundaryFinder f(s.candidates());
        REQUIRE(f.ok());
        for (int k = 0; k < 40; ++k) {
            int ix = static_cast<int>(rng() % n), iy = static_cast<int>(rng() % n);
            auto r = f.find(at(o, (ix + 0.31) * w, (iy + 0.68) * w));
            REQUIRE(r.ok());
            REQUIRE(r.value().loops.size() == 1);
            CHECK(std::abs(loopArea(r.value().loops[0]) - w * w) < 1e-6);
        }
    }
}

TEST_CASE("a pick on a crossing of tilted lines finds a face and a pick outside everything fails") {
    for (Vec2 o : kOrigins) {
        Scene s;
        s.add(line(at(o, 0, 0), at(o, 100, 100)));
        s.add(line(at(o, 0, 100), at(o, 100, 0)));
        s.add(line(at(o, 0, 0), at(o, 100, 0)));
        s.add(line(at(o, 100, 0), at(o, 100, 100)));
        s.add(line(at(o, 100, 100), at(o, 0, 100)));
        s.add(line(at(o, 0, 100), at(o, 0, 0)));
        auto r = findBoundary(s.candidates(), at(o, 50, 10));  // the bottom triangle
        REQUIRE(r.ok());
        CHECK(std::abs(loopArea(r.value().loops[0]) - 2500) < 1e-6);
        CHECK_FALSE(findBoundary(s.candidates(), at(o, 500, 500)).ok());
    }
}

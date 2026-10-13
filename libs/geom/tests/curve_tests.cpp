#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::geom;

TEST_CASE("line curve basics") {
    auto c = CurveRef::line({0, 0}, {3, 4});
    CHECK(c.length() == doctest::Approx(5));
    CHECK(distance(c.pointAt(0.5), Vec2{1.5, 2}) < 1e-12);
    CHECK(distance(c.tangentAt(0.3), Vec2{0.6, 0.8}) < 1e-12);
    CHECK(c.paramAt({3, 0}) == doctest::Approx(0.36));
    CHECK(c.paramAt({-5, -5}) == 0);
    CHECK(c.paramAt({50, 50}) == 1);
    auto s = c.split(0.25);
    REQUIRE(s.ok());
    CHECK(s.value().first.length() == doctest::Approx(1.25));
    CHECK(s.value().second.length() == doctest::Approx(3.75));
    CHECK_FALSE(c.split(0.0).ok());
    CHECK_FALSE(c.split(1.0).ok());
}

TEST_CASE("arc curve basics and parameter wrap") {
    auto c = CurveRef::arc({1, 1}, 2, kPi / 2, kPi);  // 90 deg to 270 deg
    CHECK(c.length() == doctest::Approx(2 * kPi));
    CHECK(distance(c.startPoint(), Vec2{1, 3}) < 1e-12);
    CHECK(distance(c.endPoint(), Vec2{1, -1}) < 1e-12);
    CHECK(distance(c.tangentAt(kPi), Vec2{0, -1}) < 1e-12);  // moving down at the leftmost point
    CHECK(c.containsParam(kPi));
    CHECK_FALSE(c.containsParam(0));
    CHECK(c.containsParam(kPi / 2 - 1e-9));  // within slack
    // paramAt a point outside the angular range picks the nearer end.
    CHECK(c.paramAt({10, 1 + 0.1}) == doctest::Approx(c.t0()));
    CHECK(c.paramAt({10, 1 - 0.1}) == doctest::Approx(c.t1()));
    BBox2 b = c.bounds();
    CHECK(b.min.x == doctest::Approx(-1));
    CHECK(b.max.x == doctest::Approx(1).epsilon(1e-9));
    CHECK(b.min.y == doctest::Approx(-1));
    CHECK(b.max.y == doctest::Approx(3));
    auto s = c.split(kPi);
    REQUIRE(s.ok());
    CHECK(s.value().first.sweep() == doctest::Approx(kPi / 2));
}

TEST_CASE("circle is a closed arc") {
    auto c = CurveRef::circle({0, 0}, 5);
    CHECK(c.isClosed());
    CHECK(c.length() == doctest::Approx(10 * kPi));
    auto piece = c.subcurve(1, 2);
    REQUIRE(piece.ok());
    CHECK(piece.value().length() == doctest::Approx(5));
    CHECK(c.wrapParam(-0.5) == doctest::Approx(kTwoPi - 0.5));
}

TEST_CASE("ellipse curve: points, bounds, length, closest point") {
    auto e = CurveRef::ellipse({1, 2}, {4, 0}, 0.5, 0, kTwoPi);
    CHECK(distance(e.pointAt(0), Vec2{5, 2}) < 1e-12);
    CHECK(distance(e.pointAt(kPi / 2), Vec2{1, 4}) < 1e-12);
    BBox2 b = e.bounds();
    CHECK(b.min.x == doctest::Approx(-3));
    CHECK(b.max.x == doctest::Approx(5));
    CHECK(b.min.y == doctest::Approx(0));
    CHECK(b.max.y == doctest::Approx(4));
    // Perimeter of a=4,b=2 (Ramanujan II is within 1e-5; the integrator should be far closer).
    double a = 4, bb = 2, h = std::pow(a - bb, 2) / std::pow(a + bb, 2);
    double ram = kPi * (a + bb) * (1 + 3 * h / (10 + std::sqrt(4 - 3 * h)));
    CHECK(e.length() == doctest::Approx(ram).epsilon(1e-5));
    // Closest point from the far right is the vertex.
    Vec2 cp = e.closestPoint({100, 2});
    CHECK(distance(cp, Vec2{5, 2}) < 1e-6);
    // Rotated major axis.
    auto r = CurveRef::ellipse({0, 0}, {0, 3}, 0.5, 0, kTwoPi);
    CHECK(distance(r.pointAt(0), Vec2{0, 3}) < 1e-12);
    CHECK(distance(r.pointAt(kPi / 2), Vec2{-1.5, 0}) < 1e-12);
}

TEST_CASE("elliptical arc range") {
    auto e = CurveRef::ellipse({0, 0}, {4, 0}, 0.5, kPi / 2, kPi);
    CHECK(e.containsParam(kPi));
    CHECK_FALSE(e.containsParam(0));
    BBox2 b = e.bounds();
    CHECK(b.min.x == doctest::Approx(-4));
    CHECK(b.max.x == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("bulge arcs: both signs, endpoints and orientation") {
    for (double bulge : {1.0, -1.0, 0.5, -0.5, 2.0, -3.0, 0.1}) {
        Vec2 p0{0, 0}, p1{2, 0};
        auto arc = bulgeArc(p0, p1, bulge);
        REQUIRE(arc.has_value());
        CHECK(arc->sweep == doctest::Approx(4 * std::atan(std::abs(bulge))));
        Segment s;
        s.curve = CurveRef::arc(arc->center, arc->radius, arc->startAngle, arc->sweep);
        s.reversed = arc->reversed;
        CHECK(distance(s.pointAtLocal(0), p0) < 1e-9);
        CHECK(distance(s.pointAtLocal(1), p1) < 1e-9);
        // Positive bulge: the arc bulges to the right of the direction of travel.
        Vec2 mid = s.pointAtLocal(0.5);
        if (bulge > 0)
            CHECK(mid.y < 0);
        else
            CHECK(mid.y > 0);
        CHECK(std::abs(mid.y) ==
              doctest::Approx(std::abs(bulge) * 1.0).epsilon(1e-9));  // sagitta = b * L/2
        // All points at radius from the centre.
        for (double u : {0.0, 0.3, 0.77, 1.0})
            CHECK(distance(s.pointAtLocal(u), arc->center) ==
                  doctest::Approx(arc->radius).epsilon(1e-12));
        // Travel direction: tangent at start rotates CCW for positive bulge.
        CHECK(s.toLocal(s.curve.paramAt(mid)) == doctest::Approx(0.5).epsilon(1e-9));
    }
    CHECK_FALSE(bulgeArc({0, 0}, {1, 0}, 0.0).has_value());
    CHECK_FALSE(bulgeArc({1, 1}, {1, 1}, 1.0).has_value());
}

TEST_CASE("semicircle bulge has the chord's midpoint as centre") {
    auto a = bulgeArc({0, 0}, {2, 0}, 1.0);
    REQUIRE(a);
    CHECK(distance(a->center, Vec2{1, 0}) < 1e-12);
    CHECK(a->radius == doctest::Approx(1));
    CHECK(a->sweep == doctest::Approx(kPi));
}

TEST_CASE("bulgeThrough recovers the bulge from a middle point") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> bd(0.05, 3.0);
    for (int i = 0; i < 200; ++i) {
        double b = bd(rng) * (i % 2 ? 1 : -1);
        Vec2 p0{1, 2}, p1{5, -1};
        auto arc = bulgeArc(p0, p1, b);
        Segment s;
        s.curve = CurveRef::arc(arc->center, arc->radius, arc->startAngle, arc->sweep);
        s.reversed = arc->reversed;
        Vec2 m = s.pointAtLocal(0.37);
        auto back = bulgeThrough(p0, m, p1);
        REQUIRE(back.has_value());
        CHECK(*back == doctest::Approx(b).epsilon(1e-9));
    }
    CHECK_FALSE(bulgeThrough({0, 0}, {1, 1}, {2, 2}).has_value());
}

TEST_CASE("arcFromThreePoints") {
    auto a = arcFromThreePoints({1, 0}, {0, 1}, {-1, 0});
    REQUIRE(a.ok());
    CHECK(distance(a.value().center, Vec2{0, 0}) < 1e-12);
    CHECK(a.value().radius == doctest::Approx(1));
    CHECK(a.value().startAngle == doctest::Approx(0).epsilon(1e-9));
    CHECK(a.value().endAngle == doctest::Approx(kPi));
    // Clockwise travel gives the equivalent counter-clockwise arc.
    auto b = arcFromThreePoints({-1, 0}, {0, 1}, {1, 0});
    REQUIRE(b.ok());
    CHECK(b.value().startAngle == doctest::Approx(0).epsilon(1e-9));
    CHECK(b.value().endAngle == doctest::Approx(kPi));
    CHECK_FALSE(arcFromThreePoints({0, 0}, {1, 1}, {2, 2}).ok());
}

TEST_CASE("polyline segments keep indices and skip degenerate ones") {
    PolylineData pl;
    pl.v = {{{0, 0}, 0}, {{0, 0}, 0}, {{4, 0}, 1.0}, {{4, 4}, 0}};
    pl.closed = true;
    auto segs = segments(pl);
    REQUIRE(segs.size() == 3);
    CHECK(segs[0].index == 1);
    CHECK(segs[1].index == 2);
    CHECK(segs[1].curve.kind() == CurveRef::Kind::Arc);
    CHECK(segs[2].index == 3);
    CHECK(distance(segs[2].p1, Vec2{0, 0}) < 1e-12);
    CHECK(segmentCount(pl) == 4);
}

TEST_CASE("flatten stays within tolerance for ellipses and splines") {
    auto e = CurveRef::ellipse({0, 0}, {50, 0}, 0.1, 0, kTwoPi);
    for (double tol : {0.1, 0.01, 0.001}) {
        std::vector<Vec2> pts;
        std::vector<double> ts;
        flatten(e, tol, pts, &ts);
        REQUIRE(pts.size() == ts.size());
        double worst = 0;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            for (double f : {0.25, 0.5, 0.75}) {
                Vec2 q = e.pointAt(ts[i] + (ts[i + 1] - ts[i]) * f);
                Vec2 chord = pts[i + 1] - pts[i];
                double u = std::clamp(dot(q - pts[i], chord) / dot(chord, chord), 0.0, 1.0);
                worst = std::max(worst, distance(q, pts[i] + chord * u));
            }
        CHECK(worst <= tol * 1.5);
    }
}

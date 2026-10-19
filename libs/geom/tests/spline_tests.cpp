#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::geom;

TEST_CASE("interpolateSpline passes through every fit point") {
    std::vector<Vec2> fit = {{0, 0}, {1, 2}, {3, 3}, {4, 0}, {6, -1}, {7, 2}};
    for (bool natural : {true, false}) {
        InterpolateOptions o;
        o.natural = natural;
        auto r = interpolateSpline(fit, o);
        REQUIRE(r.ok());
        auto c = CurveRef::spline(r.value());
        REQUIRE(c.ok());
        // Fit points are hit at the chord-length parameters.
        double total = 0;
        std::vector<double> u{0};
        for (std::size_t i = 1; i < fit.size(); ++i) {
            total += distance(fit[i], fit[i - 1]);
            u.push_back(total);
        }
        for (std::size_t i = 0; i < fit.size(); ++i)
            CHECK(distance(c.value().pointAt(u[i] / total), fit[i]) < 1e-9);
        CHECK(distance(c.value().startPoint(), fit.front()) < 1e-12);
        CHECK(distance(c.value().endPoint(), fit.back()) < 1e-12);
    }
}

TEST_CASE("natural end conditions give zero curvature, tangent ends the requested direction") {
    std::vector<Vec2> fit = {{0, 0}, {2, 1}, {4, 0}, {6, 1}};
    auto nat = interpolateSpline(fit);
    REQUIRE(nat.ok());
    auto cn = CurveRef::spline(nat.value()).value();
    // Second derivative ~ 0 at both ends (finite difference on the derivative).
    double h = 1e-6;
    Vec2 d0 = (cn.derivativeAt(cn.t0() + h) - cn.derivativeAt(cn.t0())) / h;
    Vec2 d1 = (cn.derivativeAt(cn.t1()) - cn.derivativeAt(cn.t1() - h)) / h;
    CHECK(length(d0) < 1e-3);
    CHECK(length(d1) < 1e-3);

    InterpolateOptions o;
    o.startTangent = Vec2{0, 1};
    o.endTangent = Vec2{0, -1};
    auto tg = interpolateSpline(fit, o);
    REQUIRE(tg.ok());
    auto ct = CurveRef::spline(tg.value()).value();
    Vec2 t0 = ct.tangentAt(ct.t0()), t1 = ct.tangentAt(ct.t1());
    CHECK(t0.x == doctest::Approx(0).epsilon(1e-9));
    CHECK(t0.y == doctest::Approx(1).epsilon(1e-9));
    CHECK(t1.y == doctest::Approx(-1).epsilon(1e-9));
}

TEST_CASE("closed fit spline returns to its start with matching tangent") {
    std::vector<Vec2> fit = {{0, 0}, {3, 0}, {3, 3}, {0, 3}};
    InterpolateOptions o;
    o.closed = true;
    auto r = interpolateSpline(fit, o);
    REQUIRE(r.ok());
    CHECK(r.value().closed);
    auto c = CurveRef::spline(r.value()).value();
    CHECK(distance(c.startPoint(), c.endPoint()) < 1e-9);
    Vec2 a = c.tangentAt(c.t0()), b = c.tangentAt(c.t1());
    CHECK(distance(a, b) < 1e-9);
    CHECK(c.isClosed());
}

TEST_CASE("interpolateSpline rejects degenerate input") {
    CHECK_FALSE(interpolateSpline(std::vector<Vec2>{{1, 1}}).ok());
    CHECK_FALSE(interpolateSpline(std::vector<Vec2>{{1, 1}, {1, 1}}).ok());
    auto two = interpolateSpline(std::vector<Vec2>{{0, 0}, {10, 0}});
    REQUIRE(two.ok());
    auto c = CurveRef::spline(two.value()).value();
    CHECK(c.length() == doctest::Approx(10).epsilon(1e-9));
}

TEST_CASE("rational quadratic NURBS reproduces a circle arc exactly") {
    SplineData d;
    d.degree = 2;
    d.control = {{1, 0}, {1, 1}, {0, 1}};
    double w = std::sqrt(0.5);
    d.weights = {1, w, 1};
    d.knots = {0, 0, 0, 1, 1, 1};
    auto c = CurveRef::spline(d);
    REQUIRE(c.ok());
    for (int i = 0; i <= 20; ++i) {
        Vec2 p = c.value().pointAt(i / 20.0);
        CHECK(length(p) == doctest::Approx(1).epsilon(1e-12));
    }
    CHECK(c.value().length() == doctest::Approx(kPi / 2).epsilon(1e-10));
    // Derivative against finite differences.
    for (double t : {0.1, 0.4, 0.8}) {
        Vec2 num = (c.value().pointAt(t + 1e-6) - c.value().pointAt(t - 1e-6)) / 2e-6;
        CHECK(distance(num, c.value().derivativeAt(t)) < 1e-6);
    }
}

TEST_CASE("derivative of a non-rational spline matches finite differences") {
    std::vector<Vec2> fit = {{0, 0}, {1, 2}, {3, 3}, {4, 0}, {6, -1}};
    auto sd = interpolateSpline(fit).value();
    auto c = CurveRef::spline(sd).value();
    for (double t : {0.05, 0.3, 0.52, 0.9}) {
        Vec2 num = (c.pointAt(t + 1e-6) - c.pointAt(t - 1e-6)) / 2e-6;
        CHECK(distance(num, c.derivativeAt(t)) < 1e-5);
    }
}

TEST_CASE("splineSubcurve reproduces the original curve over the sub-range") {
    std::vector<Vec2> fit = {{0, 0}, {1, 2}, {3, 3}, {4, 0}, {6, -1}, {8, 4}};
    auto sd = interpolateSpline(fit).value();
    auto orig = CurveRef::spline(sd).value();
    double ta = 0.23, tb = 0.71;
    auto sub = splineSubcurve(sd, ta, tb);
    REQUIRE(sub.ok());
    auto sc = CurveRef::spline(sub.value()).value();
    CHECK(sc.t0() == doctest::Approx(ta));
    CHECK(sc.t1() == doctest::Approx(tb));
    for (int i = 0; i <= 30; ++i) {
        double t = ta + (tb - ta) * i / 30.0;
        CHECK(distance(sc.pointAt(t), orig.pointAt(t)) < 1e-10);
    }
    // Rational input keeps its weights.
    SplineData r;
    r.degree = 2;
    r.control = {{1, 0}, {1, 1}, {0, 1}};
    r.weights = {1, std::sqrt(0.5), 1};
    r.knots = {0, 0, 0, 1, 1, 1};
    auto rs = splineSubcurve(r, 0.25, 0.6);
    REQUIRE(rs.ok());
    auto rc = CurveRef::spline(rs.value()).value();
    for (double t : {0.25, 0.4, 0.6})
        CHECK(length(rc.pointAt(t)) == doctest::Approx(1).epsilon(1e-10));
}

TEST_CASE("spline closest point and bounds") {
    std::vector<Vec2> fit = {{0, 0}, {2, 3}, {5, 3}, {7, 0}};
    auto c = CurveRef::spline(interpolateSpline(fit).value()).value();
    BBox2 b = c.bounds();
    for (int i = 0; i <= 200; ++i)
        CHECK(b.contains(c.pointAt(c.t0() + (c.t1() - c.t0()) * i / 200.0)));
    // A point far above is closest to somewhere near the crest.
    Vec2 q{3.5, 10};
    Vec2 cp = c.closestPoint(q);
    // No sampled point may be closer than the reported one.
    for (int i = 0; i <= 2000; ++i) {
        Vec2 p = c.pointAt(c.t0() + (c.t1() - c.t0()) * i / 2000.0);
        CHECK(distance(p, q) >= distance(cp, q) - 1e-9);
    }
}

TEST_CASE("invalid spline data is rejected") {
    SplineData d;
    d.degree = 3;
    d.control = {{0, 0}, {1, 1}};
    CHECK_FALSE(CurveRef::spline(d).ok());
    d.control = {{0, 0}, {1, 1}, {2, 0}, {3, 1}};
    d.knots = {0, 0, 0, 0, 1, 1, 1};
    CHECK_FALSE(CurveRef::spline(d).ok());
    d.knots = {0, 0, 0, 1, 0, 1, 1, 1};
    CHECK_FALSE(CurveRef::spline(d).ok());
    d.knots = {};
    d.weights = {1, 1, -1, 1};
    CHECK_FALSE(CurveRef::spline(d).ok());
    d.weights = {};
    CHECK(CurveRef::spline(d).ok());  // clamped uniform knots are generated
}

TEST_CASE("paramAtLength inverts lengthBetween on a many-span spline and an ellipse") {
    std::vector<Vec2> fit;
    for (int i = 0; i <= 60; ++i)
        fit.push_back({i * 2.5, 10 * std::sin(i * 0.12)});
    auto r = interpolateSpline(fit);
    REQUIRE(r.ok());
    auto sc = CurveRef::spline(r.value());
    REQUIRE(sc.ok());
    const CurveRef ell = CurveRef::ellipse({3, 4}, {10, 2}, 0.4, 0.3, 4.0);
    for (const CurveRef& c : {sc.value(), ell}) {
        const double len = c.length();
        CHECK(c.paramAtLength(0) == doctest::Approx(c.t0()));
        CHECK(c.paramAtLength(len) == doctest::Approx(c.t1()));
        for (int i = 1; i < 40; ++i) {
            const double s = len * i / 40;
            const double t = c.paramAtLength(s);
            CHECK(t > c.t0());
            CHECK(t < c.t1());
            CHECK(c.lengthBetween(c.t0(), t) == doctest::Approx(s).epsilon(1e-9));
        }
    }
}

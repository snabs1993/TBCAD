#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::geom;

namespace {

std::vector<Intersection> ix(const CurveRef& a, const CurveRef& b, bool ea = false,
                             bool eb = false) {
    std::vector<Intersection> out;
    intersect(a, b, out, ea, eb);
    return out;
}

// Order-insensitive comparison of intersection points.
void expectPoints(const std::vector<Intersection>& got, std::vector<Vec2> want,
                  double tolMm = 1e-7) {
    REQUIRE(got.size() == want.size());
    for (const Intersection& g : got) {
        auto it = std::find_if(want.begin(), want.end(),
                               [&](const Vec2& w) { return distance(w, g.p) <= tolMm; });
        CHECK_MESSAGE(it != want.end(), "unexpected point (" << g.p.x << ", " << g.p.y << ")");
        if (it != want.end())
            want.erase(it);
    }
}

// Every reported point must lie on both curves, and the parameters must reproduce it.
void expectOnBoth(const CurveRef& a, const CurveRef& b, const std::vector<Intersection>& got,
                  double tolMm = 1e-7) {
    for (const Intersection& g : got) {
        CHECK(a.distanceTo(g.p) <= tolMm);
        CHECK(b.distanceTo(g.p) <= tolMm);
        CHECK(distance(a.pointAt(g.tA), g.p) <= tolMm);
        CHECK(distance(b.pointAt(g.tB), g.p) <= tolMm);
    }
}

CurveRef shifted(const CurveRef& c, Vec2 d) {
    switch (c.kind()) {
        case CurveRef::Kind::Line:
            return CurveRef::line(c.lineA() + d, c.lineB() + d);
        case CurveRef::Kind::Arc:
            return CurveRef::arc(c.center() + d, c.radius(), c.startAngle(), c.sweep());
        case CurveRef::Kind::Ellipse:
            return CurveRef::ellipse(c.center() + d, c.majorAxis(), c.ratio(), c.startAngle(),
                                     c.sweep());
        default:
            return c;
    }
}

CurveRef makeSpline(std::vector<Vec2> fit) {
    return CurveRef::spline(interpolateSpline(fit).value()).value();
}

}  // namespace

TEST_CASE("line-line") {
    auto a = CurveRef::line({0, 0}, {10, 10});
    auto b = CurveRef::line({0, 10}, {10, 0});
    expectPoints(ix(a, b), {{5, 5}});
    CHECK(ix(a, b)[0].tA == doctest::Approx(0.5));
    CHECK(ix(a, b)[0].tB == doctest::Approx(0.5));
    // T junction on an end point.
    expectPoints(ix(a, CurveRef::line({5, 5}, {5, 20})), {{5, 5}});
    // Miss unless extended.
    auto c = CurveRef::line({20, 0}, {20, 5});
    CHECK(ix(a, c).empty());
    expectPoints(ix(a, c, true, false), {});  // c does not reach y=20
    expectPoints(ix(a, CurveRef::line({20, 0}, {20, 5}), false, true), {});
    auto d = CurveRef::line({20, 0}, {20, 5});
    CHECK(ix(CurveRef::line({0, 0}, {10, 5}), d, true, false).empty());  // (20,10) is beyond d
    expectPoints(ix(CurveRef::line({0, 0}, {10, 2}), d, true, false), {{20, 4}});
}

TEST_CASE("line-line extension flags") {
    auto a = CurveRef::line({0, 0}, {10, 0});
    auto b = CurveRef::line({15, -5}, {15, 5});
    CHECK(ix(a, b).empty());
    auto r = ix(a, b, true, false);
    expectPoints(r, {{15, 0}});
    CHECK(r[0].tA == doctest::Approx(1.5));
    CHECK(ix(a, b, false, true).empty());
    expectPoints(ix(a, b, true, true), {{15, 0}});
    auto b2 = CurveRef::line({15, 2}, {15, 5});
    expectPoints(ix(a, b2, true, true), {{15, 0}});
    CHECK(ix(a, b2, true, false).empty());
}

TEST_CASE("line-line parallel, collinear and nearly parallel") {
    auto a = CurveRef::line({0, 0}, {10, 0});
    CHECK(ix(a, CurveRef::line({0, 1}, {10, 1})).empty());
    // Collinear overlap reports the overlap end points.
    expectPoints(ix(a, CurveRef::line({4, 0}, {14, 0})), {{4, 0}, {10, 0}});
    expectPoints(ix(a, CurveRef::line({-3, 0}, {20, 0})), {{0, 0}, {10, 0}});
    expectPoints(ix(a, CurveRef::line({3, 0}, {6, 0})), {{3, 0}, {6, 0}});
    // Collinear touching at one end.
    expectPoints(ix(a, CurveRef::line({10, 0}, {15, 0})), {{10, 0}});
    // Collinear, disjoint.
    CHECK(ix(a, CurveRef::line({12, 0}, {15, 0})).empty());
    // Direction reversed.
    expectPoints(ix(a, CurveRef::line({14, 0}, {4, 0})), {{4, 0}, {10, 0}});
    // Nearly parallel (1e-9 rad) crossing in the middle.
    double ang = 1e-9;
    auto n1 = CurveRef::line({0, 0}, {100, 0});
    auto n2 = CurveRef::line({50 - 50 * std::cos(ang), -50 * std::sin(ang)},
                             {50 + 50 * std::cos(ang), 50 * std::sin(ang)});
    auto r = ix(n1, n2);
    REQUIRE(r.size() == 1);
    CHECK(r[0].p.x == doctest::Approx(50).epsilon(1e-6));
    CHECK(n1.distanceTo(r[0].p) < 1e-9);
    CHECK(n2.distanceTo(r[0].p) < 1e-7);
}

TEST_CASE("line-line far from the origin") {
    Vec2 off{1e7, -1e7};
    auto a = shifted(CurveRef::line({0, 0}, {10, 10}), off);
    auto b = shifted(CurveRef::line({0, 10}, {10, 0}), off);
    expectPoints(ix(a, b), {Vec2{5, 5} + off}, 1e-6);
}

TEST_CASE("line-circle and line-arc") {
    auto circ = CurveRef::circle({0, 0}, 5);
    expectPoints(ix(CurveRef::line({-10, 0}, {10, 0}), circ), {{-5, 0}, {5, 0}});
    // Only one crossing inside the segment.
    expectPoints(ix(CurveRef::line({0, 0}, {10, 0}), circ), {{5, 0}});
    // Tangent: one point.
    auto tan = ix(CurveRef::line({-10, 5}, {10, 5}), circ);
    expectPoints(tan, {{0, 5}});
    // Miss.
    CHECK(ix(CurveRef::line({-10, 6}, {10, 6}), circ).empty());
    // Segment ends before the circle unless extended.
    CHECK(ix(CurveRef::line({0, 0}, {3, 0}), circ).empty());
    expectPoints(ix(CurveRef::line({0, 0}, {3, 0}), circ, true, false), {{-5, 0}, {5, 0}});
    // Parameters: line t and arc angle.
    auto r = ix(CurveRef::line({-10, 0}, {10, 0}), circ);
    for (const auto& h : r) {
        CHECK(std::abs(circ.pointAt(h.tB).x - h.p.x) < 1e-9);
        CHECK(std::abs(circ.pointAt(h.tB).y - h.p.y) < 1e-9);
    }
    // Arc limits the answer: upper half only.
    auto arc = CurveRef::arc({0, 0}, 5, 0, kPi);
    expectPoints(ix(CurveRef::line({-10, -2}, {10, -2}), arc), {});
    expectPoints(ix(CurveRef::line({-10, 3}, {10, 3}), arc), {{-4, 3}, {4, 3}});
    expectPoints(ix(CurveRef::line({-10, -3}, {10, -3}), arc, false, true), {{-4, -3}, {4, -3}});
    // Arc first, line second.
    auto sw = ix(arc, CurveRef::line({-10, 3}, {10, 3}));
    expectPoints(sw, {{-4, 3}, {4, 3}});
    expectOnBoth(arc, CurveRef::line({-10, 3}, {10, 3}), sw);
    // Line through an arc end point.
    expectPoints(ix(CurveRef::line({-10, 0}, {10, 0}), arc), {{-5, 0}, {5, 0}});
}

TEST_CASE("line-circle far from the origin") {
    Vec2 off{1e7, 1e7};
    auto circ = shifted(CurveRef::circle({0, 0}, 5), off);
    auto line = shifted(CurveRef::line({-10, 3}, {10, 3}), off);
    expectPoints(ix(line, circ), {Vec2{-4, 3} + off, Vec2{4, 3} + off}, 1e-6);
}

TEST_CASE("circle-circle and arc-arc") {
    auto a = CurveRef::circle({0, 0}, 5);
    expectPoints(ix(a, CurveRef::circle({6, 0}, 5)), {{3, 4}, {3, -4}});
    // External tangent.
    expectPoints(ix(a, CurveRef::circle({10, 0}, 5)), {{5, 0}});
    // Internal tangent.
    expectPoints(ix(a, CurveRef::circle({2, 0}, 3)), {{5, 0}});
    expectPoints(ix(CurveRef::circle({2, 0}, 3), a), {{5, 0}});
    // Disjoint, contained, concentric.
    CHECK(ix(a, CurveRef::circle({20, 0}, 5)).empty());
    CHECK(ix(a, CurveRef::circle({1, 0}, 1)).empty());
    CHECK(ix(a, CurveRef::circle({0, 0}, 3)).empty());
    // Identical circles: no discrete intersections.
    CHECK(ix(a, CurveRef::circle({0, 0}, 5)).empty());
    // Arcs restrict the result.
    auto upper = CurveRef::arc({0, 0}, 5, 0, kPi);
    expectPoints(ix(upper, CurveRef::circle({6, 0}, 5)), {{3, 4}});
    auto lower = CurveRef::arc({6, 0}, 5, kPi, kPi);  // lower half of the second circle
    CHECK(ix(upper, lower).empty());
    expectPoints(ix(upper, lower, true, false), {{3, -4}});
}

TEST_CASE("arc-arc extension and coincident arcs") {
    auto upper = CurveRef::arc({0, 0}, 5, 0, kPi);
    auto other = CurveRef::arc({6, 0}, 5, kPi, kPi);  // lower half
    // Extending the first arc to a full circle exposes the lower crossing point of the arcs'
    // circles with the second (lower) arc.
    expectPoints(ix(upper, other, true, false), {{3, -4}});
    expectPoints(ix(upper, other, false, true), {{3, 4}});
    // Coincident arcs overlap on [pi/2, pi].
    auto a = CurveRef::arc({0, 0}, 5, 0, kPi);
    auto b = CurveRef::arc({0, 0}, 5, kPi / 2, kPi);
    expectPoints(ix(a, b), {{0, 5}, {-5, 0}});
    // Arc fully inside another: its end points.
    auto c = CurveRef::arc({0, 0}, 5, kPi / 4, kPi / 4);
    expectPoints(ix(a, c), {c.startPoint(), c.endPoint()});
    // Touching end to end.
    auto d = CurveRef::arc({0, 0}, 5, kPi, kPi / 2);
    expectPoints(ix(a, d), {{-5, 0}});
}

TEST_CASE("circle-circle far from the origin") {
    Vec2 off{-1e7, 1e7};
    auto a = shifted(CurveRef::circle({0, 0}, 5), off);
    auto b = shifted(CurveRef::circle({6, 0}, 5), off);
    expectPoints(ix(a, b), {Vec2{3, 4} + off, Vec2{3, -4} + off}, 1e-6);
}

TEST_CASE("line-ellipse (closed form)") {
    auto e = CurveRef::ellipse({0, 0}, {4, 0}, 0.5, 0, kTwoPi);
    expectPoints(ix(CurveRef::line({-10, 0}, {10, 0}), e), {{-4, 0}, {4, 0}});
    expectPoints(ix(CurveRef::line({-10, 2}, {10, 2}), e), {{0, 2}});  // tangent at the top
    CHECK(ix(CurveRef::line({-10, 2.1}, {10, 2.1}), e).empty());
    // Diagonal through the centre.
    double x = std::sqrt(1.0 / (1.0 / 16 + 1.0 / 4));  // x^2/16 + x^2/4 = 1
    expectPoints(ix(CurveRef::line({-10, -10}, {10, 10}), e), {{-x, -x}, {x, x}});
    // Elliptical arc limits.
    auto upper = CurveRef::ellipse({0, 0}, {4, 0}, 0.5, 0, kPi);
    expectPoints(ix(CurveRef::line({-10, 1}, {10, 1}), upper),
                 {{-4 * std::sqrt(0.75), 1}, {4 * std::sqrt(0.75), 1}});
    CHECK(ix(CurveRef::line({-10, -1}, {10, -1}), upper).empty());
    expectPoints(ix(CurveRef::line({-10, -1}, {10, -1}), upper, false, true),
                 {{-4 * std::sqrt(0.75), -1}, {4 * std::sqrt(0.75), -1}});
    // Rotated ellipse; swap order.
    auto rot = CurveRef::ellipse({1, 1}, {0, 4}, 0.5, 0, kTwoPi);
    auto r = ix(rot, CurveRef::line({-10, 1}, {10, 1}));
    expectPoints(r, {{-1, 1}, {3, 1}});
    expectOnBoth(rot, CurveRef::line({-10, 1}, {10, 1}), r);
}

TEST_CASE("ellipse-circle and ellipse-ellipse (numeric)") {
    auto e = CurveRef::ellipse({0, 0}, {4, 0}, 0.5, 0, kTwoPi);
    auto c = CurveRef::circle({0, 0}, 3);
    // x^2/16 + y^2/4 = 1 and x^2 + y^2 = 9  ->  x^2 = 16*(9-4)/12 ... solve exactly.
    double x2 = (9.0 - 4.0) / (1.0 - 0.25);
    double y2 = 9.0 - x2;
    double x = std::sqrt(x2), y = std::sqrt(y2);
    auto r = ix(e, c);
    expectPoints(r, {{x, y}, {-x, y}, {x, -y}, {-x, -y}});
    expectOnBoth(e, c, r, 1e-8);
    auto r2 = ix(c, e);
    expectPoints(r2, {{x, y}, {-x, y}, {x, -y}, {-x, -y}});

    auto e2 = CurveRef::ellipse({0, 0}, {0, 4}, 0.5, 0, kTwoPi);
    double q = std::sqrt(16.0 / 5.0);
    auto r3 = ix(e, e2);
    expectPoints(r3, {{q, q}, {-q, q}, {q, -q}, {-q, -q}}, 1e-7);
    expectOnBoth(e, e2, r3, 1e-8);

    // Tangent contact: circle r=2 touches the ellipse at the top (0,2) and bottom.
    auto tc = CurveRef::circle({0, 0}, 2);
    auto rt = ix(e, tc);
    REQUIRE_FALSE(rt.empty());
    for (const auto& h : rt) {
        CHECK(e.distanceTo(h.p) < 1e-7);
        CHECK(tc.distanceTo(h.p) < 1e-7);
        CHECK(std::abs(h.p.x) < 1e-3);  // contacts at (0, +-2)
    }

    // Elliptical arc restriction.
    auto upperArc = CurveRef::ellipse({0, 0}, {4, 0}, 0.5, 0, kPi);
    expectPoints(ix(upperArc, c), {{x, y}, {-x, y}});
    // Extension of a partial ellipse to the full one.
    expectPoints(ix(upperArc, c, true, false), {{x, y}, {-x, y}, {x, -y}, {-x, -y}});
}

TEST_CASE("spline intersections (numeric)") {
    auto s = makeSpline({{0, 0}, {2, 3}, {5, 3}, {7, 0}, {10, -2}});
    // Horizontal line at y = 1.5 crosses the arch twice or more.
    auto line = CurveRef::line({-1, 1.5}, {11, 1.5});
    auto r = ix(s, line);
    REQUIRE(r.size() >= 2);
    expectOnBoth(s, line, r, 1e-8);
    for (const auto& h : r)
        CHECK(h.p.y == doctest::Approx(1.5).epsilon(1e-9));
    auto r2 = ix(line, s);
    CHECK(r2.size() == r.size());
    // Spline vs circle.
    auto circ = CurveRef::circle({5, 0}, 3);
    auto rc = ix(s, circ);
    REQUIRE_FALSE(rc.empty());
    expectOnBoth(s, circ, rc, 1e-8);
    // Spline vs spline.
    auto s2 = makeSpline({{0, 3}, {3, 0}, {6, 4}, {10, 0}});
    auto rs = ix(s, s2);
    REQUIRE_FALSE(rs.empty());
    expectOnBoth(s, s2, rs, 1e-8);
    // Line extended to reach the spline.
    auto shortLine = CurveRef::line({-3, 1.5}, {-2, 1.5});
    CHECK(ix(shortLine, s).empty());
    auto ext = ix(shortLine, s, true, false);
    REQUIRE(ext.size() == r.size());
    for (const auto& h : ext)
        CHECK(shortLine.pointAt(h.tA).y == doctest::Approx(1.5));
    // Tangent: horizontal line touching the crest. Crest y is max of the spline.
    double top = -1e9;
    double tTop = 0;
    for (int i = 0; i <= 20000; ++i) {
        double t = s.t0() + (s.t1() - s.t0()) * i / 20000.0;
        double y = s.pointAt(t).y;
        if (y > top) {
            top = y;
            tTop = t;
        }
    }
    (void)tTop;
    auto touch = ix(s, CurveRef::line({-1, top}, {11, top}));
    REQUIRE_FALSE(touch.empty());
    for (const auto& h : touch)
        CHECK(h.p.y == doctest::Approx(top).epsilon(1e-6));
    CHECK(ix(s, CurveRef::line({-1, top + 0.01}, {11, top + 0.01})).empty());
}

TEST_CASE("numeric intersections far from the origin") {
    Vec2 off{1e7, 1e7};
    auto e = shifted(CurveRef::ellipse({0, 0}, {4, 0}, 0.5, 0, kTwoPi), off);
    auto c = shifted(CurveRef::circle({0, 0}, 3), off);
    double x2 = 5.0 / 0.75, y2 = 9.0 - x2;
    double x = std::sqrt(x2), y = std::sqrt(y2);
    auto r = ix(e, c);
    expectPoints(r, {Vec2{x, y} + off, Vec2{-x, y} + off, Vec2{x, -y} + off, Vec2{-x, -y} + off},
                 1e-6);
}

TEST_CASE("polyline with bulges (entity level)") {
    // Stadium: 3 long, 2 high, closed, semicircular ends.
    Entity stadium{{},
                   PolylineData{{{{0, 0}, 0}, {{3, 0}, 1.0}, {{3, 2}, 0}, {{0, 2}, 1.0}}, true, 0}};
    Entity vline{{}, LineData{{1.5, -5}, {1.5, 5}}};
    std::vector<EntityIntersection> r;
    REQUIRE(intersectEntities(stadium, vline, r).ok());
    REQUIRE(r.size() == 2);
    std::sort(r.begin(), r.end(), [](auto& a, auto& b) { return a.p.y < b.p.y; });
    CHECK(distance(r[0].p, Vec2{1.5, 0}) < 1e-9);
    CHECK(distance(r[1].p, Vec2{1.5, 2}) < 1e-9);
    CHECK(r[0].tA == doctest::Approx(0.5));  // segment 0, halfway
    CHECK(r[1].tA == doctest::Approx(2.5));  // segment 2 (3,2)->(0,2), halfway
    // Horizontal line through the middle: hits the two semicircles at x = -1 and x = 4.
    r.clear();
    Entity hline{{}, LineData{{-5, 1}, {10, 1}}};
    REQUIRE(intersectEntities(stadium, hline, r).ok());
    REQUIRE(r.size() == 2);
    std::sort(r.begin(), r.end(), [](auto& a, auto& b) { return a.p.x < b.p.x; });
    CHECK(distance(r[0].p, Vec2{-1, 1}) < 1e-9);
    CHECK(distance(r[1].p, Vec2{4, 1}) < 1e-9);
    CHECK(r[1].tA == doctest::Approx(1.5).epsilon(1e-9));  // arc segment 1, halfway
    CHECK(r[0].tA == doctest::Approx(3.5).epsilon(1e-9));
    // Polyline vs circle through the shared vertex does not duplicate the hit.
    r.clear();
    Entity circ{{}, CircleData{{3, 0}, 1.0}};
    REQUIRE(intersectEntities(stadium, circ, r).ok());
    for (std::size_t i = 0; i < r.size(); ++i)
        for (std::size_t j = i + 1; j < r.size(); ++j)
            CHECK(distance(r[i].p, r[j].p) > 1e-6);
    // Unsupported kinds report an error.
    Entity txt{{}, TextData{}};
    CHECK_FALSE(intersectEntities(stadium, txt, r).ok());
}

TEST_CASE("open polyline extension applies to its end segments only") {
    Entity pl{{}, PolylineData{{{{0, 0}, 0}, {{10, 0}, 0}, {{10, 10}, 0}}, false, 0}};
    Entity cut{{}, LineData{{-5, -5}, {-5, 5}}};  // beyond the start of the first segment
    std::vector<EntityIntersection> r;
    REQUIRE(intersectEntities(pl, cut, r).ok());
    CHECK(r.empty());
    REQUIRE(intersectEntities(pl, cut, r, true, false).ok());
    REQUIRE(r.size() == 1);
    CHECK(distance(r[0].p, Vec2{-5, 0}) < 1e-12);
    // A line crossing the middle segment's extension (x=10 line y=-5..) is not an end segment
    // extension hit unless it is the last segment.
    r.clear();
    Entity up{{}, LineData{{5, 5}, {5, 15}}};  // above the first segment, which is not "last"
    REQUIRE(intersectEntities(pl, up, r, true, false).ok());
    CHECK(r.empty());
    Entity beyondEnd{{}, LineData{{5, 15}, {15, 15}}};  // beyond the end of the last segment
    REQUIRE(intersectEntities(pl, beyondEnd, r, true, false).ok());
    REQUIRE(r.size() == 1);
    CHECK(distance(r[0].p, Vec2{10, 15}) < 1e-12);
}

namespace {

CurveRef randomCurve(std::mt19937& rng, int kind) {
    std::uniform_real_distribution<double> pos(-20, 20), rad(1, 15), ang(0, kTwoPi),
        ratio(0.2, 1.0);
    Vec2 c{pos(rng), pos(rng)};
    switch (kind) {
        case 0:
            return CurveRef::line({pos(rng), pos(rng)}, {pos(rng), pos(rng)});
        case 1:
            return CurveRef::circle(c, rad(rng));
        case 2:
            return CurveRef::arc(c, rad(rng), ang(rng), 0.5 + 5 * std::abs(std::sin(ang(rng))));
        case 3: {
            double a = rad(rng), t = ang(rng);
            return CurveRef::ellipse(c, {a * std::cos(t), a * std::sin(t)}, ratio(rng), 0, kTwoPi);
        }
        default: {
            std::vector<Vec2> fit;
            for (int i = 0; i < 6; ++i)
                fit.push_back({pos(rng), pos(rng)});
            auto r = interpolateSpline(fit);
            return CurveRef::spline(r.value()).value();
        }
    }
}

}  // namespace

TEST_CASE("property: reported intersections lie on both curves") {
    std::mt19937 rng(12345);
    int checked = 0;
    for (int iter = 0; iter < 400; ++iter) {
        int ka = iter % 5, kb = (iter / 5) % 5;
        CurveRef a = randomCurve(rng, ka), b = randomCurve(rng, kb);
        for (bool ext : {false, true}) {
            auto r = ix(a, b, ext, ext);
            for (const Intersection& h : r) {
                CHECK(a.pointAt(h.tA).x == doctest::Approx(h.p.x).epsilon(1e-9));
                double da = ext ? distance(a.pointAt(h.tA), h.p) : a.distanceTo(h.p);
                double db = ext ? distance(b.pointAt(h.tB), h.p) : b.distanceTo(h.p);
                CHECK(da <= 1e-7);
                CHECK(db <= 1e-7);
                ++checked;
            }
        }
    }
    CHECK(checked > 200);
}

TEST_CASE("property: line-line against brute force") {
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> pos(-10, 10);
    for (int i = 0; i < 2000; ++i) {
        Vec2 a0{pos(rng), pos(rng)}, a1{pos(rng), pos(rng)}, b0{pos(rng), pos(rng)},
            b1{pos(rng), pos(rng)};
        auto r = ix(CurveRef::line(a0, a1), CurveRef::line(b0, b1));
        // Brute force by sampling the first segment for sign changes of the side of b.
        Vec2 d = b1 - b0;
        auto side = [&](Vec2 p) { return cross(d, p - b0); };
        bool signChange = side(a0) * side(a1) <= 0;
        double u = 0;
        if (signChange && std::abs(side(a0) - side(a1)) > 1e-12) {
            double t = side(a0) / (side(a0) - side(a1));
            Vec2 p = lerp(a0, a1, t);
            u = dot(p - b0, d) / dot(d, d);
            bool expectHit = u >= 0 && u <= 1;
            if (std::abs(u) > 1e-6 && std::abs(u - 1) > 1e-6)
                CHECK((r.size() == 1) == expectHit);
        } else if (!signChange) {
            CHECK(r.empty());
        }
    }
}

#include <doctest/doctest.h>

#include <algorithm>

#include "tbcad/geom/polybool.h"

using namespace tbcad;
using namespace tbcad::geom;

namespace {
Ring rect(double x0, double y0, double x1, double y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}
Region reg(std::initializer_list<Ring> rs) {
    Region r;
    r.rings = rs;
    return r;
}
}  // namespace

TEST_CASE("overlapping squares: union, intersection, difference") {
    const Region a = reg({rect(0, 0, 10, 10)}), b = reg({rect(5, 5, 15, 15)});
    CHECK(area(boolean(a, b, BoolOp::Union)) == doctest::Approx(175));
    CHECK(area(boolean(a, b, BoolOp::Intersect)) == doctest::Approx(25));
    CHECK(area(boolean(a, b, BoolOp::Subtract)) == doctest::Approx(75));
    CHECK(area(boolean(b, a, BoolOp::Subtract)) == doctest::Approx(75));
    CHECK(boolean(a, b, BoolOp::Union).rings.size() == 1);
    CHECK(boolean(a, b, BoolOp::Union).rings[0].size() == 8);
}

TEST_CASE("disjoint and contained regions") {
    const Region a = reg({rect(0, 0, 10, 10)});
    const Region far = reg({rect(20, 0, 30, 10)});
    CHECK(boolean(a, far, BoolOp::Union).rings.size() == 2);
    CHECK(boolean(a, far, BoolOp::Intersect).empty());
    CHECK(area(boolean(a, far, BoolOp::Subtract)) == doctest::Approx(100));

    const Region inner = reg({rect(2, 2, 4, 4)});
    CHECK(area(boolean(a, inner, BoolOp::Union)) == doctest::Approx(100));
    CHECK(area(boolean(a, inner, BoolOp::Intersect)) == doctest::Approx(4));
    Region holed = boolean(a, inner, BoolOp::Subtract);
    CHECK(holed.rings.size() == 2);
    CHECK(area(holed) == doctest::Approx(96));
    CHECK_FALSE(contains(holed, {3, 3}, 0));
    CHECK(contains(holed, {8, 8}));
    CHECK(boolean(inner, a, BoolOp::Subtract).empty());
}

TEST_CASE("shared edges: touching squares merge, equal squares are one") {
    const Region a = reg({rect(0, 0, 10, 10)}), right = reg({rect(10, 0, 20, 10)});
    Region u = boolean(a, right, BoolOp::Union);
    REQUIRE(u.rings.size() == 1);
    CHECK(u.rings[0].size() == 4);  // the shared edge and the collinear middle points are gone
    CHECK(area(u) == doctest::Approx(200));
    CHECK(boolean(a, right, BoolOp::Intersect).empty());
    CHECK(area(boolean(a, right, BoolOp::Subtract)) == doctest::Approx(100));

    CHECK(area(boolean(a, a, BoolOp::Union)) == doctest::Approx(100));
    CHECK(area(boolean(a, a, BoolOp::Intersect)) == doctest::Approx(100));
    CHECK(boolean(a, a, BoolOp::Subtract).empty());
}

TEST_CASE(
    "input orientation does not matter; a clockwise ring is read like a counter-clockwise one") {
    Ring cw = rect(0, 0, 10, 10);
    std::reverse(cw.begin(), cw.end());
    CHECK(area(boolean(reg({cw}), reg({rect(5, 5, 15, 15)}), BoolOp::Intersect)) ==
          doctest::Approx(25));
}

TEST_CASE("a plus shape from two bars, and a notch cut with subtract") {
    const Region h = reg({rect(0, 4, 12, 8)}), v = reg({rect(4, 0, 8, 12)});
    Region plus = boolean(h, v, BoolOp::Union);
    REQUIRE(plus.rings.size() == 1);
    CHECK(plus.rings[0].size() == 12);
    CHECK(area(plus) == doctest::Approx(48 + 48 - 16));
    Region cut = boolean(reg({rect(0, 0, 10, 10)}), reg({rect(8, 3, 12, 7)}), BoolOp::Subtract);
    CHECK(area(cut) == doctest::Approx(100 - 8));
    CHECK(cut.rings[0].size() == 8);
}

TEST_CASE("a region with a hole minus a patch over the hole") {
    Region frame = boolean(reg({rect(0, 0, 10, 10)}), reg({rect(3, 3, 7, 7)}), BoolOp::Subtract);
    Region r = boolean(frame, reg({rect(2, 2, 8, 8)}), BoolOp::Subtract);
    CHECK(area(r) == doctest::Approx(100 - 36));
    Region filled = boolean(frame, reg({rect(3, 3, 7, 7)}), BoolOp::Union);
    CHECK(area(filled) == doctest::Approx(100));
    CHECK(filled.rings.size() == 1);
}

#include <cmath>
#include <random>

namespace {
// True when no two edges of the ring meet except neighbours at their shared vertex.
bool simpleRing(const Ring& r) {
    const auto cross = [](Vec2 o, Vec2 a, Vec2 b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    const auto onSeg = [](Vec2 a, Vec2 b, Vec2 p) {
        return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) &&
               std::min(a.y, b.y) <= p.y && p.y <= std::max(a.y, b.y);
    };
    const std::size_t n = r.size();
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j == n - 1))
                continue;
            const Vec2 a = r[i], b = r[(i + 1) % n], c = r[j], d = r[(j + 1) % n];
            const double d1 = cross(a, b, c), d2 = cross(a, b, d), d3 = cross(c, d, a),
                         d4 = cross(c, d, b);
            if (((d1 > 0) != (d2 > 0) && d1 != 0 && d2 != 0 && (d3 > 0) != (d4 > 0) && d3 != 0 &&
                 d4 != 0) ||
                (d1 == 0 && onSeg(a, b, c)) || (d2 == 0 && onSeg(a, b, d)) ||
                (d3 == 0 && onSeg(c, d, a)) || (d4 == 0 && onSeg(c, d, b)))
                return false;
        }
    return true;
}

// A star-shaped polygon around (cx, cy); `snap` > 0 rounds the vertices to a grid so that edges of
// different polygons often overlap or touch.
Ring randomStar(std::mt19937& rng, double cx, double cy, double snap) {
    std::uniform_real_distribution<double> unit(0, 1);
    const int n = 3 + static_cast<int>(rng() % 8);
    std::vector<double> angles;
    for (int i = 0; i < n; ++i)
        angles.push_back(unit(rng) * 2 * 3.14159265358979);
    std::sort(angles.begin(), angles.end());
    // The outline only winds once around the centre when no gap between neighbours reaches half a
    // turn; otherwise the closing edge would cut across the others.
    double gap = angles.front() + 2 * 3.14159265358979 - angles.back();
    for (std::size_t i = 1; i < angles.size(); ++i)
        gap = std::max(gap, angles[i] - angles[i - 1]);
    if (gap > 3.0)
        return {};
    Ring ring;
    for (double t : angles) {
        const double r = 2 + 8 * unit(rng);
        Vec2 p{cx + r * std::cos(t), cy + r * std::sin(t)};
        if (snap > 0) {
            p.x = std::round(p.x / snap) * snap;
            p.y = std::round(p.y / snap) * snap;
        }
        ring.push_back(p);
    }
    return simpleRing(ring) ? ring : Ring{};
}
}  // namespace

TEST_CASE("random polygons: the boolean operations agree with area and point tests") {
    std::mt19937 rng(4242);
    std::uniform_real_distribution<double> pos(-6, 6), probe(-20, 20);
    int checked = 0;
    for (int iter = 0; iter < 1500; ++iter) {
        const double snap = iter % 2 == 0 ? 0 : 1.0;
        const Ring ra = randomStar(rng, pos(rng), pos(rng), snap);
        const Ring rb = randomStar(rng, pos(rng), pos(rng), snap);
        if (ra.size() < 3 || rb.size() < 3)
            continue;
        const Region a = reg({ra}), b = reg({rb});
        if (std::abs(area(normalized(a))) < 1 || std::abs(area(normalized(b))) < 1)
            continue;
        const Region u = boolean(a, b, BoolOp::Union), i = boolean(a, b, BoolOp::Intersect),
                     d = boolean(a, b, BoolOp::Subtract);
        const double aa = area(normalized(a)), ab = area(normalized(b));
        CAPTURE(iter);
        CHECK(area(u) + area(i) == doctest::Approx(aa + ab).epsilon(1e-6));
        CHECK(area(d) == doctest::Approx(aa - area(i)).epsilon(1e-6));
        for (int k = 0; k < 20; ++k) {
            const Vec2 p{probe(rng), probe(rng)};
            // Skip points that sit on an outline of the inputs or the results.
            const auto near = [&](const Region& r) {
                for (const Ring& ring : r.rings)
                    for (std::size_t j = 0; j < ring.size(); ++j) {
                        const Vec2 s = ring[j], e = ring[(j + 1) % ring.size()];
                        const Vec2 v = e - s;
                        const double len2 = v.x * v.x + v.y * v.y;
                        double t = len2 > 0 ? ((p.x - s.x) * v.x + (p.y - s.y) * v.y) / len2 : 0;
                        t = std::max(0.0, std::min(1.0, t));
                        const Vec2 q{s.x + t * v.x, s.y + t * v.y};
                        if (std::hypot(p.x - q.x, p.y - q.y) < 1e-3)
                            return true;
                    }
                return false;
            };
            if (near(a) || near(b) || near(u) || near(i) || near(d))
                continue;
            const bool ia = contains(normalized(a), p), ib = contains(normalized(b), p);
            CHECK(contains(u, p) == (ia || ib));
            CHECK(contains(i, p) == (ia && ib));
            CHECK(contains(d, p) == (ia && !ib));
        }
        ++checked;
    }
    CHECK(checked > 200);
}

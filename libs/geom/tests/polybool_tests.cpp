#include <doctest/doctest.h>

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

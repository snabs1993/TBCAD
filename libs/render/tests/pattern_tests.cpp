#include <doctest/doctest.h>

#include "tbcad/render/pattern.h"

using namespace tbcad::render;

TEST_CASE("dash pattern period and continuity") {
    CHECK(DashPattern{}.continuous());
    CHECK(DashPattern{{0, 0}}.continuous());
    DashPattern dashed{{5, -3}};
    CHECK_FALSE(dashed.continuous());
    CHECK(dashed.period() == doctest::Approx(8));
    DashPattern center{{12, -2, 2, -2}};
    CHECK(center.period() == doctest::Approx(18));
}

TEST_CASE("isDash follows + dash, - gap") {
    DashPattern p{{5, -3}};
    const double px = 0.01;
    CHECK(p.isDash(0, px));
    CHECK(p.isDash(4.99, px));
    CHECK_FALSE(p.isDash(5.01, px));
    CHECK_FALSE(p.isDash(7.99, px));
    CHECK(p.isDash(8.01, px));  // second period
    CHECK(p.isDash(8.0 * 1000 + 1, px));
    CHECK(p.isDash(-3.5, px));  // negative distances wrap
}

TEST_CASE("a dot is one pixel long and takes no length") {
    DashPattern p{{4, -2, 0, -2}};  // dash gap dot gap, period 8
    const double px = 0.1;
    CHECK(p.period() == doctest::Approx(8));
    CHECK(p.isDash(1, px));
    CHECK_FALSE(p.isDash(5, px));
    CHECK(p.isDash(6.05, px));  // the dot at 6
    CHECK_FALSE(p.isDash(6.5, px));
}

TEST_CASE("patterns that would shimmer are drawn solid") {
    DashPattern p{{1, -1}};
    CHECK_FALSE(p.solidAt(100));  // 200 px per period
    CHECK(p.solidAt(0.5));        // 1 px per period
    CHECK(DashPattern{}.solidAt(1000));
}

TEST_CASE("wrapDashDistance keeps distances small without breaking phase") {
    const double period = 8;
    CHECK(wrapDashDistance(8.0 * 12345 + 3, period) == doctest::Approx(3).epsilon(1e-9));
    CHECK(wrapDashDistance(-1, period) == doctest::Approx(7));
    CHECK(wrapDashDistance(17.5, 0) == 17.5);
    DashPattern p{{5, -3}};
    for (double d : {0.2, 4.9, 5.1, 7.9, 1e6 + 0.5}) {
        CHECK(p.isDash(d, 0.01) == p.isDash(wrapDashDistance(d, period), 0.01));
    }
}

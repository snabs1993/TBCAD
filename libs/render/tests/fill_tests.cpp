#include <doctest/doctest.h>

#include "tbcad/render/fill.h"

using namespace tbcad;
using namespace tbcad::render;

namespace {
std::vector<Vec2> square(double x, double y, double s) {
    return {{x, y}, {x + s, y}, {x + s, y + s}, {x, y + s}};
}
}  // namespace

TEST_CASE("a convex ring becomes n-2 triangles") {
    auto tris = triangulateEvenOdd({square(0, 0, 10)});
    CHECK(tris.size() == 6);
    CHECK(triangleArea(tris) == doctest::Approx(100));
}

TEST_CASE("concave outline keeps its area") {
    std::vector<Vec2> l = {{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}};
    auto tris = triangulateEvenOdd({l});
    CHECK(triangleArea(tris) == doctest::Approx(10 * 4 + 4 * 6));
}

TEST_CASE("a ring inside another is a hole") {
    auto tris = triangulateEvenOdd({square(0, 0, 10), square(2, 2, 4)});
    CHECK(triangleArea(tris) == doctest::Approx(100 - 16));
}

TEST_CASE("islands inside holes are filled again (even-odd)") {
    auto tris = triangulateEvenOdd({square(0, 0, 20), square(2, 2, 16), square(6, 6, 8)});
    CHECK(triangleArea(tris) == doctest::Approx(400 - 256 + 64));
}

TEST_CASE("several outlines and several holes") {
    std::vector<std::vector<Vec2>> rings = {square(0, 0, 10), square(1, 1, 2), square(5, 5, 2),
                                            square(30, 0, 10), square(32, 2, 3)};
    auto tris = triangulateEvenOdd(rings);
    CHECK(triangleArea(tris) == doctest::Approx(100 - 4 - 4 + 100 - 9));
}

TEST_CASE("hole assignment does not depend on ring order") {
    auto a = triangulateEvenOdd({square(2, 2, 4), square(0, 0, 10)});
    CHECK(triangleArea(a) == doctest::Approx(84));
}

TEST_CASE("degenerate rings are ignored") {
    CHECK(triangulateEvenOdd({}).empty());
    CHECK(triangulateEvenOdd({{{0, 0}, {1, 1}}}).empty());
    std::vector<Vec2> nan = {{0, 0}, {1, 0}, {std::nan(""), 1}};
    CHECK(triangulateEvenOdd({nan}).empty());
    auto tris = triangulateEvenOdd({nan, square(0, 0, 3)});
    CHECK(triangleArea(tris) == doctest::Approx(9));
}

TEST_CASE("far coordinates triangulate without losing area") {
    auto tris = triangulateEvenOdd({square(1e8, -1e8, 100), square(1e8 + 10, -1e8 + 10, 20)});
    CHECK(triangleArea(tris) == doctest::Approx(10000 - 400).epsilon(1e-6));
}

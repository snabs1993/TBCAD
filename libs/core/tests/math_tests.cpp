#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "tbcad/core/math.h"

using namespace tbcad;

namespace {
bool near(const Vec2& a, const Vec2& b, double eps = 1e-9) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps;
}
}  // namespace

TEST_CASE("Vec2 arithmetic and products") {
    Vec2 a{3, 4};
    Vec2 b{-1, 2};
    CHECK(a + b == Vec2{2, 6});
    CHECK(a - b == Vec2{4, 2});
    CHECK(a * 2 == Vec2{6, 8});
    CHECK(2 * a == Vec2{6, 8});
    CHECK(a / 2 == Vec2{1.5, 2});
    CHECK(-a == Vec2{-3, -4});
    CHECK(dot(a, b) == 5);
    CHECK(cross(a, b) == 10);
    CHECK(length(a) == doctest::Approx(5));
    CHECK(lengthSquared(a) == 25);
    CHECK(perp(Vec2{1, 0}) == Vec2{0, 1});
    CHECK(lerp(a, b, 0.5) == Vec2{1, 3});
    CHECK(distance(a, b) == doctest::Approx(std::sqrt(20.0)));
}

TEST_CASE("Vec2 degenerate cases") {
    CHECK(normalized(Vec2{0, 0}) == Vec2{0, 0});
    CHECK(length(normalized(Vec2{3e-300, 4e-300})) == doctest::Approx(1));
    CHECK(length(Vec2{1e200, 1e200}) == doctest::Approx(std::sqrt(2.0) * 1e200));
    CHECK(cross(Vec2{1, 2}, Vec2{2, 4}) == 0);
    CHECK(isFinite(Vec2{1, 2}));
    CHECK_FALSE(isFinite(Vec2{std::nan(""), 2}));
    CHECK_FALSE(isFinite(Vec2{1, std::numeric_limits<double>::infinity()}));
}

TEST_CASE("tolerances scale with magnitude") {
    CHECK(tol::nearlyEqual(0.0, 5e-10));
    CHECK_FALSE(tol::nearlyEqual(0.0, 5e-9));
    // 1e9 is allowed to differ by 1e9 * 1e-9 = 1.
    CHECK(tol::nearlyEqual(1e9, 1e9 + 0.5));
    CHECK_FALSE(tol::nearlyEqual(1e9, 1e9 + 2));
    CHECK(tol::nearlyEqual(Vec2{1, 2}, Vec2{1, 2}));
    CHECK(tol::isZeroLength(5e-7));
    CHECK_FALSE(tol::isZeroLength(2e-6));
    CHECK(tol::isZeroLength(Vec2{3e-7, 3e-7}));
    CHECK(tol::nearlyEqualAngle(0.0, kTwoPi));
    CHECK(tol::nearlyEqualAngle(-0.5, kTwoPi - 0.5));
    CHECK_FALSE(tol::nearlyEqualAngle(0.0, 0.001));
}

TEST_CASE("normalizeAngle wraps into one turn") {
    CHECK(normalizeAngle(0) == 0);
    CHECK(normalizeAngle(kTwoPi) == 0);
    CHECK(normalizeAngle(-kPi / 2) == doctest::Approx(1.5 * kPi));
    CHECK(normalizeAngle(5 * kPi) == doctest::Approx(kPi));
    CHECK(normalizeAngle(-1e-20) < kTwoPi);
}

TEST_CASE("BBox2") {
    BBox2 b;
    CHECK(b.empty());
    CHECK(b.width() == 0);
    CHECK_FALSE(b.contains(Vec2{0, 0}));
    CHECK_FALSE(b.intersects(b));
    b.expand(Vec2{1, 2});
    CHECK_FALSE(b.empty());
    b.expand(Vec2{-3, 5});
    CHECK(b.min == Vec2{-3, 2});
    CHECK(b.max == Vec2{1, 5});
    CHECK(b.width() == 4);
    CHECK(b.height() == 3);
    CHECK(b.center() == Vec2{-1, 3.5});
    CHECK(b.contains(Vec2{0, 3}));
    CHECK_FALSE(b.contains(Vec2{2, 3}));

    BBox2 c;
    c.expand(Vec2{0, 0});
    c.expand(Vec2{10, 10});
    CHECK(b.intersects(c));
    CHECK(c.intersects(b));
    BBox2 far;
    far.expand(Vec2{20, 20});
    CHECK_FALSE(b.intersects(far));
    CHECK(c.containsBox(BBox2{Vec2{1, 1}, Vec2{2, 2}}));
    CHECK_FALSE(c.containsBox(b));

    BBox2 u = b;
    u.merge(far);
    CHECK(u.max == Vec2{20, 20});
    u.merge(BBox2{});  // empty is a no-op
    CHECK(u.max == Vec2{20, 20});
    CHECK(c.inflated(1).min == Vec2{-1, -1});
    CHECK(BBox2{}.inflated(1).empty());
}

TEST_CASE("Transform2 basics") {
    Vec2 p{1, 0};
    CHECK(near(Transform2::translate({2, 3}).apply(p), {3, 3}));
    CHECK(near(Transform2::translate({2, 3}).applyVector(p), {1, 0}));
    CHECK(near(Transform2::rotate(kPi / 2).apply(p), {0, 1}));
    CHECK(near(Transform2::rotate(kPi, {1, 1}).apply({2, 1}), {0, 1}));
    CHECK(near(Transform2::scale(2, 3).apply({1, 1}), {2, 3}));
    CHECK(near(Transform2::scale(2, 2, {1, 1}).apply({2, 2}), {3, 3}));
    CHECK(near(Transform2::scale(2, 2, {1, 1}).apply({1, 1}), {1, 1}));
    CHECK(Transform2::identity().determinant() == 1);
    CHECK(Transform2::scale(2, 3).determinant() == 6);
}

TEST_CASE("Transform2 mirror") {
    auto m = Transform2::mirror({0, 0}, {1, 0});  // across the X axis
    REQUIRE(m);
    CHECK(near(m.value().apply({3, 2}), {3, -2}));
    CHECK(m.value().determinant() == doctest::Approx(-1));

    auto d = Transform2::mirror({0, 1}, {1, 2});  // y = x + 1
    REQUIRE(d);
    CHECK(near(d.value().apply({0, 1}), {0, 1}));
    CHECK(near(d.value().apply({2, 0}), {-1, 3}));
    CHECK(near(d.value().apply(d.value().apply({5, -7})), {5, -7}));

    auto bad = Transform2::mirror({1, 1}, {1, 1});
    CHECK_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Transform2 compose applies the inner transform first") {
    auto t = Transform2::translate({10, 0});
    auto r = Transform2::rotate(kPi / 2);
    CHECK(near(compose(t, r).apply({1, 0}), {10, 1}));
    CHECK(near(compose(r, t).apply({1, 0}), {0, 11}));
    CHECK(near((t * r).apply({1, 0}), {10, 1}));
}

TEST_CASE("Transform2 inverse") {
    auto t = compose(Transform2::translate({5, -2}),
                     compose(Transform2::rotate(0.7), Transform2::scale(2, 3)));
    auto inv = t.inverse();
    REQUIRE(inv);
    Vec2 p{1.5, -4.25};
    CHECK(near(inv.value().apply(t.apply(p)), p, 1e-12));
    CHECK(near(t.apply(inv.value().apply(p)), p, 1e-12));
}

TEST_CASE("singular transforms return an error") {
    CHECK_FALSE(Transform2::scale(0, 1).inverse());
    CHECK_FALSE(Transform2::scale(1, 0).inverse());
    Transform2 flat;
    flat.m00 = 1;
    flat.m01 = 2;
    flat.m10 = 2;
    flat.m11 = 4;
    auto r = flat.inverse();
    CHECK_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    Transform2 zero;
    zero.m00 = zero.m11 = 0;
    CHECK_FALSE(zero.inverse());
    CHECK_FALSE(zero.isUniform());
}

TEST_CASE("Transform2 isUniform") {
    CHECK(Transform2::identity().isUniform());
    CHECK(Transform2::rotate(1.0).isUniform());
    CHECK(Transform2::scale(3, 3).isUniform());
    CHECK(compose(Transform2::rotate(0.3), Transform2::scale(2, 2)).isUniform());
    CHECK(Transform2::mirror({0, 0}, {1, 1}).value().isUniform());
    CHECK_FALSE(Transform2::scale(1, 2).isUniform());
    Transform2 shear;
    shear.m01 = 0.5;
    CHECK_FALSE(shear.isUniform());
}

TEST_CASE("Transform2 applied to a bounding box") {
    BBox2 b;
    b.expand({0, 0});
    b.expand({2, 1});
    BBox2 r = Transform2::rotate(kPi / 2).applyToBox(b);
    CHECK(r.min.x == doctest::Approx(-1));
    CHECK(r.max.x == doctest::Approx(0));
    CHECK(r.max.y == doctest::Approx(2));
    CHECK(Transform2::identity().applyToBox(BBox2{}).empty());
}

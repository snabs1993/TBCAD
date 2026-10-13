#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include "tbcad/core/result.h"

namespace tbcad {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

namespace tol {
inline constexpr double kLinear = 1e-6;    // mm
inline constexpr double kAngular = 1e-10;  // rad
}  // namespace tol

// Wraps an angle into [0, 2*pi).
inline double normalizeAngle(double a) {
    double r = std::fmod(a, kTwoPi);
    if (r < 0)
        r += kTwoPi;
    if (r >= kTwoPi)
        r = 0;
    return r;
}

struct Vec2 {
    double x = 0;
    double y = 0;

    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2& operator+=(const Vec2& o) {
        x += o.x;
        y += o.y;
        return *this;
    }
    constexpr Vec2& operator-=(const Vec2& o) {
        x -= o.x;
        y -= o.y;
        return *this;
    }
    constexpr Vec2& operator*=(double s) {
        x *= s;
        y *= s;
        return *this;
    }
    constexpr Vec2& operator/=(double s) {
        x /= s;
        y /= s;
        return *this;
    }
};

constexpr Vec2 operator+(Vec2 a, const Vec2& b) {
    return a += b;
}
constexpr Vec2 operator-(Vec2 a, const Vec2& b) {
    return a -= b;
}
constexpr Vec2 operator*(Vec2 a, double s) {
    return a *= s;
}
constexpr Vec2 operator*(double s, Vec2 a) {
    return a *= s;
}
constexpr Vec2 operator/(Vec2 a, double s) {
    return a /= s;
}

constexpr double dot(const Vec2& a, const Vec2& b) {
    return a.x * b.x + a.y * b.y;
}
constexpr double cross(const Vec2& a, const Vec2& b) {
    return a.x * b.y - a.y * b.x;
}
constexpr double lengthSquared(const Vec2& v) {
    return dot(v, v);
}
inline double length(const Vec2& v) {
    return std::hypot(v.x, v.y);
}
// Zero vector in, zero vector out.
inline Vec2 normalized(const Vec2& v) {
    double l = length(v);
    return l > 0 ? v / l : Vec2{};
}
// Rotated +90 degrees (counter-clockwise).
constexpr Vec2 perp(const Vec2& v) {
    return {-v.y, v.x};
}
constexpr Vec2 lerp(const Vec2& a, const Vec2& b, double t) {
    return a + (b - a) * t;
}
inline double distance(const Vec2& a, const Vec2& b) {
    return length(b - a);
}
inline bool isFinite(const Vec2& v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

namespace tol {
// abs(a-b) <= kLinear * max(1, |a|, |b|) / 1e3
inline bool nearlyEqual(double a, double b) {
    return std::abs(a - b) <= kLinear * std::max({1.0, std::abs(a), std::abs(b)}) / 1e3;
}
inline bool nearlyEqual(const Vec2& a, const Vec2& b) {
    return nearlyEqual(a.x, b.x) && nearlyEqual(a.y, b.y);
}
// Angles compared modulo 2*pi.
inline bool nearlyEqualAngle(double a, double b) {
    double d = std::abs(normalizeAngle(a) - normalizeAngle(b));
    return d <= kAngular || kTwoPi - d <= kAngular;
}
inline bool isZeroLength(double len) {
    return std::abs(len) <= kLinear;
}
inline bool isZeroLength(const Vec2& v) {
    return length(v) <= kLinear;
}
}  // namespace tol

struct BBox2 {
    Vec2 min{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    Vec2 max{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};

    bool empty() const { return min.x > max.x || min.y > max.y; }
    void expand(const Vec2& p) {
        min.x = std::min(min.x, p.x);
        min.y = std::min(min.y, p.y);
        max.x = std::max(max.x, p.x);
        max.y = std::max(max.y, p.y);
    }
    void merge(const BBox2& b) {
        if (b.empty())
            return;
        expand(b.min);
        expand(b.max);
    }
    bool intersects(const BBox2& o) const {
        return !empty() && !o.empty() && min.x <= o.max.x && o.min.x <= max.x && min.y <= o.max.y &&
               o.min.y <= max.y;
    }
    bool contains(const Vec2& p) const {
        return !empty() && p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }
    bool containsBox(const BBox2& o) const {
        return !empty() && !o.empty() && contains(o.min) && contains(o.max);
    }
    double width() const { return empty() ? 0 : max.x - min.x; }
    double height() const { return empty() ? 0 : max.y - min.y; }
    Vec2 center() const { return empty() ? Vec2{} : (min + max) * 0.5; }
    // Grown by d on every side (shrunk when negative; may become empty).
    BBox2 inflated(double d) const {
        if (empty())
            return *this;
        BBox2 r;
        r.min = {min.x - d, min.y - d};
        r.max = {max.x + d, max.y + d};
        return r;
    }
};

// 2x3 affine transform: p' = [m00 m01; m10 m11] * p + [tx; ty].
struct Transform2 {
    double m00 = 1, m01 = 0, tx = 0;
    double m10 = 0, m11 = 1, ty = 0;

    static Transform2 identity() { return {}; }
    static Transform2 translate(const Vec2& d);
    static Transform2 rotate(double angle, const Vec2& about = {});
    static Transform2 scale(double sx, double sy, const Vec2& about = {});
    // Reflection across the line through a and b. Error when a and b coincide.
    static Result<Transform2> mirror(const Vec2& lineA, const Vec2& lineB);

    Vec2 apply(const Vec2& p) const {
        return {m00 * p.x + m01 * p.y + tx, m10 * p.x + m11 * p.y + ty};
    }
    // Ignores the translation part.
    Vec2 applyVector(const Vec2& v) const { return {m00 * v.x + m01 * v.y, m10 * v.x + m11 * v.y}; }
    BBox2 applyToBox(const BBox2& b) const;

    double determinant() const { return m00 * m11 - m01 * m10; }
    // True for rotation + uniform scale + translation (a mirror counts as uniform).
    bool isUniform() const;
    // Error when the transform is singular.
    Result<Transform2> inverse() const;
};

// outer after inner: compose(a, b).apply(p) == a.apply(b.apply(p)).
Transform2 compose(const Transform2& outer, const Transform2& inner);
inline Transform2 operator*(const Transform2& outer, const Transform2& inner) {
    return compose(outer, inner);
}

}  // namespace tbcad

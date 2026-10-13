#include "tbcad/core/math.h"

namespace tbcad {

Transform2 Transform2::translate(const Vec2& d) {
    Transform2 t;
    t.tx = d.x;
    t.ty = d.y;
    return t;
}

Transform2 Transform2::rotate(double angle, const Vec2& about) {
    double c = std::cos(angle);
    double s = std::sin(angle);
    Transform2 t;
    t.m00 = c;
    t.m01 = -s;
    t.m10 = s;
    t.m11 = c;
    t.tx = about.x - (c * about.x - s * about.y);
    t.ty = about.y - (s * about.x + c * about.y);
    return t;
}

Transform2 Transform2::scale(double sx, double sy, const Vec2& about) {
    Transform2 t;
    t.m00 = sx;
    t.m11 = sy;
    t.tx = about.x - sx * about.x;
    t.ty = about.y - sy * about.y;
    return t;
}

Result<Transform2> Transform2::mirror(const Vec2& a, const Vec2& b) {
    Vec2 d = b - a;
    double len2 = lengthSquared(d);
    if (!(len2 > 0) || !std::isfinite(len2)) {
        return makeError(ErrorCode::InvalidArgument, "mirror line has zero length");
    }
    // Reflection matrix across direction d: 2*d*d^T/|d|^2 - I.
    double cc = (d.x * d.x - d.y * d.y) / len2;
    double ss = 2 * d.x * d.y / len2;
    Transform2 t;
    t.m00 = cc;
    t.m01 = ss;
    t.m10 = ss;
    t.m11 = -cc;
    // Keep point a fixed.
    t.tx = a.x - (t.m00 * a.x + t.m01 * a.y);
    t.ty = a.y - (t.m10 * a.x + t.m11 * a.y);
    return t;
}

BBox2 Transform2::applyToBox(const BBox2& b) const {
    BBox2 r;
    if (b.empty())
        return r;
    r.expand(apply(b.min));
    r.expand(apply(b.max));
    r.expand(apply(Vec2{b.min.x, b.max.y}));
    r.expand(apply(Vec2{b.max.x, b.min.y}));
    return r;
}

bool Transform2::isUniform() const {
    double l0 = std::hypot(m00, m10);
    double l1 = std::hypot(m01, m11);
    double d = m00 * m01 + m10 * m11;
    double scale = std::max(l0, l1);
    if (scale == 0)
        return false;
    return std::abs(l0 - l1) <= 1e-12 * scale && std::abs(d) <= 1e-12 * scale * scale;
}

Result<Transform2> Transform2::inverse() const {
    double det = determinant();
    double scale = std::max({std::abs(m00), std::abs(m01), std::abs(m10), std::abs(m11)});
    if (!(scale > 0) || !std::isfinite(det) || std::abs(det) <= 1e-12 * scale * scale) {
        return makeError(ErrorCode::InvalidArgument, "transform is singular");
    }
    Transform2 r;
    r.m00 = m11 / det;
    r.m01 = -m01 / det;
    r.m10 = -m10 / det;
    r.m11 = m00 / det;
    r.tx = -(r.m00 * tx + r.m01 * ty);
    r.ty = -(r.m10 * tx + r.m11 * ty);
    return r;
}

Transform2 compose(const Transform2& a, const Transform2& b) {
    Transform2 r;
    r.m00 = a.m00 * b.m00 + a.m01 * b.m10;
    r.m01 = a.m00 * b.m01 + a.m01 * b.m11;
    r.m10 = a.m10 * b.m00 + a.m11 * b.m10;
    r.m11 = a.m10 * b.m01 + a.m11 * b.m11;
    r.tx = a.m00 * b.tx + a.m01 * b.ty + a.tx;
    r.ty = a.m10 * b.tx + a.m11 * b.ty + a.ty;
    return r;
}

}  // namespace tbcad

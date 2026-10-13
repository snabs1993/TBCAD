#include "tbcad/blocks/array.h"

#include <cmath>

#include "tbcad/geom/geom.h"

namespace tbcad::blocks {

std::vector<Transform2> rectangularTransforms(const RectArray& s) {
    std::vector<Transform2> out;
    const Vec2 ex{std::cos(s.angle), std::sin(s.angle)};
    const Vec2 ey = perp(ex);
    for (int r = 0; r < s.rows; ++r)
        for (int c = 0; c < s.columns; ++c) {
            if (r == 0 && c == 0)
                continue;
            out.push_back(
                Transform2::translate(ex * (c * s.columnSpacing) + ey * (r * s.rowSpacing)));
        }
    return out;
}

std::vector<Transform2> polarTransforms(const PolarArray& s, Vec2 reference) {
    std::vector<Transform2> out;
    if (s.count < 2)
        return out;
    const bool full = std::abs(std::abs(s.fillAngle) - kTwoPi) < 1e-9;
    const double step = full ? s.fillAngle / s.count : s.fillAngle / (s.count - 1);
    for (int i = 1; i < s.count; ++i) {
        Transform2 rot = Transform2::rotate(step * i, s.center);
        out.push_back(s.rotateItems ? rot
                                    : Transform2::translate(rot.apply(reference) - reference));
    }
    return out;
}

namespace {

Vec2 sourceCenter(const std::vector<Entity>& source) {
    BBox2 box;
    for (const Entity& e : source)
        box.merge(geom::bounds(e));
    return box.center();
}

Result<void> checkCount(long long n) {
    if (n > kMaxArrayItems)
        return makeError(ErrorCode::OutOfRange, "That array would create more than " +
                                                    std::to_string(kMaxArrayItems) + " objects");
    return {};
}

std::vector<Entity> apply(const std::vector<Entity>& source, const std::vector<Transform2>& ts,
                          Result<void>& status) {
    std::vector<Entity> out;
    out.reserve(source.size() * ts.size());
    for (const Transform2& t : ts)
        for (const Entity& e : source) {
            auto r = geom::transformed(e, t);
            if (!r) {
                status = r.error();
                return {};
            }
            Entity copy = std::move(r).value();
            copy.common.id = {};
            out.push_back(std::move(copy));
        }
    return out;
}

}  // namespace

Result<std::vector<Entity>> arrayRectangular(const std::vector<Entity>& source,
                                             const RectArray& spec) {
    if (spec.rows < 1 || spec.columns < 1)
        return makeError(ErrorCode::InvalidArgument,
                         "An array needs at least one row and one column");
    if (auto c = checkCount(static_cast<long long>(spec.rows) * spec.columns * source.size());
        !c.ok())
        return c.error();
    Result<void> status{};
    auto out = apply(source, rectangularTransforms(spec), status);
    if (!status.ok())
        return status.error();
    return out;
}

Result<std::vector<Entity>> arrayPolar(const std::vector<Entity>& source, const PolarArray& spec) {
    if (spec.count < 1)
        return makeError(ErrorCode::InvalidArgument, "A polar array needs at least one item");
    if (!(std::abs(spec.fillAngle) > 0))
        return makeError(ErrorCode::InvalidArgument, "The angle to fill cannot be zero");
    if (auto c = checkCount(static_cast<long long>(spec.count) * source.size()); !c.ok())
        return c.error();
    Vec2 ref = spec.basePoint ? *spec.basePoint : sourceCenter(source);
    Result<void> status{};
    auto out = apply(source, polarTransforms(spec, ref), status);
    if (!status.ok())
        return status.error();
    return out;
}

}  // namespace tbcad::blocks

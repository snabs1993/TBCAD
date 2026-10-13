#include "tbcad/blocks/array.h"

#include <algorithm>
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

namespace {

// Position and direction of travel at arc length `s` along the path segments.
struct PathPoint {
    Vec2 p{};
    double angle = 0;
};

PathPoint pathPointAt(const std::vector<geom::Segment>& segs, const std::vector<double>& lens,
                      double s) {
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (s <= lens[i] || i + 1 == segs.size()) {
            const geom::Segment& g = segs[i];
            double len = g.curve.length();
            double u = len > 0 ? std::clamp(s / len, 0.0, 1.0) : 0.0;
            double t = g.toCurveParam(u);
            Vec2 dir = g.curve.tangentAt(t);
            if (g.reversed)
                dir = dir * -1.0;
            return {g.curve.pointAt(t), std::atan2(dir.y, dir.x)};
        }
        s -= lens[i];
    }
    return {};
}

}  // namespace

long long arrayItemCount(const ArrayInfo& a) {
    switch (a.kind) {
        case ArrayInfo::Kind::Rectangular:
            return static_cast<long long>(std::max(a.rows, 1)) * std::max(a.columns, 1);
        case ArrayInfo::Kind::Polar:
        case ArrayInfo::Kind::Path:
            return std::max(a.count, 1);
    }
    return 1;
}

std::vector<Transform2> arrayItemTransforms(const ArrayInfo& a) {
    std::vector<Transform2> out;
    long long n = std::min(arrayItemCount(a), kMaxArrayItems);
    out.reserve(static_cast<std::size_t>(n));
    switch (a.kind) {
        case ArrayInfo::Kind::Rectangular: {
            const Vec2 ex{std::cos(a.angle), std::sin(a.angle)};
            const Vec2 ey = perp(ex);
            for (int r = 0; r < std::max(a.rows, 1) && static_cast<long long>(out.size()) < n; ++r)
                for (int c = 0;
                     c < std::max(a.columns, 1) && static_cast<long long>(out.size()) < n; ++c)
                    out.push_back(Transform2::translate(ex * (c * a.columnSpacing) +
                                                        ey * (r * a.rowSpacing)));
            break;
        }
        case ArrayInfo::Kind::Polar: {
            const bool full = std::abs(std::abs(a.fillAngle) - kTwoPi) < 1e-9;
            const int cnt = std::max(a.count, 1);
            const double step = cnt < 2 ? 0 : (full ? a.fillAngle / cnt : a.fillAngle / (cnt - 1));
            for (long long i = 0; i < n; ++i) {
                Transform2 rot = Transform2::rotate(step * static_cast<double>(i), a.center);
                out.push_back(a.rotateItems
                                  ? rot
                                  : Transform2::translate(rot.apply(a.reference) - a.reference));
            }
            break;
        }
        case ArrayInfo::Kind::Path: {
            PolylineData pl;
            pl.v = a.path;
            pl.closed = a.pathClosed;
            auto segs = geom::segments(pl);
            std::vector<double> lens;
            double total = 0;
            for (const auto& g : segs) {
                lens.push_back(g.curve.length());
                total += lens.back();
            }
            if (segs.empty()) {
                out.assign(static_cast<std::size_t>(n), Transform2::identity());
                break;
            }
            const int cnt = std::max(a.count, 1);
            double spacing = a.pathSpacing;
            if (!(spacing > 0))
                spacing = a.pathClosed ? total / cnt : (cnt > 1 ? total / (cnt - 1) : 0);
            const PathPoint first = pathPointAt(segs, lens, 0);
            for (long long i = 0; i < n; ++i) {
                double s = spacing * static_cast<double>(i);
                if (a.pathClosed && total > 0)
                    s = std::fmod(s, total);
                else
                    s = std::min(s, total);
                PathPoint pp = pathPointAt(segs, lens, s);
                Transform2 t = Transform2::translate(pp.p - a.reference);
                if (a.rotateItems)
                    t = Transform2::rotate(pp.angle - first.angle, pp.p) *
                        Transform2::translate(pp.p - a.reference);
                out.push_back(t);
            }
            break;
        }
    }
    return out;
}

}  // namespace tbcad::blocks

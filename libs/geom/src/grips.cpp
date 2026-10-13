#include <algorithm>
#include <cmath>

#include "internal.h"
#include "tbcad/geom/entity.h"

namespace tbcad::geom {

namespace {

Error badGrip() {
    return makeError(ErrorCode::OutOfRange, "grip index out of range");
}

}  // namespace

std::vector<Vec2> gripPoints(const Entity& e) {
    std::vector<Vec2> g;
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            g = {d.a, d.b, (d.a + d.b) * 0.5};
            break;
        }
        case EntityKind::Circle: {
            const auto& d = *entityAs<CircleData>(e);
            g = {d.center, d.center + Vec2{d.radius, 0}, d.center + Vec2{0, d.radius},
                 d.center + Vec2{-d.radius, 0}, d.center + Vec2{0, -d.radius}};
            break;
        }
        case EntityKind::Arc: {
            const auto& d = *entityAs<ArcData>(e);
            CurveRef c = detail::arcCurve(d);
            g = {c.startPoint(), c.endPoint(), c.pointAt(0.5 * (c.t0() + c.t1())), d.center};
            break;
        }
        case EntityKind::Polyline: {
            const auto& pl = *entityAs<PolylineData>(e);
            for (const PolyVertex& v : pl.v)
                g.push_back(v.p);
            int n = static_cast<int>(pl.v.size());
            for (int i = 0; i < segmentCount(pl); ++i) {
                const PolyVertex& a = pl.v[static_cast<std::size_t>(i)];
                const PolyVertex& b = pl.v[static_cast<std::size_t>((i + 1) % n)];
                Vec2 m = (a.p + b.p) * 0.5;
                if (auto arc = bulgeArc(a.p, b.p, a.bulge)) {
                    Segment s;
                    s.curve = CurveRef::arc(arc->center, arc->radius, arc->startAngle, arc->sweep);
                    s.reversed = arc->reversed;
                    m = s.pointAtLocal(0.5);
                }
                g.push_back(m);
            }
            break;
        }
        case EntityKind::Point:
            g = {entityAs<PointData>(e)->p};
            break;
        case EntityKind::Ellipse: {
            const auto& d = *entityAs<EllipseData>(e);
            Vec2 minor = perp(d.majorAxis) * d.ratio;
            g = {d.center, d.center + d.majorAxis, d.center + minor, d.center - d.majorAxis,
                 d.center - minor};
            CurveRef c = detail::ellipseCurve(d);
            if (!c.isClosed()) {
                g.push_back(c.startPoint());
                g.push_back(c.endPoint());
            }
            break;
        }
        case EntityKind::Spline: {
            const auto& d = *entityAs<SplineData>(e);
            g = d.fit.empty() ? d.control : d.fit;
            break;
        }
        case EntityKind::Text:
            g = {entityAs<TextData>(e)->position};
            break;
        case EntityKind::MText:
            g = {entityAs<MTextData>(e)->position};
            break;
        case EntityKind::Insert:
            g = {entityAs<InsertData>(e)->position};
            break;
        case EntityKind::Dimension: {
            const auto& d = *entityAs<DimensionData>(e);
            g.assign(d.defPts.begin(), d.defPts.end());
            g.push_back(d.textPos);
            break;
        }
        case EntityKind::Leader: {
            const auto& d = *entityAs<LeaderData>(e);
            g = d.vertices;
            g.push_back(d.textPos);
            break;
        }
        case EntityKind::Ray:
            g = {entityAs<RayData>(e)->origin};
            break;
        case EntityKind::MLine:
            g = entityAs<MlineData>(e)->v;
            break;
        case EntityKind::Table: {
            const auto& d = *entityAs<TableData>(e);
            g.push_back(d.origin);
            double w = 0, h = 0;
            for (double x : d.colWidths)
                w += x;
            for (double y : d.rowHeights)
                h += y;
            const double c = std::cos(d.rotation), si = std::sin(d.rotation);
            // bottom-right corner in the rotated frame (y points down)
            g.push_back(d.origin + Vec2{w * c + h * si, w * si - h * c});
            break;
        }
        default:
            break;
    }
    return g;
}

Result<Entity> moveGrip(const Entity& e, int index, Vec2 to) {
    if (!isFinite(to))
        return makeError(ErrorCode::InvalidArgument, "grip target is not finite");
    Entity out = e;
    if (index < 0)
        return badGrip();
    switch (kind(e)) {
        case EntityKind::Line: {
            auto& d = *entityAs<LineData>(out);
            if (index == 0)
                d.a = to;
            else if (index == 1)
                d.b = to;
            else if (index == 2) {
                Vec2 delta = to - (d.a + d.b) * 0.5;
                d.a += delta;
                d.b += delta;
            } else
                return badGrip();
            return out;
        }
        case EntityKind::Circle: {
            auto& d = *entityAs<CircleData>(out);
            if (index == 0) {
                d.center = to;
            } else if (index <= 4) {
                double r = distance(d.center, to);
                if (tol::isZeroLength(r))
                    return makeError(ErrorCode::InvalidArgument, "radius would be zero");
                d.radius = r;
            } else
                return badGrip();
            return out;
        }
        case EntityKind::Arc: {
            const auto& d = *entityAs<ArcData>(e);
            CurveRef c = detail::arcCurve(d);
            Vec2 s = c.startPoint(), en = c.endPoint(), m = c.pointAt(0.5 * (c.t0() + c.t1()));
            Result<ArcData> r = badGrip();
            switch (index) {
                case 0:
                    r = arcFromThreePoints(to, m, en);
                    break;
                case 1:
                    r = arcFromThreePoints(s, m, to);
                    break;
                case 2:
                    r = arcFromThreePoints(s, to, en);
                    break;
                case 3: {
                    auto& o = *entityAs<ArcData>(out);
                    o.center = to;
                    return out;
                }
                default:
                    return badGrip();
            }
            if (!r)
                return r.error();
            *entityAs<ArcData>(out) = r.value();
            return out;
        }
        case EntityKind::Polyline: {
            auto& pl = *entityAs<PolylineData>(out);
            int n = static_cast<int>(pl.v.size());
            if (index < n) {
                pl.v[static_cast<std::size_t>(index)].p = to;
                return out;
            }
            int seg = index - n;
            if (seg >= segmentCount(pl))
                return badGrip();
            PolyVertex& a = pl.v[static_cast<std::size_t>(seg)];
            const PolyVertex& b = pl.v[static_cast<std::size_t>((seg + 1) % n)];
            auto bu = bulgeThrough(a.p, to, b.p);
            a.bulge = bu ? *bu : 0.0;
            return out;
        }
        case EntityKind::Point:
            if (index != 0)
                return badGrip();
            entityAs<PointData>(out)->p = to;
            return out;
        case EntityKind::Ellipse: {
            auto& d = *entityAs<EllipseData>(out);
            if (index == 0) {
                d.center = to;
                return out;
            }
            if (index >= 1 && index <= 4) {
                Vec2 v = to - d.center;
                if (index == 1 || index == 3) {
                    Vec2 major = index == 1 ? v : -v;
                    if (tol::isZeroLength(major))
                        return makeError(ErrorCode::InvalidArgument, "axis would be zero");
                    d.majorAxis = major;
                } else {
                    double minor = std::abs(dot(v, normalized(perp(d.majorAxis))));
                    double ratio = minor / length(d.majorAxis);
                    if (!(ratio > 1e-9))
                        return makeError(ErrorCode::InvalidArgument, "axis ratio would be zero");
                    // Keep the major axis the longer one.
                    if (ratio > 1)
                        return makeError(ErrorCode::InvalidArgument,
                                         "minor axis cannot exceed the major axis");
                    d.ratio = ratio;
                }
                return out;
            }
            if (index == 5 || index == 6) {
                CurveRef full = CurveRef::ellipse(d.center, d.majorAxis, d.ratio, 0, kTwoPi);
                if (!detail::ellipseCurve(*entityAs<EllipseData>(e)).isClosed()) {
                    double t = normalizeAngle(full.paramAt(to));
                    (index == 5 ? d.startParam : d.endParam) = t;
                    return out;
                }
            }
            return badGrip();
        }
        case EntityKind::Spline: {
            auto& d = *entityAs<SplineData>(out);
            auto& pts = d.fit.empty() ? d.control : d.fit;
            if (index >= static_cast<int>(pts.size()))
                return badGrip();
            pts[static_cast<std::size_t>(index)] = to;
            if (!d.fit.empty()) {
                InterpolateOptions o;
                o.degree = d.degree;
                o.closed = d.closed;
                auto r = interpolateSpline(d.fit, o);
                if (!r)
                    return r.error();
                d.knots = r.value().knots;
                d.control = r.value().control;
                d.weights.clear();
            }
            return out;
        }
        case EntityKind::Text: {
            if (index != 0)
                return badGrip();
            auto& d = *entityAs<TextData>(out);
            Vec2 delta = to - d.position;
            d.position = to;
            d.alignPoint += delta;
            return out;
        }
        case EntityKind::MText:
            if (index != 0)
                return badGrip();
            entityAs<MTextData>(out)->position = to;
            return out;
        case EntityKind::Insert:
            if (index != 0)
                return badGrip();
            entityAs<InsertData>(out)->position = to;
            return out;
        case EntityKind::Dimension: {
            auto& d = *entityAs<DimensionData>(out);
            if (index < 4) {
                d.defPts[static_cast<std::size_t>(index)] = to;
            } else if (index == 4) {
                d.textPos = to;
                d.textPosUser = true;
            } else
                return badGrip();
            return out;
        }
        case EntityKind::Leader: {
            auto& d = *entityAs<LeaderData>(out);
            int n = static_cast<int>(d.vertices.size());
            if (index < n)
                d.vertices[static_cast<std::size_t>(index)] = to;
            else if (index == n)
                d.textPos = to;
            else
                return badGrip();
            return out;
        }
        case EntityKind::Ray: {
            if (index != 0)
                return badGrip();
            entityAs<RayData>(out)->origin = to;
            return out;
        }
        case EntityKind::MLine: {
            auto& d = *entityAs<MlineData>(out);
            if (index < 0 || static_cast<std::size_t>(index) >= d.v.size())
                return badGrip();
            d.v[static_cast<std::size_t>(index)] = to;
            return out;
        }
        case EntityKind::Table: {
            auto& d = *entityAs<TableData>(out);
            if (index == 0) {
                d.origin = to;
                return out;
            }
            if (index != 1)
                return badGrip();
            // The corner drags the whole grid: columns and rows keep their proportions.
            double w = 0, h = 0;
            for (double x : d.colWidths)
                w += x;
            for (double y : d.rowHeights)
                h += y;
            const double c = std::cos(d.rotation), si = std::sin(d.rotation);
            const Vec2 v = to - d.origin;
            const double nw = v.x * c + v.y * si;
            const double nh = v.x * si - v.y * c;
            if (!(nw > 1e-9) || !(nh > 1e-9) || !(w > 0) || !(h > 0))
                return makeError(ErrorCode::InvalidArgument, "table would collapse");
            for (double& x : d.colWidths)
                x *= nw / w;
            for (double& y : d.rowHeights)
                y *= nh / h;
            return out;
        }
        default:
            return makeError(ErrorCode::Unsupported, "entity has no grips");
    }
}

}  // namespace tbcad::geom

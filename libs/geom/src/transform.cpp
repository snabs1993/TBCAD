#include <algorithm>
#include <cmath>

#include "internal.h"
#include "tbcad/geom/entity.h"

namespace tbcad::geom {

namespace {

using detail::arcSweep;

struct Canon {
    Vec2 center{};
    Vec2 major{};
    double ratio = 1;
    double start = 0, sweep = kTwoPi;
};

// p(t) = c + u cos t + v sin t for t in [t0, t0 + sweep]  ->  canonical ellipse.
Result<Canon> canonicalEllipse(Vec2 c, Vec2 u, Vec2 v, double t0, double sweep) {
    double theta = 0.5 * std::atan2(2.0 * dot(u, v), lengthSquared(u) - lengthSquared(v));
    Vec2 m = u * std::cos(theta) + v * std::sin(theta);
    Vec2 n = u * -std::sin(theta) + v * std::cos(theta);
    double th = theta;
    if (lengthSquared(n) > lengthSquared(m)) {
        Vec2 nm = n;
        n = -m;
        m = nm;
        th += kPi / 2;
    }
    double lm = length(m), ln = length(n);
    if (!(lm > 0) || ln < 1e-9 * lm)
        return makeError(ErrorCode::InvalidArgument, "transform flattens the curve to a line");
    Canon out;
    out.center = c;
    out.major = m;
    out.ratio = ln / lm;
    out.sweep = sweep;
    if (cross(m, n) > 0) {
        out.start = t0 - th;
    } else {
        out.start = th - t0 - sweep;
    }
    out.start = normalizeAngle(out.start);
    return out;
}

EllipseData toEllipseData(const Canon& c) {
    EllipseData e;
    e.center = c.center;
    e.majorAxis = c.major;
    e.ratio = c.ratio;
    if (c.sweep >= kTwoPi - 1e-12) {
        e.startParam = 0;
        e.endParam = kTwoPi;
    } else {
        e.startParam = c.start;
        e.endParam = normalizeAngle(c.start + c.sweep);
    }
    return e;
}

double rotationOf(const Transform2& t, double angle) {
    Vec2 d = t.applyVector({std::cos(angle), std::sin(angle)});
    return normalizeAngle(std::atan2(d.y, d.x));
}

double areaScale(const Transform2& t) {
    return std::sqrt(std::abs(t.determinant()));
}

Result<Entity> transformSpline(const Entity& e, const Transform2& t) {
    Entity out = e;
    auto* s = entityAs<SplineData>(out);
    for (Vec2& p : s->control)
        p = t.apply(p);
    for (Vec2& p : s->fit)
        p = t.apply(p);
    return out;
}

// Arc described by centre/radius/start/sweep under an arbitrary transform.
struct ArcGeometry {
    bool isEllipse = false;
    Vec2 center{};
    double radius = 0;
    Vec2 major{};
    double ratio = 1;
    double start = 0, sweep = 0;
};

Result<ArcGeometry> transformArcGeometry(Vec2 c, double r, double start, double sweep,
                                         const Transform2& t) {
    ArcGeometry g;
    if (t.isUniform()) {
        g.center = t.apply(c);
        g.radius = r * areaScale(t);
        double a0 = rotationOf(t, start), a1 = rotationOf(t, start + sweep);
        if (t.determinant() < 0)
            std::swap(a0, a1);
        g.start = a0;
        g.sweep = sweep >= kTwoPi - 1e-12 ? kTwoPi : normalizeAngle(a1 - a0);
        if (sweep < kTwoPi - 1e-12 && g.sweep < 1e-12)
            g.sweep = kTwoPi;
        return g;
    }
    Vec2 u = t.applyVector({r, 0}), v = t.applyVector({0, r});
    auto ce = canonicalEllipse(t.apply(c), u, v, start, sweep);
    if (!ce)
        return ce.error();
    g.isEllipse = true;
    g.center = ce.value().center;
    g.major = ce.value().major;
    g.ratio = ce.value().ratio;
    g.start = ce.value().start;
    g.sweep = ce.value().sweep;
    return g;
}

Result<Canon> transformEllipse(const EllipseData& d, const Transform2& t) {
    Vec2 u = t.applyVector(d.majorAxis);
    Vec2 v = t.applyVector(perp(d.majorAxis) * d.ratio);
    double sw = detail::ellipseSweep(d);
    return canonicalEllipse(t.apply(d.center), u, v, d.startParam, sw);
}

Result<HatchEdge> transformHatchEdge(const HatchEdge& edge, const Transform2& t) {
    bool mirror = t.determinant() < 0;
    if (const auto* l = std::get_if<HatchLineEdge>(&edge))
        return HatchEdge{HatchLineEdge{t.apply(l->a), t.apply(l->b)}};
    if (const auto* a = std::get_if<HatchArcEdge>(&edge)) {
        // Geometric counter-clockwise range.
        double g0 = a->ccw ? a->startAngle : a->endAngle;
        double g1 = a->ccw ? a->endAngle : a->startAngle;
        double sw = normalizeAngle(g1 - g0);
        if (sw < 1e-12)
            sw = kTwoPi;
        Vec2 first = a->center + Vec2{std::cos(a->startAngle), std::sin(a->startAngle)} * a->radius;
        auto g = transformArcGeometry(a->center, a->radius, g0, sw, t);
        if (!g)
            return g.error();
        Vec2 firstT = t.apply(first);
        if (!g.value().isEllipse) {
            HatchArcEdge out;
            out.center = g.value().center;
            out.radius = g.value().radius;
            Vec2 p0 = g.value().center +
                      Vec2{std::cos(g.value().start), std::sin(g.value().start)} * g.value().radius;
            bool startsAtFirst =
                distance(p0, firstT) <=
                distance(g.value().center + Vec2{std::cos(g.value().start + g.value().sweep),
                                                 std::sin(g.value().start + g.value().sweep)} *
                                                g.value().radius,
                         firstT);
            out.ccw = startsAtFirst;
            double e = normalizeAngle(g.value().start + g.value().sweep);
            out.startAngle = startsAtFirst ? g.value().start : e;
            out.endAngle = startsAtFirst ? e : g.value().start;
            return HatchEdge{out};
        }
        HatchEllipseEdge out;
        out.center = g.value().center;
        out.majorAxis = g.value().major;
        out.ratio = g.value().ratio;
        CurveRef c = CurveRef::ellipse(g.value().center, g.value().major, g.value().ratio,
                                       g.value().start, g.value().sweep);
        bool startsAtFirst = distance(c.startPoint(), firstT) <= distance(c.endPoint(), firstT);
        out.ccw = startsAtFirst;
        double e = g.value().sweep >= kTwoPi - 1e-12
                       ? kTwoPi
                       : normalizeAngle(g.value().start + g.value().sweep);
        double s0 = g.value().sweep >= kTwoPi - 1e-12 ? 0.0 : g.value().start;
        out.startParam = startsAtFirst ? s0 : e;
        out.endParam = startsAtFirst ? e : s0;
        return HatchEdge{out};
    }
    if (const auto* el = std::get_if<HatchEllipseEdge>(&edge)) {
        double g0 = el->ccw ? el->startParam : el->endParam;
        double g1 = el->ccw ? el->endParam : el->startParam;
        double sw = (g1 - g0 >= kTwoPi - 1e-12) ? kTwoPi : normalizeAngle(g1 - g0);
        if (sw < 1e-12)
            sw = kTwoPi;
        EllipseData d;
        d.center = el->center;
        d.majorAxis = el->majorAxis;
        d.ratio = el->ratio;
        d.startParam = g0;
        d.endParam = g0 + sw;
        auto ce = transformEllipse(d, t);
        if (!ce)
            return ce.error();
        CurveRef orig = CurveRef::ellipse(el->center, el->majorAxis, el->ratio, el->startParam, sw);
        Vec2 firstT = t.apply(CurveRef::ellipse(el->center, el->majorAxis, el->ratio, 0, kTwoPi)
                                  .pointAt(el->startParam));
        (void)orig;
        CurveRef c = CurveRef::ellipse(ce.value().center, ce.value().major, ce.value().ratio,
                                       ce.value().start, ce.value().sweep);
        HatchEllipseEdge out;
        out.center = ce.value().center;
        out.majorAxis = ce.value().major;
        out.ratio = ce.value().ratio;
        bool startsAtFirst = distance(c.startPoint(), firstT) <= distance(c.endPoint(), firstT);
        out.ccw = startsAtFirst;
        double e = ce.value().sweep >= kTwoPi - 1e-12
                       ? kTwoPi
                       : normalizeAngle(ce.value().start + ce.value().sweep);
        double s0 = ce.value().sweep >= kTwoPi - 1e-12 ? 0.0 : ce.value().start;
        out.startParam = startsAtFirst ? s0 : e;
        out.endParam = startsAtFirst ? e : s0;
        return HatchEdge{out};
    }
    if (const auto* sp = std::get_if<HatchSplineEdge>(&edge)) {
        HatchSplineEdge out = *sp;
        for (Vec2& p : out.spline.control)
            p = t.apply(p);
        for (Vec2& p : out.spline.fit)
            p = t.apply(p);
        return HatchEdge{std::move(out)};
    }
    const auto& pe = std::get<HatchPolylineEdge>(edge);
    HatchPolylineEdge out = pe;
    for (PolyVertex& v : out.v) {
        if (v.bulge != 0 && !t.isUniform())
            return makeError(ErrorCode::Unsupported,
                             "cannot scale a bulged polyline edge non-uniformly");
        v.p = t.apply(v.p);
        if (mirror)
            v.bulge = -v.bulge;
    }
    return HatchEdge{std::move(out)};
}

}  // namespace

Result<Entity> transformed(const Entity& e, const Transform2& t) {
    double det = t.determinant();
    if (!std::isfinite(det) || std::abs(det) < 1e-18)
        return makeError(ErrorCode::InvalidArgument, "transform is singular");
    bool uniform = t.isUniform();
    bool mirror = det < 0;
    double s = areaScale(t);
    Entity out = e;
    switch (kind(e)) {
        case EntityKind::Line: {
            auto& d = *entityAs<LineData>(out);
            d.a = t.apply(d.a);
            d.b = t.apply(d.b);
            return out;
        }
        case EntityKind::Point: {
            auto& d = *entityAs<PointData>(out);
            d.p = t.apply(d.p);
            return out;
        }
        case EntityKind::Circle: {
            const auto& d = *entityAs<CircleData>(e);
            if (uniform) {
                auto& o = *entityAs<CircleData>(out);
                o.center = t.apply(d.center);
                o.radius = d.radius * s;
                return out;
            }
            auto ce = canonicalEllipse(t.apply(d.center), t.applyVector({d.radius, 0}),
                                       t.applyVector({0, d.radius}), 0, kTwoPi);
            if (!ce)
                return ce.error();
            out.data = Indirect<EllipseData>(toEllipseData(ce.value()));
            return out;
        }
        case EntityKind::Arc: {
            const auto& d = *entityAs<ArcData>(e);
            auto g = transformArcGeometry(d.center, d.radius, d.startAngle, arcSweep(d), t);
            if (!g)
                return g.error();
            if (!g.value().isEllipse) {
                auto& o = *entityAs<ArcData>(out);
                o.center = g.value().center;
                o.radius = g.value().radius;
                o.startAngle = normalizeAngle(g.value().start);
                o.endAngle = normalizeAngle(g.value().start + g.value().sweep);
                return out;
            }
            Canon c;
            c.center = g.value().center;
            c.major = g.value().major;
            c.ratio = g.value().ratio;
            c.start = g.value().start;
            c.sweep = g.value().sweep;
            out.data = Indirect<EllipseData>(toEllipseData(c));
            return out;
        }
        case EntityKind::Ellipse: {
            auto ce = transformEllipse(*entityAs<EllipseData>(e), t);
            if (!ce)
                return ce.error();
            *entityAs<EllipseData>(out) = toEllipseData(ce.value());
            return out;
        }
        case EntityKind::Polyline: {
            auto& o = *entityAs<PolylineData>(out);
            for (PolyVertex& v : o.v) {
                if (v.bulge != 0 && !uniform)
                    return makeError(ErrorCode::Unsupported,
                                     "cannot scale a polyline with arc segments non-uniformly "
                                     "(explode it first)");
                v.p = t.apply(v.p);
                if (mirror)
                    v.bulge = -v.bulge;
                v.startWidth *= s;
                v.endWidth *= s;
            }
            o.constantWidth *= s;
            return out;
        }
        case EntityKind::Spline:
            return transformSpline(e, t);
        case EntityKind::Text: {
            if (!uniform)
                return makeError(ErrorCode::Unsupported, "text cannot be scaled non-uniformly");
            auto& o = *entityAs<TextData>(out);
            o.position = t.apply(o.position);
            o.alignPoint = t.apply(o.alignPoint);
            o.height *= s;
            if (!mirror)
                o.rotation = rotationOf(t, o.rotation);
            return out;
        }
        case EntityKind::MText: {
            if (!uniform)
                return makeError(ErrorCode::Unsupported, "text cannot be scaled non-uniformly");
            auto& o = *entityAs<MTextData>(out);
            o.position = t.apply(o.position);
            o.height *= s;
            o.width *= s;
            if (!mirror)
                o.rotation = rotationOf(t, o.rotation);
            return out;
        }
        case EntityKind::Insert: {
            auto& o = *entityAs<InsertData>(out);
            // Linear part of the instance transform after applying t.
            double c = std::cos(o.rotation), sn = std::sin(o.rotation);
            Vec2 c0 = t.applyVector({c * o.scale.x, sn * o.scale.x});
            Vec2 c1 = t.applyVector({-sn * o.scale.y, c * o.scale.y});
            double l0 = length(c0), l1 = length(c1);
            if (!(l0 > 0) || !(l1 > 0) || std::abs(dot(c0, c1)) > 1e-9 * l0 * l1)
                return makeError(ErrorCode::Unsupported, "transform skews the block instance");
            o.position = t.apply(o.position);
            o.rotation = normalizeAngle(std::atan2(c0.y, c0.x));
            o.scale.x = l0;
            o.scale.y = cross(c0, c1) / l0;
            return out;
        }
        case EntityKind::Hatch: {
            auto& o = *entityAs<HatchData>(out);
            for (HatchLoop& loop : o.loops)
                for (HatchEdge& edge : loop.edges) {
                    auto r = transformHatchEdge(edge, t);
                    if (!r)
                        return r.error();
                    edge = std::move(r).value();
                }
            o.patternOrigin = t.apply(o.patternOrigin);
            o.patternAngle = rotationOf(t, o.patternAngle);
            o.patternScale *= s;
            return out;
        }
        case EntityKind::Dimension: {
            if (!uniform)
                return makeError(ErrorCode::Unsupported,
                                 "dimensions cannot be scaled non-uniformly");
            auto& o = *entityAs<DimensionData>(out);
            for (Vec2& p : o.defPts)
                p = t.apply(p);
            o.textPos = t.apply(o.textPos);
            if (o.kind != DimKind::Angular2Line && o.kind != DimKind::Angular3Point)
                o.measurement *= s;
            if (o.kind == DimKind::Linear)
                o.rotation = rotationOf(t, o.rotation);
            return out;
        }
        case EntityKind::Leader: {
            if (!uniform)
                return makeError(ErrorCode::Unsupported, "leaders cannot be scaled non-uniformly");
            auto& o = *entityAs<LeaderData>(out);
            for (Vec2& p : o.vertices)
                p = t.apply(p);
            o.textPos = t.apply(o.textPos);
            return out;
        }
        case EntityKind::Ray: {
            auto& o = *entityAs<RayData>(out);
            const Vec2 p0 = t.apply(o.origin);
            const Vec2 p1 = t.apply(o.origin + o.dir);
            o.origin = p0;
            const Vec2 d = p1 - p0;
            const double len = std::hypot(d.x, d.y);
            if (!(len > 1e-18))
                return makeError(ErrorCode::InvalidArgument, "transform collapses the ray");
            o.dir = {d.x / len, d.y / len};
            return out;
        }
        case EntityKind::MLine: {
            if (!uniform)
                return makeError(ErrorCode::Unsupported,
                                 "multilines cannot be scaled non-uniformly");
            auto& o = *entityAs<MlineData>(out);
            for (Vec2& p : o.v)
                p = t.apply(p);
            o.scale *= s;
            if (mirror)
                o.justification = 2 - o.justification;
            return out;
        }
        case EntityKind::Table: {
            if (!uniform)
                return makeError(ErrorCode::Unsupported, "tables cannot be scaled non-uniformly");
            if (mirror)
                return makeError(ErrorCode::Unsupported, "tables cannot be mirrored");
            auto& o = *entityAs<TableData>(out);
            o.origin = t.apply(o.origin);
            o.rotation = rotationOf(t, o.rotation);
            for (double& w : o.colWidths)
                w *= s;
            for (double& h : o.rowHeights)
                h *= s;
            o.textHeight *= s;
            o.margin *= s;
            return out;
        }
        case EntityKind::Proxy: {
            auto& o = *entityAs<ProxyData>(out);
            for (DisplayItem& it : o.graphics.items) {
                std::visit(
                    [&](auto& item) {
                        using T = std::decay_t<decltype(item)>;
                        if constexpr (std::is_same_v<T, DisplayFill>) {
                            for (auto& r : item.rings)
                                for (Vec2& p : r)
                                    p = t.apply(p);
                        } else {
                            for (Vec2& p : item.pts)
                                p = t.apply(p);
                        }
                    },
                    it);
            }
            return out;
        }
    }
    return makeError(ErrorCode::Unsupported, "unknown entity kind");
}

}  // namespace tbcad::geom

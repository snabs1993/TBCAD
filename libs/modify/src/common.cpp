#include "tbcad/modify/common.h"

#include <algorithm>
#include <cmath>

namespace tbcad::modify {

using geom::CurveRef;

std::optional<ArcGeom> arcGeom(const Seg& s) {
    if (!s.isArc())
        return std::nullopt;
    Vec2 chord = s.p1 - s.p0;
    double len = length(chord);
    if (!(len > 0))
        return std::nullopt;
    ArcGeom g;
    double ab = std::abs(s.bulge);
    g.radius = len * 0.5 * (1.0 + s.bulge * s.bulge) / (2.0 * ab);
    double d = len * 0.5 * (1.0 - s.bulge * s.bulge) / (2.0 * s.bulge);
    g.center = (s.p0 + s.p1) * 0.5 + perp(chord / len) * d;
    g.theta = 4.0 * std::atan(s.bulge);
    g.startAngle = std::atan2(s.p0.y - g.center.y, s.p0.x - g.center.x);
    return g;
}

Vec2 segPoint(const Seg& s, double u) {
    auto g = arcGeom(s);
    if (!g)
        return lerp(s.p0, s.p1, u);
    if (u == 0)
        return s.p0;
    if (u == 1)
        return s.p1;
    double a = g->startAngle + u * g->theta;
    return g->center + Vec2{std::cos(a), std::sin(a)} * g->radius;
}

Vec2 segTangent(const Seg& s, double u) {
    auto g = arcGeom(s);
    if (!g)
        return normalized(s.p1 - s.p0);
    double a = g->startAngle + u * g->theta;
    Vec2 t{-std::sin(a), std::cos(a)};
    return g->theta > 0 ? t : -t;
}

double segLength(const Seg& s) {
    auto g = arcGeom(s);
    return g ? g->radius * std::abs(g->theta) : distance(s.p0, s.p1);
}

namespace {
// Angular distance travelled from the start in the direction of travel, in [0, 2*pi).
double travelOffset(const ArcGeom& g, Vec2 p) {
    double phi = std::atan2(p.y - g.center.y, p.x - g.center.x);
    return g.theta > 0 ? normalizeAngle(phi - g.startAngle) : normalizeAngle(g.startAngle - phi);
}
}  // namespace

double segParam(const Seg& s, Vec2 p) {
    auto g = arcGeom(s);
    if (!g) {
        Vec2 d = s.p1 - s.p0;
        double dd = lengthSquared(d);
        return dd > 0 ? std::clamp(dot(p - s.p0, d) / dd, 0.0, 1.0) : 0.0;
    }
    double sw = std::abs(g->theta);
    double off = travelOffset(*g, p);
    if (off <= sw)
        return off / sw;
    return (off - sw < kTwoPi - off) ? 1.0 : 0.0;
}

double segParamUnbounded(const Seg& s, Vec2 p) {
    auto g = arcGeom(s);
    if (!g) {
        Vec2 d = s.p1 - s.p0;
        double dd = lengthSquared(d);
        return dd > 0 ? dot(p - s.p0, d) / dd : 0.0;
    }
    double sw = std::abs(g->theta);
    double off = travelOffset(*g, p);
    if (off <= sw)
        return off / sw;
    if (kTwoPi - off < off - sw)
        return -(kTwoPi - off) / sw;
    return 1.0 + (off - sw) / sw;
}

Seg segSub(const Seg& s, double u0, double u1) {
    Seg r;
    r.p0 = u0 <= 0 ? s.p0 : segPoint(s, u0);
    r.p1 = u1 >= 1 ? s.p1 : segPoint(s, u1);
    if (s.isArc()) {
        double th = 4.0 * std::atan(s.bulge);
        r.bulge = std::tan((std::min(u1, 1.0) - std::max(u0, 0.0)) * th / 4.0);
    }
    return r;
}

Seg segWithEnd(const Seg& s, Vec2 p) {
    Seg r{s.p0, p, 0};
    if (s.isArc()) {
        double th = 4.0 * std::atan(s.bulge);
        double u = segParamUnbounded(s, p);
        double sweep = std::clamp(u * th, -(kTwoPi - 1e-6), kTwoPi - 1e-6);
        r.bulge = std::tan(sweep / 4.0);
    }
    return r;
}

Seg segWithStart(const Seg& s, Vec2 p) {
    Seg r{p, s.p1, 0};
    if (s.isArc()) {
        double th = 4.0 * std::atan(s.bulge);
        double u = segParamUnbounded(s, p);
        double sweep = std::clamp((1.0 - u) * th, -(kTwoPi - 1e-6), kTwoPi - 1e-6);
        r.bulge = std::tan(sweep / 4.0);
    }
    return r;
}

Seg segReversed(const Seg& s) {
    return {s.p1, s.p0, -s.bulge};
}

CurveRef segCurve(const Seg& s) {
    auto g = arcGeom(s);
    if (!g)
        return CurveRef::line(s.p0, s.p1);
    if (g->theta > 0)
        return CurveRef::arc(g->center, g->radius, normalizeAngle(g->startAngle), g->theta);
    return CurveRef::arc(g->center, g->radius, normalizeAngle(g->startAngle + g->theta),
                         -g->theta);
}

PolylineData cleanPolyline(const PolylineData& pl) {
    PolylineData out;
    out.closed = pl.closed;
    out.constantWidth = pl.constantWidth;
    for (const PolyVertex& v : pl.v) {
        if (!out.v.empty() && samePoint(out.v.back().p, v.p)) {
            out.v.back().bulge = v.bulge;
            out.v.back().endWidth = v.endWidth;
            continue;
        }
        out.v.push_back(v);
    }
    if (out.closed && out.v.size() > 1 && samePoint(out.v.back().p, out.v.front().p))
        out.v.pop_back();
    return out;
}

std::vector<Seg> polylineSegs(const PolylineData& pl) {
    std::vector<Seg> out;
    std::size_t n = pl.v.size();
    int count = geom::segmentCount(pl);
    for (int i = 0; i < count; ++i) {
        const PolyVertex& a = pl.v[static_cast<std::size_t>(i)];
        const PolyVertex& b = pl.v[(static_cast<std::size_t>(i) + 1) % n];
        out.push_back({a.p, b.p, a.bulge});
    }
    return out;
}

PolylineData polylineFromSegs(const std::vector<Seg>& chain, bool closed, const PolylineData* like) {
    PolylineData pl;
    if (like)
        pl.constantWidth = like->constantWidth;
    pl.closed = closed;
    for (const Seg& s : chain) {
        PolyVertex v;
        v.p = s.p0;
        v.bulge = s.isArc() ? s.bulge : 0.0;
        pl.v.push_back(v);
    }
    if (!closed && !chain.empty()) {
        PolyVertex v;
        v.p = chain.back().p1;
        pl.v.push_back(v);
    }
    return pl;
}

std::vector<Seg> sliceSegs(const std::vector<Seg>& segs, double s0, double s1) {
    std::vector<Seg> out;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        double fi = static_cast<double>(i);
        double lo = std::max(s0, fi), hi = std::min(s1, fi + 1.0);
        if (hi - lo <= 1e-12)
            continue;
        Seg s = segSub(segs[i], lo - fi, hi - fi);
        if (segLength(s) <= tol::kLinear)
            continue;
        out.push_back(s);
    }
    return out;
}

std::vector<Seg> sliceCyclic(const std::vector<Seg>& segs, double s0, double s1) {
    if (s1 > s0)
        return sliceSegs(segs, s0, s1);
    auto a = sliceSegs(segs, s0, static_cast<double>(segs.size()));
    auto b = sliceSegs(segs, 0, s1);
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

double closestGlobalParam(const std::vector<Seg>& segs, Vec2 p) {
    double best = 0, bestD = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < segs.size(); ++i) {
        double u = segParam(segs[i], p);
        double d = distance(segPoint(segs[i], u), p);
        if (d < bestD) {
            bestD = d;
            best = static_cast<double>(i) + u;
        }
    }
    return best;
}

Entity withData(const Entity& like, EntityData data) {
    Entity e;
    e.common = like.common;
    e.common.id = {};
    e.data = std::move(data);
    return e;
}

std::optional<EntityData> dataFromCurve(const CurveRef& c) {
    switch (c.kind()) {
        case CurveRef::Kind::Line:
            return EntityData{LineData{c.lineA(), c.lineB()}};
        case CurveRef::Kind::Arc: {
            if (c.sweep() >= kTwoPi - 1e-9)
                return EntityData{CircleData{c.center(), c.radius()}};
            ArcData a;
            a.center = c.center();
            a.radius = c.radius();
            a.startAngle = normalizeAngle(c.startAngle());
            a.endAngle = normalizeAngle(c.startAngle() + c.sweep());
            return EntityData{a};
        }
        case CurveRef::Kind::Ellipse: {
            EllipseData e;
            e.center = c.center();
            e.majorAxis = c.majorAxis();
            e.ratio = c.ratio();
            if (c.sweep() >= kTwoPi - 1e-9) {
                e.startParam = 0;
                e.endParam = kTwoPi;
            } else {
                e.startParam = normalizeAngle(c.startAngle());
                e.endParam = normalizeAngle(c.startAngle() + c.sweep());
            }
            return EntityData{Indirect<EllipseData>(e)};
        }
        case CurveRef::Kind::Spline:
            return std::nullopt;
    }
    return std::nullopt;
}

bool isCurveEntity(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
            return true;
        default:
            return false;
    }
}

}  // namespace tbcad::modify

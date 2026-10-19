#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "internal.h"
#include "tbcad/geom/entity.h"
#include "tbcad/geom/box_grid.h"

namespace tbcad::geom {

namespace detail {

double arcSweep(const ArcData& a) {
    double s = normalizeAngle(a.endAngle - a.startAngle);
    return s < 1e-12 ? kTwoPi : s;
}

double ellipseSweep(const EllipseData& e) {
    double raw = e.endParam - e.startParam;
    if (raw >= kTwoPi - 1e-12)
        return kTwoPi;
    double s = normalizeAngle(raw);
    return s < 1e-12 ? kTwoPi : s;
}

namespace {
struct CacheEntry {
    SplineData data{};
    std::shared_ptr<const SplineCurve> curve{};
};

bool sameData(const SplineData& a, const SplineData& b) {
    auto eq = [](const auto& x, const auto& y) {
        return x.size() == y.size() &&
               (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    return a.degree == b.degree && a.closed == b.closed && eq(a.knots, b.knots) &&
           eq(a.control, b.control) && eq(a.weights, b.weights) && eq(a.fit, b.fit);
}
}  // namespace

std::shared_ptr<const SplineCurve> cachedSpline(const SplineData& data) {
    thread_local std::vector<CacheEntry> cache;
    for (std::size_t i = 0; i < cache.size(); ++i) {
        if (sameData(cache[i].data, data)) {
            if (i != 0)
                std::swap(cache[i], cache[0]);
            return cache[0].curve;
        }
    }
    auto r = SplineCurve::make(data);
    if (!r)
        return nullptr;
    constexpr std::size_t kCap = 16;
    if (cache.size() >= kCap)
        cache.pop_back();
    cache.insert(cache.begin(), CacheEntry{data, r.value()});
    return cache[0].curve;
}

}  // namespace detail

namespace {

using detail::arcCurve;
using detail::ellipseCurve;

inline double dist2(Vec2 a, Vec2 b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

inline Vec2 closestOnSegment(Vec2 a, Vec2 b, Vec2 p) {
    Vec2 d = b - a;
    double dd = d.x * d.x + d.y * d.y;
    if (dd <= 0)
        return a;
    double t = ((p.x - a.x) * d.x + (p.y - a.y) * d.y) / dd;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return {a.x + d.x * t, a.y + d.y * t};
}

inline Vec2 closestOnArc(Vec2 c, double r, double start, double sweep, Vec2 p) {
    double vx = p.x - c.x, vy = p.y - c.y;
    double ang = (vx == 0 && vy == 0) ? start : std::atan2(vy, vx);
    double off = normalizeAngle(ang - start);
    if (sweep >= kTwoPi - 1e-12 || off <= sweep) {
        double len = std::sqrt(vx * vx + vy * vy);
        if (len <= 0)
            return {c.x + r * std::cos(ang), c.y + r * std::sin(ang)};
        return {c.x + vx / len * r, c.y + vy / len * r};
    }
    Vec2 s{c.x + r * std::cos(start), c.y + r * std::sin(start)};
    double e = start + sweep;
    Vec2 en{c.x + r * std::cos(e), c.y + r * std::sin(e)};
    return dist2(s, p) <= dist2(en, p) ? s : en;
}

// Closest point on a polyline without allocating.
Vec2 closestOnPolyline(const PolylineData& pl, Vec2 p) {
    int n = static_cast<int>(pl.v.size());
    if (n == 0)
        return p;
    if (n == 1)
        return pl.v[0].p;
    int count = segmentCount(pl);
    Vec2 best = pl.v[0].p;
    double bd = std::numeric_limits<double>::infinity();
    for (int i = 0; i < count; ++i) {
        const PolyVertex& a = pl.v[static_cast<std::size_t>(i)];
        const PolyVertex& b = pl.v[static_cast<std::size_t>((i + 1) % n)];
        Vec2 q;
        if (a.bulge != 0) {
            if (auto arc = bulgeArc(a.p, b.p, a.bulge))
                q = closestOnArc(arc->center, arc->radius, arc->startAngle, arc->sweep, p);
            else
                q = closestOnSegment(a.p, b.p, p);
        } else {
            q = closestOnSegment(a.p, b.p, p);
        }
        double d = dist2(q, p);
        if (d < bd) {
            bd = d;
            best = q;
        }
    }
    return best;
}

std::optional<Vec2> closestOnDisplay(const DisplayList& list, Vec2 p) {
    Vec2 best{};
    double bd = std::numeric_limits<double>::infinity();
    auto consider = [&](Vec2 q) {
        double d = dist2(q, p);
        if (d < bd) {
            bd = d;
            best = q;
        }
    };
    auto ringIn = [&](const std::vector<Vec2>& pts, bool closed) {
        for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            consider(closestOnSegment(pts[i], pts[i + 1], p));
        if (closed && pts.size() > 2)
            consider(closestOnSegment(pts.back(), pts.front(), p));
        if (pts.size() == 1)
            consider(pts[0]);
    };
    for (const DisplayItem& it : list.items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&it)) {
            ringIn(pl->pts, pl->closed);
        } else if (const auto* f = std::get_if<DisplayFill>(&it)) {
            bool inside = false;
            for (const auto& ring : f->rings) {
                ringIn(ring, true);
                std::size_t n = ring.size();
                for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                    if (((ring[i].y > p.y) != (ring[j].y > p.y)) &&
                        (p.x <
                         (ring[j].x - ring[i].x) * (p.y - ring[i].y) / (ring[j].y - ring[i].y) +
                             ring[i].x))
                        inside = !inside;
                }
            }
            if (inside)
                return p;
        } else if (const auto* pt = std::get_if<DisplayPoints>(&it)) {
            for (const Vec2& q : pt->pts)
                consider(q);
        }
    }
    if (bd == std::numeric_limits<double>::infinity())
        return std::nullopt;
    return best;
}

}  // namespace

namespace {

void sampleCurve(const CurveRef& c, double tol, bool reversed, std::vector<Vec2>& pts) {
    std::vector<Vec2> part;
    if (c.kind() == CurveRef::Kind::Arc && c.radius() > 0) {
        const double step = std::sqrt(8 * std::max(tol, 1e-12) / c.radius());
        const auto n = static_cast<std::size_t>(
            std::clamp(std::ceil(c.sweep() / std::min(step, 1.0)), 4.0, 262144.0));
        part.reserve(n + 1);
        for (std::size_t i = 0; i <= n; ++i)
            part.push_back(c.pointAt(c.startAngle() +
                                     c.sweep() * static_cast<double>(i) / static_cast<double>(n)));
        part.front() = c.startPoint();
        part.back() = c.endPoint();
    } else {
        flatten(c, std::max(tol, 1e-12), part);
    }
    if (reversed)
        std::reverse(part.begin(), part.end());
    for (const Vec2& p : part)
        if (pts.empty() || distance(pts.back(), p) > 0)
            pts.push_back(p);
}

}  // namespace

bool outlinePoints(const Entity& e, double tol, std::vector<Vec2>& out) {
    out.clear();
    if (const auto* pl = entityAs<PolylineData>(e)) {
        for (const Segment& s : segments(*pl))
            sampleCurve(s.curve, tol, s.reversed, out);
        if (pl->closed && out.size() > 1 && distance(out.front(), out.back()) <= 0)
            out.pop_back();
        return !out.empty();
    }
    std::vector<CurveRef> curves;
    if (!curvesOf(e, curves) || curves.empty())
        return false;
    for (const CurveRef& c : curves)
        sampleCurve(c, tol, false, out);
    const double dup = 1e-9 * (1 + std::max(std::fabs(out.front().x), std::fabs(out.front().y)));
    if (curves.size() == 1 && curves[0].isClosed() && out.size() > 1 &&
        distance(out.front(), out.back()) <= dup)
        out.pop_back();
    return !out.empty();
}

bool curvesOf(const Entity& e, std::vector<CurveRef>& out) {
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            out.push_back(CurveRef::line(d.a, d.b));
            return true;
        }
        case EntityKind::Circle: {
            const auto& d = *entityAs<CircleData>(e);
            out.push_back(CurveRef::circle(d.center, d.radius));
            return true;
        }
        case EntityKind::Arc:
            out.push_back(arcCurve(*entityAs<ArcData>(e)));
            return true;
        case EntityKind::Polyline:
            for (Segment& s : segments(*entityAs<PolylineData>(e)))
                out.push_back(std::move(s.curve));
            return true;
        case EntityKind::Ray: {
            const auto& d = *entityAs<RayData>(e);
            out.push_back(CurveRef::line(rayStart(d), rayEnd(d)));
            return true;
        }
        case EntityKind::Ellipse: {
            const auto& d = *entityAs<EllipseData>(e);
            if (d.ratio > 0 && lengthSquared(d.majorAxis) > 0)
                out.push_back(ellipseCurve(d));
            return true;
        }
        case EntityKind::Spline: {
            const auto& d = *entityAs<SplineData>(e);
            if (auto sp = detail::cachedSpline(d))
                out.push_back(CurveRef::spline(sp, sp->tMin(), sp->tMax()));
            return true;
        }
        default:
            return false;
    }
}

// ---- Intersections
// -------------------------------------------------------------------------------

namespace {

struct Piece {
    CurveRef curve{};
    const Segment* seg = nullptr;  // polyline segment, else null
    bool first = false, last = false;
};

}  // namespace

Result<void> intersectEntities(const Entity& a, const Entity& b,
                               std::vector<EntityIntersection>& out, bool extendA, bool extendB) {
    auto gather = [](const Entity& e, std::vector<Segment>& segs, std::vector<Piece>& pcs,
                     bool& poly) -> bool {
        poly = false;
        switch (kind(e)) {
            case EntityKind::Polyline: {
                const auto& pl = *entityAs<PolylineData>(e);
                segs = segments(pl);
                poly = true;
                for (std::size_t i = 0; i < segs.size(); ++i) {
                    Piece p;
                    p.curve = segs[i].curve;
                    p.seg = &segs[i];
                    p.first = !pl.closed && i == 0;
                    p.last = !pl.closed && i + 1 == segs.size();
                    pcs.push_back(std::move(p));
                }
                return true;
            }
            default: {
                std::vector<CurveRef> cs;
                if (!curvesOf(e, cs))
                    return false;
                for (CurveRef& c : cs)
                    pcs.push_back({std::move(c), nullptr, false, false});
                return true;
            }
        }
    };
    std::vector<Segment> sa, sb;
    std::vector<Piece> pa, pb;
    bool polyA = false, polyB = false;
    if (!gather(a, sa, pa, polyA) || !gather(b, sb, pb, polyB))
        return makeError(ErrorCode::Unsupported, "entity has no curve geometry");

    auto paramOf = [](const Piece& p, double t, bool ext, bool& ok) {
        ok = true;
        if (!p.seg)
            return t;
        double u = p.seg->toLocalUnbounded(t);
        if (u < -1e-9) {
            ok = ext && p.first;
            u = 0;  // extension reported as the clamped start; the point carries the position
        } else if (u > 1 + 1e-9) {
            ok = ext && p.last;
            u = 1;
        }
        u = std::clamp(u, 0.0, 1.0);
        return p.seg->index + u;
    };
    std::vector<Intersection> tmp;
    std::size_t first = out.size();
    const auto pairUp = [&](const Piece& x, const Piece& y) {
        tmp.clear();
        bool ex = extendA && (!x.seg || x.first || x.last);
        bool ey = extendB && (!y.seg || y.first || y.last);
        intersect(x.curve, y.curve, tmp, ex, ey);
        for (const Intersection& i : tmp) {
            bool okA = true, okB = true;
            double ta = paramOf(x, i.tA, extendA, okA);
            double tb = paramOf(y, i.tB, extendB, okB);
            if (!okA || !okB)
                continue;
            bool dup = false;
            for (std::size_t k = first; k < out.size(); ++k)
                if (distance(out[k].p, i.p) <=
                    std::max(tol::kLinear,
                             1e-13 * std::max({std::abs(i.p.x), std::abs(i.p.y), 1.0}))) {
                    dup = true;
                    break;
                }
            if (!dup)
                out.push_back({i.p, ta, tb});
        }
    };
    if (pa.size() * pb.size() <= 4096) {
        for (const Piece& x : pa)
            for (const Piece& y : pb)
                pairUp(x, y);
        return {};
    }
    // Many pieces on both sides (two long polylines): a grid over the boxes of one side finds, for
    // each piece of the other, the few it can meet. A piece that may be extended has no useful box
    // and meets all of them. Pairs are visited in the same order as above.
    std::vector<BBox2> boxes;
    boxes.reserve(pb.size());
    std::vector<std::size_t> everywhere;  // pieces of b that extend
    const double margin = std::max(tol::kLinear * 10, 1e-6);
    for (std::size_t j = 0; j < pb.size(); ++j) {
        boxes.push_back(pb[j].curve.bounds().inflated(margin));
        if (extendB && (!pb[j].seg || pb[j].first || pb[j].last))
            everywhere.push_back(j);
    }
    const BoxGrid grid(boxes);
    std::vector<std::size_t> hits;
    for (const Piece& x : pa) {
        const bool xExtends = extendA && (!x.seg || x.first || x.last);
        hits.clear();
        if (xExtends) {
            for (std::size_t j = 0; j < pb.size(); ++j)
                hits.push_back(j);
        } else {
            grid.query(x.curve.bounds().inflated(margin), [&](std::size_t j) { hits.push_back(j); });
            hits.insert(hits.end(), everywhere.begin(), everywhere.end());
            std::sort(hits.begin(), hits.end());
            hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
        }
        for (std::size_t j : hits)
            pairUp(x, pb[j]);
    }
    return {};
}

std::optional<Vec2> pointAtParam(const Entity& e, double t) {
    if (kind(e) == EntityKind::Polyline) {
        const auto& pl = *entityAs<PolylineData>(e);
        if (pl.v.empty())
            return std::nullopt;
        return polylinePointAt(pl, t);
    }
    std::vector<CurveRef> cs;
    if (!curvesOf(e, cs) || cs.empty())
        return std::nullopt;
    return cs[0].pointAt(t);
}

Vec2 polylinePointAt(const PolylineData& pl, double t) {
    int n = static_cast<int>(pl.v.size());
    if (n == 0)
        return {};
    int count = segmentCount(pl);
    if (count == 0)
        return pl.v[0].p;
    double ft = std::floor(t);
    int idx = std::clamp(static_cast<int>(ft), 0, count - 1);
    double u = std::clamp(t - idx, 0.0, 1.0);
    const PolyVertex& a = pl.v[static_cast<std::size_t>(idx)];
    const PolyVertex& b = pl.v[static_cast<std::size_t>((idx + 1) % n)];
    if (auto arc = bulgeArc(a.p, b.p, a.bulge)) {
        CurveRef c = CurveRef::arc(arc->center, arc->radius, arc->startAngle, arc->sweep);
        Segment s;
        s.curve = c;
        s.reversed = arc->reversed;
        return s.pointAtLocal(u);
    }
    return lerp(a.p, b.p, u);
}

// ---- Distance and selection
// ------------------------------------------------------------------------

std::optional<Vec2> closestPoint(const Entity& e, Vec2 p, const Document* doc) {
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            return closestOnSegment(d.a, d.b, p);
        }
        case EntityKind::Circle: {
            const auto& d = *entityAs<CircleData>(e);
            return closestOnArc(d.center, d.radius, 0, kTwoPi, p);
        }
        case EntityKind::Arc: {
            const auto& d = *entityAs<ArcData>(e);
            return closestOnArc(d.center, d.radius, d.startAngle, detail::arcSweep(d), p);
        }
        case EntityKind::Polyline:
            return closestOnPolyline(*entityAs<PolylineData>(e), p);
        case EntityKind::Point:
            return entityAs<PointData>(e)->p;
        case EntityKind::Ray:
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            if (cs.empty())
                return std::nullopt;
            return cs[0].closestPoint(p);
        }
        case EntityKind::Proxy:
            return closestOnDisplay(entityAs<ProxyData>(e)->graphics, p);
        default:
            break;
    }
    if (!doc || !DisplayRegistry::has(kind(e)))
        return std::nullopt;
    DisplayList list;
    buildDisplay(*doc, e, DisplayContext{}, list);
    return closestOnDisplay(list, p);
}

double distance(const Entity& e, Vec2 p, const Document* doc) {
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            return std::sqrt(dist2(closestOnSegment(d.a, d.b, p), p));
        }
        case EntityKind::Circle: {
            const auto& d = *entityAs<CircleData>(e);
            return std::abs(std::sqrt(dist2(d.center, p)) - d.radius);
        }
        default:
            break;
    }
    auto q = closestPoint(e, p, doc);
    if (!q)
        return std::numeric_limits<double>::infinity();
    return std::sqrt(dist2(*q, p));
}

bool hitTest(const Entity& e, Vec2 p, double apertureMm, const Document* doc) {
    switch (kind(e)) {
        case EntityKind::Ellipse:
        case EntityKind::Spline:
        case EntityKind::Arc:
        case EntityKind::Polyline: {
            BBox2 b = doc ? bounds(e, *doc) : bounds(e);
            if (!b.empty() && !b.inflated(apertureMm).contains(p))
                return false;
            break;
        }
        default:
            break;
    }
    return distance(e, p, doc) <= apertureMm;
}

bool insideWindow(const Entity& e, const BBox2& window, const Document* doc) {
    BBox2 b = doc ? bounds(e, *doc) : bounds(e);
    return !b.empty() && window.containsBox(b);
}

namespace {

// Liang-Barsky: does the segment touch the box?
bool segmentTouchesBox(Vec2 a, Vec2 b, const BBox2& w) {
    double t0 = 0, t1 = 1;
    Vec2 d = b - a;
    double p[4] = {-d.x, d.x, -d.y, d.y};
    double q[4] = {a.x - w.min.x, w.max.x - a.x, a.y - w.min.y, w.max.y - a.y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0)
                return false;
        } else {
            double r = q[i] / p[i];
            if (p[i] < 0) {
                if (r > t1)
                    return false;
                t0 = std::max(t0, r);
            } else {
                if (r < t0)
                    return false;
                t1 = std::min(t1, r);
            }
        }
    }
    return t0 <= t1;
}

bool curveTouchesBox(const CurveRef& c, const BBox2& w) {
    if (!c.bounds().intersects(w))
        return false;
    if (c.kind() == CurveRef::Kind::Line)
        return segmentTouchesBox(c.lineA(), c.lineB(), w);
    if (w.contains(c.startPoint()) || w.contains(c.endPoint()))
        return true;
    Vec2 corners[4] = {w.min, {w.max.x, w.min.y}, w.max, {w.min.x, w.max.y}};
    std::vector<Intersection> hits;
    for (int i = 0; i < 4; ++i) {
        intersect(c, CurveRef::line(corners[i], corners[(i + 1) % 4]), hits);
        if (!hits.empty())
            return true;
    }
    return false;
}

}  // namespace

bool crossesWindow(const Entity& e, const BBox2& window, const Document* doc) {
    BBox2 b = doc ? bounds(e, *doc) : bounds(e);
    if (b.empty() || !b.intersects(window))
        return false;
    switch (kind(e)) {
        case EntityKind::Point:
            return window.contains(entityAs<PointData>(e)->p);
        case EntityKind::Line:
        case EntityKind::Ray:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            for (const CurveRef& c : cs)
                if (curveTouchesBox(c, window))
                    return true;
            return false;
        }
        case EntityKind::Polyline: {
            const auto& pl = *entityAs<PolylineData>(e);
            if (pl.v.size() == 1)
                return window.contains(pl.v[0].p);
            for (const Segment& s : segments(pl))
                if (curveTouchesBox(s.curve, window))
                    return true;
            return false;
        }
        default:
            return true;  // bounds overlap is the best available answer
    }
}

// ---- Snap helpers
// --------------------------------------------------------------------------------------

std::vector<Vec2> endpoints(const Entity& e) {
    std::vector<Vec2> out;
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            out = {d.a, d.b};
            break;
        }
        case EntityKind::Arc:
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            for (const CurveRef& c : cs)
                if (!c.isClosed()) {
                    out.push_back(c.startPoint());
                    out.push_back(c.endPoint());
                }
            break;
        }
        case EntityKind::Polyline:
            for (const PolyVertex& v : entityAs<PolylineData>(e)->v)
                out.push_back(v.p);
            break;
        case EntityKind::Ray:
            if (!entityAs<RayData>(e)->infinite)
                out.push_back(entityAs<RayData>(e)->origin);
            break;
        default:
            break;
    }
    return out;
}

std::vector<Vec2> midpoints(const Entity& e) {
    std::vector<Vec2> out;
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            out.push_back((d.a + d.b) * 0.5);
            break;
        }
        case EntityKind::Arc:
        case EntityKind::Ellipse: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            for (const CurveRef& c : cs)
                if (!c.isClosed())
                    out.push_back(c.pointAt(0.5 * (c.t0() + c.t1())));
            break;
        }
        case EntityKind::Polyline:
            for (const Segment& s : segments(*entityAs<PolylineData>(e)))
                out.push_back(s.pointAtLocal(0.5));
            break;
        default:
            break;
    }
    return out;
}

std::vector<Vec2> centers(const Entity& e) {
    std::vector<Vec2> out;
    switch (kind(e)) {
        case EntityKind::Circle:
            out.push_back(entityAs<CircleData>(e)->center);
            break;
        case EntityKind::Arc:
            out.push_back(entityAs<ArcData>(e)->center);
            break;
        case EntityKind::Ellipse:
            out.push_back(entityAs<EllipseData>(e)->center);
            break;
        case EntityKind::Polyline:
            for (const Segment& s : segments(*entityAs<PolylineData>(e)))
                if (s.curve.kind() == CurveRef::Kind::Arc)
                    out.push_back(s.curve.center());
            break;
        default:
            break;
    }
    return out;
}

std::vector<Vec2> quadrants(const Entity& e) {
    std::vector<Vec2> out;
    auto fromArc = [&](const CurveRef& c) {
        for (int k = 0; k < 4; ++k) {
            double a = k * kPi / 2;
            if (c.containsParam(a, 0))
                out.push_back(c.pointAt(a));
        }
    };
    switch (kind(e)) {
        case EntityKind::Circle:
        case EntityKind::Arc: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            for (const CurveRef& c : cs)
                fromArc(c);
            break;
        }
        case EntityKind::Ellipse: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            for (const CurveRef& c : cs)
                for (int k = 0; k < 4; ++k) {
                    double t = k * kPi / 2;
                    if (c.containsParam(t, 0))
                        out.push_back(c.pointAt(t));
                }
            break;
        }
        case EntityKind::Polyline:
            for (const Segment& s : segments(*entityAs<PolylineData>(e)))
                if (s.curve.kind() == CurveRef::Kind::Arc)
                    fromArc(s.curve);
            break;
        default:
            break;
    }
    return out;
}

// ---- Measures
// ------------------------------------------------------------------------------------------

double signedArea(const PolylineData& pl) {
    int n = static_cast<int>(pl.v.size());
    if (n < 3 && !(n == 2 && (pl.v[0].bulge != 0 || (pl.closed && pl.v[1].bulge != 0))))
        return 0;
    double a = 0;
    const Vec2 o = pl.v[0].p;  // measured from the first vertex: survey-sized coordinates
    for (int i = 0; i < n; ++i) {
        const PolyVertex& p = pl.v[static_cast<std::size_t>(i)];
        const PolyVertex& q = pl.v[static_cast<std::size_t>((i + 1) % n)];
        // Segment i exists for open polylines only up to n-2, but the area closes the shape;
        // the closing chord has no bulge for an open polyline.
        double bulge = (i == n - 1 && !pl.closed) ? 0.0 : p.bulge;
        a += 0.5 * cross(p.p - o, q.p - o);
        if (bulge != 0) {
            if (auto arc = bulgeArc(p.p, q.p, bulge)) {
                double seg = 0.5 * arc->radius * arc->radius * (arc->sweep - std::sin(arc->sweep));
                a += bulge > 0 ? seg : -seg;
            }
        }
    }
    return a;
}

double area(const PolylineData& pl) {
    return std::abs(signedArea(pl));
}

double length(const PolylineData& pl) {
    double total = 0;
    for (const Segment& s : segments(pl))
        total += s.curve.length();
    return total;
}

double length(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Polyline:
            return length(*entityAs<PolylineData>(e));
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            return distance(d.a, d.b);
        }
        case EntityKind::Circle:
            return kTwoPi * entityAs<CircleData>(e)->radius;
        case EntityKind::Arc:
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            double total = 0;
            for (const CurveRef& c : cs)
                total += c.length();
            return total;
        }
        default:
            return 0;
    }
}

}  // namespace tbcad::geom

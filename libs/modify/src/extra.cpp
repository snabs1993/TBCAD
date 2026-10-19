#include "tbcad/modify/extra.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>

#include "tbcad/modify/break_join.h"
#include "tbcad/modify/trim.h"

namespace tbcad::modify {

using geom::CurveRef;

namespace {

Error err(std::string m) {
    return Error{ErrorCode::InvalidArgument, std::move(m)};
}

Seg arcToSeg(const ArcData& a) {
    double sweep = normalizeAngle(a.endAngle - a.startAngle);
    if (sweep <= 0)
        sweep = kTwoPi;
    Seg s;
    s.p0 = a.center + Vec2{std::cos(a.startAngle), std::sin(a.startAngle)} * a.radius;
    s.p1 = a.center + Vec2{std::cos(a.endAngle), std::sin(a.endAngle)} * a.radius;
    s.bulge = std::tan(sweep / 4);
    return s;
}

// Travel-direction segments of an open line, arc or polyline; nullopt for anything else.
std::optional<std::vector<Seg>> openSegs(const Entity& e) {
    if (const auto* l = entityAs<LineData>(e)) {
        if (samePoint(l->a, l->b))
            return std::nullopt;
        return std::vector<Seg>{Seg{l->a, l->b, 0}};
    }
    if (const auto* a = entityAs<ArcData>(e))
        return std::vector<Seg>{arcToSeg(*a)};
    if (const auto* p = entityAs<PolylineData>(e)) {
        if (p->closed)
            return std::nullopt;
        auto segs = polylineSegs(cleanPolyline(*p));
        if (segs.empty())
            return std::nullopt;
        return segs;
    }
    return std::nullopt;
}

std::vector<Seg> reversedSegs(const std::vector<Seg>& segs) {
    std::vector<Seg> out;
    out.reserve(segs.size());
    for (auto it = segs.rbegin(); it != segs.rend(); ++it)
        out.push_back(segReversed(*it));
    return out;
}

double totalLength(const std::vector<Seg>& segs) {
    double t = 0;
    for (const Seg& s : segs)
        t += segLength(s);
    return t;
}

// Same entity kind as `src` built from a segment chain.
Result<EntityEdit> fromSegs(const Entity& src, const std::vector<Seg>& segs) {
    if (segs.empty())
        return err("Nothing would be left of the object.");
    EntityEdit r;
    if (entityAs<PolylineData>(src)) {
        r.modified = withData(src, polylineFromSegs(segs, false, entityAs<PolylineData>(src)));
    } else {
        auto d = dataFromCurve(segCurve(segs.front()));
        if (!d)
            return err("The object cannot be changed.");
        r.modified = withData(src, std::move(*d));
    }
    return r;
}

// Changes the last segment's end by `delta` mm of arc length.
Result<std::vector<Seg>> changeEnd(std::vector<Seg> segs, double delta) {
    if (std::abs(delta) < 1e-12)
        return segs;
    if (delta > 0) {
        Seg& s = segs.back();
        double len = segLength(s);
        double u = (len + delta) / len;
        if (s.isArc()) {
            auto g = arcGeom(s);
            if (!g || std::abs(g->theta) * u >= kTwoPi - 1e-6)
                return err("The arc would become a full circle.");
        }
        // Built from the sweep, not from the new end point: an arc extended past half of the
        // remaining circle would otherwise read as lying before its start.
        Seg longer{s.p0, segPoint(s, u), 0};
        if (s.isArc())
            longer.bulge = std::tan(4.0 * std::atan(s.bulge) * u / 4.0);
        s = longer;
        return segs;
    }
    double rem = -delta;
    if (rem >= totalLength(segs) - 1e-9)
        return err("The object would disappear.");
    while (!segs.empty()) {
        double len = segLength(segs.back());
        if (rem < len - 1e-9) {
            segs.back() = segSub(segs.back(), 0.0, (len - rem) / len);
            break;
        }
        rem -= len;
        segs.pop_back();
        if (rem <= 1e-9)
            break;
    }
    return segs;
}

// Index 0 = start end, 1 = end end: the end of the segment chain closest to the pick.
bool nearerToEnd(const std::vector<Seg>& segs, Vec2 pick) {
    double total = totalLength(segs);
    double g = closestGlobalParam(segs, pick);
    auto idx = static_cast<std::size_t>(std::min<double>(std::floor(g), segs.size() - 1.0));
    double before = 0;
    for (std::size_t i = 0; i < idx; ++i)
        before += segLength(segs[i]);
    double at = before + (g - static_cast<double>(idx)) * segLength(segs[idx]);
    return at >= total / 2;
}

Result<EntityEdit> lengthenCurve(const Entity& e, const CurveRef& c, Vec2 pick, double delta,
                                 std::optional<Vec2> toPoint);
std::optional<CurveRef> curveForLengthen(const Entity& e);

Result<EntityEdit> lengthenBy(const Entity& e, Vec2 pick, double delta) {
    auto segs = openSegs(e);
    if (!segs) {
        if (auto c = curveForLengthen(e))
            return lengthenCurve(e, *c, pick, delta, std::nullopt);
        return err(
            "Only lines, arcs, elliptical arcs and open polylines or splines can be lengthened.");
    }
    bool atEnd = nearerToEnd(*segs, pick);
    std::vector<Seg> work = atEnd ? *segs : reversedSegs(*segs);
    auto changed = changeEnd(std::move(work), delta);
    if (!changed)
        return changed.error();
    std::vector<Seg> out = atEnd ? std::move(changed.value()) : reversedSegs(changed.value());
    return fromSegs(e, out);
}

// Flattened path of a curve entity, in travel order.
bool pathOf(const Entity& e, std::vector<Vec2>& pts, bool& closed) {
    pts.clear();
    closed = false;
    std::vector<CurveRef> curves;
    if (!geom::curvesOf(e, curves) || curves.empty())
        return false;
    constexpr double kFlat = 1e-6;
    if (const auto* p = entityAs<PolylineData>(e)) {
        closed = p->closed;
    } else if (kind(e) == EntityKind::Circle) {
        closed = true;
    } else if (const auto* el = entityAs<EllipseData>(e)) {
        closed = std::abs(el->endParam - el->startParam) >= kTwoPi - 1e-9;
    } else if (const auto* sp = entityAs<SplineData>(e)) {
        closed = sp->closed || sp->periodic;
    }
    // In the direction of travel: a polyline arc with a negative bulge runs clockwise.
    if (!geom::outlinePoints(e, kFlat, pts))
        return false;
    if (closed && pts.size() > 1 && samePoint(pts.front(), pts.back(), 1e-9))
        pts.pop_back();
    return pts.size() >= 2;
}

struct Path {
    std::vector<Vec2> pts;  // closed paths repeat the first point at the end
    std::vector<double> cum;
    double length() const { return cum.empty() ? 0.0 : cum.back(); }
    Vec2 at(double s) const {
        s = std::clamp(s, 0.0, length());
        auto it = std::upper_bound(cum.begin(), cum.end(), s);
        std::size_t i = std::min<std::size_t>(it - cum.begin(), cum.size() - 1);
        if (i == 0)
            return pts.front();
        double seg = cum[i] - cum[i - 1];
        double u = seg > 0 ? (s - cum[i - 1]) / seg : 0.0;
        return pts[i - 1] + (pts[i] - pts[i - 1]) * u;
    }
};

Result<std::pair<Path, bool>> buildPath(const Entity& e) {
    std::vector<Vec2> pts;
    bool closed = false;
    if (!pathOf(e, pts, closed))
        return err("This object cannot be divided or measured.");
    if (closed)
        pts.push_back(pts.front());
    Path p;
    p.pts = std::move(pts);
    p.cum.assign(p.pts.size(), 0.0);
    for (std::size_t i = 1; i < p.pts.size(); ++i)
        p.cum[i] = p.cum[i - 1] + distance(p.pts[i - 1], p.pts[i]);
    if (p.length() <= 0)
        return err("The object has no length.");
    return std::make_pair(std::move(p), closed);
}

// ---- overkill helpers ---------------------------------------------------------------------------

bool sameProps(const EntityCommon& a, const EntityCommon& b) {
    return a.layer == b.layer && a.linetype == b.linetype && a.color == b.color &&
           a.lineweight == b.lineweight && std::abs(a.linetypeScale - b.linetypeScale) < 1e-9;
}

bool angleNear(double a, double b, double tol) {
    double d = std::abs(normalizeAngle(a - b));
    return std::min(d, kTwoPi - d) <= tol;
}

bool sameGeometry(const Entity& a, const Entity& b, double tol) {
    if (kind(a) != kind(b))
        return false;
    switch (kind(a)) {
        case EntityKind::Line: {
            const auto& x = *entityAs<LineData>(a);
            const auto& y = *entityAs<LineData>(b);
            return (samePoint(x.a, y.a, tol) && samePoint(x.b, y.b, tol)) ||
                   (samePoint(x.a, y.b, tol) && samePoint(x.b, y.a, tol));
        }
        case EntityKind::Circle: {
            const auto& x = *entityAs<CircleData>(a);
            const auto& y = *entityAs<CircleData>(b);
            return samePoint(x.center, y.center, tol) && std::abs(x.radius - y.radius) <= tol;
        }
        case EntityKind::Arc: {
            const auto& x = *entityAs<ArcData>(a);
            const auto& y = *entityAs<ArcData>(b);
            double angTol = tol / std::max(x.radius, 1e-9);
            return samePoint(x.center, y.center, tol) && std::abs(x.radius - y.radius) <= tol &&
                   angleNear(x.startAngle, y.startAngle, angTol) &&
                   angleNear(x.endAngle, y.endAngle, angTol);
        }
        case EntityKind::Point:
            return samePoint(entityAs<PointData>(a)->p, entityAs<PointData>(b)->p, tol);
        case EntityKind::Ellipse: {
            const auto& x = *entityAs<EllipseData>(a);
            const auto& y = *entityAs<EllipseData>(b);
            return samePoint(x.center, y.center, tol) && samePoint(x.majorAxis, y.majorAxis, tol) &&
                   std::abs(x.ratio - y.ratio) <= 1e-9 &&
                   std::abs(x.startParam - y.startParam) <= 1e-9 &&
                   std::abs(x.endParam - y.endParam) <= 1e-9;
        }
        case EntityKind::Polyline: {
            const auto& x = *entityAs<PolylineData>(a);
            const auto& y = *entityAs<PolylineData>(b);
            if (x.closed != y.closed || x.v.size() != y.v.size() || x.v.empty())
                return false;
            std::size_t n = x.v.size();
            auto matches = [&](bool reversed, std::size_t shift) {
                for (std::size_t i = 0; i < n; ++i) {
                    std::size_t j = reversed ? (shift + n - i) % n : (shift + i) % n;
                    if (!x.closed && (reversed ? j != n - 1 - i : j != i))
                        return false;
                    if (!samePoint(x.v[i].p, y.v[j].p, tol))
                        return false;
                    // Bulge of the segment starting here; reversed travel flips it onto the
                    // previous vertex.
                    if (i + 1 < n || x.closed) {
                        double bx = x.v[i].bulge;
                        double by;
                        if (!reversed) {
                            by = y.v[j].bulge;
                        } else {
                            std::size_t jn = (j + n - 1) % n;
                            if (!x.closed && j == 0)
                                return false;
                            by = -y.v[jn].bulge;
                        }
                        if (std::abs(bx - by) > 1e-9)
                            return false;
                    }
                }
                return true;
            };
            if (!x.closed)
                return matches(false, 0) || matches(true, n - 1);
            for (std::size_t s = 0; s < n; ++s) {
                if (matches(false, s) || matches(true, s))
                    return true;
            }
            return false;
        }
        case EntityKind::Text: {
            const auto& x = *entityAs<TextData>(a);
            const auto& y = *entityAs<TextData>(b);
            return x.utf8 == y.utf8 && samePoint(x.position, y.position, tol) &&
                   std::abs(x.height - y.height) <= tol &&
                   std::abs(x.rotation - y.rotation) <= 1e-9 &&
                   std::abs(x.widthFactor - y.widthFactor) <= 1e-9 && x.h == y.h && x.v == y.v &&
                   x.style == y.style && !x.attdef && !y.attdef;
        }
        case EntityKind::MText: {
            const auto& x = *entityAs<MTextData>(a);
            const auto& y = *entityAs<MTextData>(b);
            return x.utf8 == y.utf8 && samePoint(x.position, y.position, tol) &&
                   std::abs(x.height - y.height) <= tol && std::abs(x.width - y.width) <= tol &&
                   std::abs(x.rotation - y.rotation) <= 1e-9 && x.attach == y.attach &&
                   x.style == y.style && x.columns == y.columns;
        }
        case EntityKind::Insert: {
            const auto& x = *entityAs<InsertData>(a);
            const auto& y = *entityAs<InsertData>(b);
            if (x.array || y.array || !(x.block == y.block) ||
                x.attribs.size() != y.attribs.size() || x.dynValues != y.dynValues ||
                x.asmValues.size() != y.asmValues.size() || !x.clip.empty() || !y.clip.empty())
                return false;
            for (std::size_t i = 0; i < x.attribs.size(); ++i) {
                if (x.attribs[i].tag != y.attribs[i].tag ||
                    x.attribs[i].value != y.attribs[i].value ||
                    x.attribs[i].offset != y.attribs[i].offset)
                    return false;
            }
            return samePoint(x.position, y.position, tol) && samePoint(x.scale, y.scale, 1e-9) &&
                   std::abs(x.rotation - y.rotation) <= 1e-9;
        }
        case EntityKind::Spline: {
            const auto& x = *entityAs<SplineData>(a);
            const auto& y = *entityAs<SplineData>(b);
            if (x.degree != y.degree || x.closed != y.closed ||
                x.control.size() != y.control.size() || x.fit.size() != y.fit.size() ||
                x.weights != y.weights)
                return false;
            for (std::size_t i = 0; i < x.control.size(); ++i) {
                if (!samePoint(x.control[i], y.control[i], tol))
                    return false;
            }
            for (std::size_t i = 0; i < x.fit.size(); ++i) {
                if (!samePoint(x.fit[i], y.fit[i], tol))
                    return false;
            }
            return true;
        }
        default:
            return false;
    }
}

bool takesPart(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Point:
        case EntityKind::Ellipse:
        case EntityKind::Polyline:
        case EntityKind::Text:
        case EntityKind::MText:
        case EntityKind::Insert:
        case EntityKind::Spline:
            return true;
        default:
            return false;
    }
}

double minX(const Entity& e) {
    std::vector<CurveRef> cs;
    if (const auto* p = entityAs<PointData>(e))
        return p->p.x;
    if (const auto* t = entityAs<TextData>(e))
        return t->position.x;
    if (const auto* t = entityAs<MTextData>(e))
        return t->position.x;
    if (const auto* i = entityAs<InsertData>(e))
        return i->position.x;
    if (const auto* sp = entityAs<SplineData>(e)) {
        double m = 0;
        bool any = false;
        for (const auto* pts : {&sp->control, &sp->fit}) {
            for (const Vec2& q : *pts) {
                m = any ? std::min(m, q.x) : q.x;
                any = true;
            }
        }
        return m;
    }
    if (!geom::curvesOf(e, cs) || cs.empty())
        return 0;
    double m = cs.front().bounds().min.x;
    for (const CurveRef& c : cs)
        m = std::min(m, c.bounds().min.x);
    return m;
}

// Elliptical arcs and open splines are lengthened along their own curve: an arc runs on round its
// ellipse, a spline continues along its end tangent.
std::optional<CurveRef> curveForLengthen(const Entity& e) {
    std::vector<CurveRef> cs;
    if (const auto* el = entityAs<EllipseData>(e)) {
        if (normalizeAngle(el->endParam - el->startParam) < 1e-12)
            return std::nullopt;
    } else if (const auto* sp = entityAs<SplineData>(e)) {
        if (sp->closed || sp->periodic)
            return std::nullopt;
    } else {
        return std::nullopt;
    }
    if (!geom::curvesOf(e, cs) || cs.size() != 1)
        return std::nullopt;
    return cs[0];
}

Result<EntityEdit> lengthenCurve(const Entity& e, const CurveRef& c, Vec2 pick, double delta,
                                 std::optional<Vec2> toPoint) {
    const bool atEnd = distance(pick, c.endPoint()) <= distance(pick, c.startPoint());
    const double len = c.length();
    EntityEdit edit;
    if (const auto* el = entityAs<EllipseData>(e)) {
        EllipseData out = *el;
        const double gap = kTwoPi - c.sweep();
        if (toPoint) {
            const CurveRef full =
                CurveRef::ellipse(el->center, el->majorAxis, el->ratio, el->startParam, kTwoPi);
            const double t = normalizeAngle(full.paramAt(*toPoint));
            (atEnd ? out.endParam : out.startParam) = t;
            if (normalizeAngle(out.endParam - out.startParam) < 1e-9)
                return err("The result would have no length.");
            edit.modified = withData(e, out);
            return edit;
        }
        if (delta < 0) {
            if (-delta >= len - tol::kLinear)
                return err("The object would have no length left.");
            if (atEnd)
                out.endParam = normalizeAngle(c.paramAtLength(len + delta));
            else
                out.startParam = normalizeAngle(c.paramAtLength(-delta));
        } else {
            const CurveRef room =
                atEnd ? CurveRef::ellipse(el->center, el->majorAxis, el->ratio, el->endParam, gap)
                      : CurveRef::ellipse(el->center, el->majorAxis, el->ratio,
                                          el->startParam - gap, gap);
            const double roomLen = room.length();
            if (delta >= roomLen - tol::kLinear)
                return err("The ellipse would close on itself.");
            if (atEnd)
                out.endParam = normalizeAngle(room.paramAtLength(delta));
            else
                out.startParam = normalizeAngle(room.paramAtLength(roomLen - delta));
        }
        edit.modified = withData(e, out);
        return edit;
    }
    const auto& sd = *entityAs<SplineData>(e);
    const Vec2 from = atEnd ? c.endPoint() : c.startPoint();
    Vec2 dir = c.tangentAt(atEnd ? c.t1() : c.t0());
    if (!atEnd)
        dir = dir * -1.0;
    if (tol::isZeroLength(dir))
        return err("The spline has no direction at this end.");
    if (toPoint)
        delta = dot(*toPoint - from, dir);
    if (delta < 0) {
        if (-delta >= len - tol::kLinear)
            return err("The object would have no length left.");
        const double ta = atEnd ? c.t0() : c.paramAtLength(-delta);
        const double tb = atEnd ? c.paramAtLength(len + delta) : c.t1();
        auto sub = geom::splineSubcurve(sd, ta, tb);
        if (!sub)
            return err("The spline could not be shortened.");
        edit.modified = withData(e, Indirect<SplineData>(std::move(sub.value())));
        return edit;
    }
    if (delta <= tol::kLinear)
        return err("The result would have no change.");
    edit.modified =
        withData(e, Indirect<SplineData>(splineWithStraightRun(sd, atEnd, from + dir * delta)));
    return edit;
}

}  // namespace

// ---- LENGTHEN -----------------------------------------------------------------------------------

std::optional<double> lengthenableLength(const Entity& e) {
    auto segs = openSegs(e);
    if (!segs) {
        if (auto c = curveForLengthen(e))
            return c->length();
        return std::nullopt;
    }
    return totalLength(*segs);
}

Result<EntityEdit> lengthenEntity(const Entity& e, Vec2 pick, LengthenMode mode, double value) {
    auto len = lengthenableLength(e);
    if (!len)
        return err(
            "Only lines, arcs, elliptical arcs and open polylines or splines can be lengthened.");
    switch (mode) {
        case LengthenMode::Delta:
            return lengthenBy(e, pick, value);
        case LengthenMode::Percent:
            if (value <= 0)
                return err("The percentage must be positive.");
            return lengthenBy(e, pick, *len * (value / 100.0 - 1.0));
        case LengthenMode::Total:
            if (value <= 0)
                return err("The length must be positive.");
            return lengthenBy(e, pick, value - *len);
    }
    return err("Unknown lengthen mode.");
}

Result<EntityEdit> lengthenToPoint(const Entity& e, Vec2 pick, Vec2 target) {
    auto segs = openSegs(e);
    if (!segs) {
        if (auto c = curveForLengthen(e))
            return lengthenCurve(e, *c, pick, 0, target);
        return err(
            "Only lines, arcs, elliptical arcs and open polylines or splines can be lengthened.");
    }
    bool atEnd = nearerToEnd(*segs, pick);
    const Seg& s = atEnd ? segs->back() : segReversed(segs->front());
    double u = segParamUnbounded(s, target);
    return lengthenBy(e, pick, (u - 1.0) * segLength(s));
}

// ---- REVERSE ------------------------------------------------------------------------------------

Result<EntityEdit> reverseEntity(const Entity& e) {
    EntityEdit r;
    if (const auto* l = entityAs<LineData>(e)) {
        r.modified = withData(e, LineData{l->b, l->a});
        return r;
    }
    if (const auto* p = entityAs<PolylineData>(e)) {
        PolylineData out;
        out.closed = p->closed;
        out.constantWidth = p->constantWidth;
        std::size_t n = p->v.size();
        out.v.resize(n);
        for (std::size_t j = 0; j < n; ++j) {
            out.v[j].p = p->v[n - 1 - j].p;
            bool hasSeg = p->closed || j + 1 < n;
            if (!hasSeg)
                continue;
            const PolyVertex& old = p->v[(n - 2 - j + n) % n];
            out.v[j].bulge = -old.bulge;
            out.v[j].startWidth = old.endWidth;
            out.v[j].endWidth = old.startWidth;
        }
        r.modified = withData(e, std::move(out));
        return r;
    }
    if (const auto* sp = entityAs<SplineData>(e)) {
        SplineData out = *sp;
        std::reverse(out.control.begin(), out.control.end());
        std::reverse(out.weights.begin(), out.weights.end());
        std::reverse(out.fit.begin(), out.fit.end());
        if (!out.knots.empty()) {
            double lo = out.knots.front(), hi = out.knots.back();
            for (double& k : out.knots)
                k = lo + hi - k;
            std::reverse(out.knots.begin(), out.knots.end());
        }
        r.modified = withData(e, Indirect<SplineData>(std::move(out)));
        return r;
    }
    return err("This object has no direction to reverse.");
}

// ---- DIVIDE / MEASURE ---------------------------------------------------------------------------

Result<std::vector<Vec2>> dividePoints(const Entity& e, int count) {
    if (count < 2 || count > 32767)
        return err("The number of segments must be between 2 and 32767.");
    auto built = buildPath(e);
    if (!built)
        return built.error();
    const auto& [path, closed] = built.value();
    std::vector<Vec2> out;
    double step = path.length() / count;
    for (int k = closed ? 0 : 1; k < count; ++k)
        out.push_back(path.at(step * k));
    return out;
}

Result<std::vector<Vec2>> measurePoints(const Entity& e, double segment, bool fromEnd) {
    if (segment <= 0)
        return err("The segment length must be positive.");
    auto built = buildPath(e);
    if (!built)
        return built.error();
    const auto& [path, closed] = built.value();
    double len = path.length();
    if (len / segment > 100000)
        return err("That would create more than 100000 points.");
    std::vector<Vec2> out;
    for (int k = 1;; ++k) {
        double s = segment * k;
        if (s > len + 1e-5 || (closed && s >= len - 1e-5))
            break;
        out.push_back(path.at(fromEnd ? len - s : s));
    }
    return out;
}

// ---- ALIGN --------------------------------------------------------------------------------------

Result<Transform2> alignTransform(Vec2 src1, Vec2 dst1, std::optional<Vec2> src2,
                                  std::optional<Vec2> dst2, bool scale) {
    if (!src2 || !dst2)
        return Transform2::translate(dst1 - src1);
    Vec2 sv = *src2 - src1, dv = *dst2 - dst1;
    double sl = length(sv), dl = length(dv);
    if (sl < 1e-9 || dl < 1e-9)
        return err("The two points of a pair must differ.");
    double angle = std::atan2(dv.y, dv.x) - std::atan2(sv.y, sv.x);
    double f = scale ? dl / sl : 1.0;
    Transform2 t = Transform2::scale(f, f, src1);
    t = compose(Transform2::rotate(angle, src1), t);
    t = compose(Transform2::translate(dst1 - src1), t);
    return t;
}

// ---- OVERKILL -----------------------------------------------------------------------------------

OverkillResult overkill(const std::vector<const Entity*>& entities, const OverkillOptions& o) {
    OverkillResult res;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        if (entities[i] && takesPart(*entities[i]))
            order.push_back(i);
    }
    std::vector<double> mx(entities.size(), 0.0);
    for (std::size_t i : order)
        mx[i] = minX(*entities[i]);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (kind(*entities[a]) != kind(*entities[b]))
            return kind(*entities[a]) < kind(*entities[b]);
        return mx[a] < mx[b];
    });
    std::vector<bool> gone(entities.size(), false);

    // 1. Exact duplicates: the earliest entity in the input stays.
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        std::size_t i = order[oi];
        for (std::size_t oj = oi + 1; oj < order.size(); ++oj) {
            std::size_t j = order[oj];
            if (kind(*entities[j]) != kind(*entities[i]) || mx[j] - mx[i] > o.tolerance * 4 + 1e-9)
                break;
            std::size_t keep = std::min(i, j), drop = std::max(i, j);
            if (gone[i] || gone[j])
                continue;
            if (!o.ignoreProperties && !sameProps(entities[i]->common, entities[j]->common))
                continue;
            if (sameGeometry(*entities[i], *entities[j], o.tolerance)) {
                gone[drop] = true;
                res.erased.push_back(drop);
                ++res.duplicates;
                (void)keep;
                if (drop == i)
                    break;
            }
        }
    }

    // 2. Overlapping collinear lines.
    if (o.combineOverlapping || o.combineEndToEnd) {
        struct L {
            std::size_t idx;
            Vec2 a, b;
        };
        std::vector<L> lines;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (!entities[i] || gone[i])
                continue;
            if (const auto* l = entityAs<LineData>(*entities[i]); l && !samePoint(l->a, l->b))
                lines.push_back({i, l->a, l->b});
        }
        std::vector<bool> used(lines.size(), false);
        auto canonical = [](const L& l) {
            Vec2 d = l.b - l.a;
            return (d.x < 0 || (d.x == 0 && d.y < 0)) ? d * -1.0 : d;
        };
        std::vector<std::size_t> idxs(lines.size());
        std::iota(idxs.begin(), idxs.end(), std::size_t{0});
        std::vector<double> angOf(lines.size());
        for (std::size_t k = 0; k < lines.size(); ++k) {
            Vec2 dk = canonical(lines[k]);
            angOf[k] = std::atan2(dk.y, dk.x);
        }
        std::sort(idxs.begin(), idxs.end(),
                  [&](std::size_t a, std::size_t b) { return angOf[a] < angOf[b]; });
        // Lines of one direction (within 1e-4 rad) are ordered by their offset across it, so a line
        // only meets the ones close enough to lie on the same line.
        std::vector<double> key(lines.size(), 0.0);
        std::vector<std::size_t> slot(lines.size(), 0);
        std::vector<std::vector<std::size_t>> clusters;
        std::vector<double> windows;
        std::vector<std::size_t> clusterOf(lines.size(), 0);
        for (std::size_t ai = 0; ai < idxs.size();) {
            std::size_t aj = ai;
            while (aj < idxs.size() && angOf[idxs[aj]] - angOf[idxs[ai]] <= 1e-4)
                ++aj;
            Vec2 dc = canonical(lines[idxs[ai]]);
            dc = dc * (1.0 / length(dc));
            std::vector<std::size_t> c(idxs.begin() + static_cast<std::ptrdiff_t>(ai),
                                       idxs.begin() + static_cast<std::ptrdiff_t>(aj));
            double maxLen = 0;
            for (std::size_t k : c) {
                Vec2 mid = (lines[k].a + lines[k].b) * 0.5;
                key[k] = cross(dc, mid);
                clusterOf[k] = clusters.size();
                maxLen = std::max(maxLen, length(lines[k].b - lines[k].a));
            }
            std::sort(c.begin(), c.end(),
                      [&](std::size_t x, std::size_t y) { return key[x] < key[y]; });
            for (std::size_t q = 0; q < c.size(); ++q)
                slot[c[q]] = q;
            clusters.push_back(std::move(c));
            windows.push_back(2 * o.tolerance + 2e-4 * maxLen + 1e-12);
            ai = aj;
        }
        for (std::size_t ai = 0; ai < idxs.size(); ++ai) {
            std::size_t a = idxs[ai];
            if (used[a])
                continue;
            Vec2 d = canonical(lines[a]);
            double dl = length(d);
            d = d * (1.0 / dl);
            double ang = angOf[a];
            std::vector<std::size_t> group{a};
            const std::vector<std::size_t>& cl = clusters[clusterOf[a]];
            double window = windows[clusterOf[a]];
            auto consider = [&](std::size_t b) {
                if (b == a || used[b] || std::abs(angOf[b] - ang) > 1e-4)
                    return;
                if (!o.ignoreProperties &&
                    !sameProps(entities[lines[a].idx]->common, entities[lines[b].idx]->common))
                    return;
                auto off = [&](Vec2 p) { return std::abs(cross(d, p - lines[a].a)); };
                if (off(lines[b].a) <= o.tolerance && off(lines[b].b) <= o.tolerance)
                    group.push_back(b);
            };
            for (std::size_t q = slot[a] + 1; q < cl.size() && key[cl[q]] - key[a] <= window; ++q)
                consider(cl[q]);
            for (std::size_t q = slot[a]; q-- > 0 && key[a] - key[cl[q]] <= window;)
                consider(cl[q]);
            if (group.size() < 2)
                continue;
            struct Iv {
                double lo, hi;
                std::size_t k;
            };
            std::vector<Iv> iv;
            for (std::size_t k : group) {
                double t0 = dot(d, lines[k].a - lines[a].a), t1 = dot(d, lines[k].b - lines[a].a);
                iv.push_back({std::min(t0, t1), std::max(t0, t1), k});
            }
            std::sort(iv.begin(), iv.end(), [](const Iv& x, const Iv& y) { return x.lo < y.lo; });
            for (std::size_t s = 0; s < iv.size();) {
                double lo = iv[s].lo, hi = iv[s].hi;
                std::vector<std::size_t> members{iv[s].k};
                std::size_t e2 = s + 1;
                while (e2 < iv.size()) {
                    bool overlap = iv[e2].lo < hi - o.tolerance;
                    bool touch = iv[e2].lo <= hi + o.tolerance;
                    if (!(overlap ? o.combineOverlapping || o.combineEndToEnd
                                  : (touch && o.combineEndToEnd)))
                        break;
                    hi = std::max(hi, iv[e2].hi);
                    members.push_back(iv[e2].k);
                    ++e2;
                }
                if (members.size() > 1) {
                    std::size_t first = lines[members.front()].idx;
                    for (std::size_t k : members)
                        first = std::min(first, lines[k].idx);
                    for (std::size_t k : members) {
                        used[k] = true;
                        gone[lines[k].idx] = true;
                        res.erased.push_back(lines[k].idx);
                    }
                    res.created.push_back(withData(
                        *entities[first], LineData{lines[a].a + d * lo, lines[a].a + d * hi}));
                }
                s = e2;
            }
        }
    }

    // 3. Arcs of one circle: let JOIN fuse each cluster.
    if (o.combineOverlapping || o.combineEndToEnd) {
        std::vector<std::size_t> arcs;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (entities[i] && !gone[i] && kind(*entities[i]) == EntityKind::Arc)
                arcs.push_back(i);
        }
        std::vector<bool> done(arcs.size(), false);
        for (std::size_t ai = 0; ai < arcs.size(); ++ai) {
            if (done[ai])
                continue;
            const auto& a = *entityAs<ArcData>(*entities[arcs[ai]]);
            std::vector<std::size_t> cluster{ai};
            for (std::size_t bi = ai + 1; bi < arcs.size(); ++bi) {
                const auto& b = *entityAs<ArcData>(*entities[arcs[bi]]);
                if (done[bi] || !samePoint(a.center, b.center, o.tolerance) ||
                    std::abs(a.radius - b.radius) > o.tolerance)
                    continue;
                if (!o.ignoreProperties &&
                    !sameProps(entities[arcs[ai]]->common, entities[arcs[bi]]->common))
                    continue;
                cluster.push_back(bi);
            }
            if (cluster.size() < 2)
                continue;
            std::vector<const Entity*> in;
            for (std::size_t k : cluster)
                in.push_back(entities[arcs[k]]);
            JoinResult jr = joinEntities(in, std::max(o.tolerance, 1e-9));
            for (std::size_t c : jr.consumed) {
                done[cluster[c]] = true;
                res.erased.push_back(arcs[cluster[c]]);
            }
            for (Entity& n : jr.created)
                res.created.push_back(std::move(n));
        }
    }

    // 4. Polylines: drop repeated vertices and vertices that lie on the straight line between
    // their neighbours. Only polylines of one width are touched, so no taper is lost.
    if (o.optimizePolylines) {
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (!entities[i] || gone[i])
                continue;
            const auto* p = entityAs<PolylineData>(*entities[i]);
            if (!p || p->v.size() < 3)
                continue;
            bool uniform = true;
            for (const PolyVertex& v : p->v) {
                if (v.startWidth != p->v.front().startWidth ||
                    v.endWidth != p->v.front().startWidth)
                    uniform = false;
            }
            if (!uniform)
                continue;
            PolylineData q = *p;
            std::size_t before = q.v.size();
            const std::size_t minCount = q.closed ? 3 : 2;
            for (bool changed = true; changed && q.v.size() > minCount;) {
                changed = false;
                for (std::size_t k = 0; k < q.v.size() && q.v.size() > minCount; ++k) {
                    std::size_t n = q.v.size();
                    bool hasPrev = q.closed || k > 0;
                    std::size_t pk = (k + n - 1) % n, nk = (k + 1) % n;
                    if (hasPrev && samePoint(q.v[pk].p, q.v[k].p, o.tolerance)) {
                        q.v[pk].bulge = q.v[k].bulge;  // the segment that leaves survives
                        q.v.erase(q.v.begin() + static_cast<std::ptrdiff_t>(k));
                        changed = true;
                        --k;
                        continue;
                    }
                    bool hasNext = q.closed || k + 1 < n;
                    if (!hasPrev || !hasNext || q.v[pk].bulge != 0 || q.v[k].bulge != 0)
                        continue;
                    Vec2 d = q.v[nk].p - q.v[pk].p;
                    double dl = length(d);
                    if (dl < 1e-12)
                        continue;
                    d = d * (1.0 / dl);
                    Vec2 w = q.v[k].p - q.v[pk].p;
                    if (std::abs(cross(d, w)) <= o.tolerance && dot(d, w) > 0 && dot(d, w) < dl) {
                        q.v.erase(q.v.begin() + static_cast<std::ptrdiff_t>(k));
                        changed = true;
                        --k;
                    }
                }
            }
            if (q.v.size() < before) {
                res.erased.push_back(i);
                res.created.push_back(withData(*entities[i], std::move(q)));
                ++res.optimized;
            }
        }
    }
    std::sort(res.erased.begin(), res.erased.end());
    res.erased.erase(std::unique(res.erased.begin(), res.erased.end()), res.erased.end());
    return res;
}

// ---- PEDIT --------------------------------------------------------------------------------------

Result<Entity> toPolylineEntity(const Entity& e) {
    if (entityAs<PolylineData>(e))
        return e;
    auto segs = openSegs(e);
    if (!segs || kind(e) == EntityKind::Polyline)
        return err("Only lines, arcs and polylines can be turned into polylines.");
    Entity out = withData(e, polylineFromSegs(*segs, false));
    return out;
}

Result<Entity> polylineToSpline(const Entity& e) {
    const auto* p = entityAs<PolylineData>(e);
    if (!p)
        return err("Select a polyline.");
    PolylineData clean = cleanPolyline(*p);
    std::vector<Vec2> pts;
    std::vector<Seg> segs = polylineSegs(clean);
    for (const Seg& s : segs) {
        int n =
            s.isArc() ? std::max(2, static_cast<int>(std::abs(std::atan(s.bulge) * 4) / 0.3)) : 1;
        for (int k = 0; k < n; ++k)
            pts.push_back(segPoint(s, static_cast<double>(k) / n));
    }
    if (!clean.closed && !segs.empty())
        pts.push_back(segs.back().p1);
    if (pts.size() < 3)
        return err("A spline needs at least three points.");
    geom::InterpolateOptions io;
    io.closed = clean.closed;
    auto sp = geom::interpolateSpline(pts, io);
    if (!sp)
        return err("The spline could not be fitted: " + sp.error().message);
    return withData(e, Indirect<SplineData>(std::move(sp.value())));
}

// Fit curve: every segment between two vertices becomes a pair of arcs that meet with a common
// tangent, and the curve passes through each vertex along the bisector of its two chords.
Result<Entity> fitPolylineCurve(const Entity& e) {
    const auto* p = entityAs<PolylineData>(e);
    if (!p)
        return err("Select a polyline.");
    PolylineData clean = cleanPolyline(*p);
    const std::size_t n = clean.v.size();
    if (n < 3 && !(n == 2 && !clean.closed))
        return err("The polyline needs at least two segments.");
    const bool closed = clean.closed;
    auto at = [&](std::ptrdiff_t i) { return clean.v[static_cast<std::size_t>((i + n) % n)].p; };
    auto dirTo = [&](Vec2 a, Vec2 b) { return normalized(b - a); };
    std::vector<Vec2> tangent(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto k = static_cast<std::ptrdiff_t>(i);
        if (!closed && (i == 0 || i + 1 == n)) {
            // An end follows the mirror image of the neighbouring tangent, so the first pair of
            // arcs degenerates to one arc.
            const std::ptrdiff_t nb = i == 0 ? 1 : k - 1;
            const std::ptrdiff_t nb2 = i == 0 ? 2 : k - 2;
            Vec2 chord = i == 0 ? dirTo(at(0), at(1)) : dirTo(at(k - 1), at(k));
            Vec2 tn = n > 2
                          ? normalized(dirTo(at(nb), at(nb2)) + dirTo(at(nb == 1 ? 0 : k), at(nb)))
                          : chord;
            if (n > 2 && i + 1 == n)
                tn = normalized(dirTo(at(k - 2), at(k - 1)) + dirTo(at(k - 1), at(k)));
            if (length(tn) < 0.5)
                tn = chord;
            tangent[i] = normalized(chord * (2 * dot(chord, tn)) - tn);
            if (length(tangent[i]) < 0.5)
                tangent[i] = chord;
            continue;
        }
        Vec2 in = dirTo(at(k - 1), at(k)), out = dirTo(at(k), at(k + 1));
        Vec2 t = in + out;
        tangent[i] = length(t) < 1e-9 ? Vec2{-in.y, in.x} : normalized(t);
    }
    PolylineData out;
    out.closed = closed;
    out.constantWidth = clean.constantWidth;
    const std::size_t segCount = closed ? n : n - 1;
    for (std::size_t i = 0; i < segCount; ++i) {
        const Vec2 p0 = clean.v[i].p, p1 = clean.v[(i + 1) % n].p;
        const Vec2 t0 = tangent[i], t1 = tangent[(i + 1) % n];
        const Vec2 v = p1 - p0;
        // Joint of the two arcs: midpoint of the tangent-line points P0 + d t0 and P1 - d t1,
        // with d chosen so both arcs are isosceles about those points.
        const double a = 2 * (dot(t0, t1) - 1), b = -2 * dot(v, t0 + t1), c = dot(v, v);
        double d = 0;
        if (std::abs(a) < 1e-12) {
            d = std::abs(b) > 1e-12 ? -c / b : length(v) / 2;
        } else {
            const double disc = b * b - 4 * a * c;
            const double sq = std::sqrt(std::max(0.0, disc));
            const double r1 = (-b + sq) / (2 * a), r2 = (-b - sq) / (2 * a);
            d = std::max(r1, r2);
            if (d <= 0)
                d = std::min(r1, r2) > 0 ? std::min(r1, r2) : length(v) / 2;
        }
        const Vec2 j = ((p0 + t0 * d) + (p1 - t1 * d)) * 0.5;
        auto bulgeFor = [](Vec2 from, Vec2 to, Vec2 tangentAtFrom) {
            const Vec2 ch = to - from;
            const double ang = std::atan2(cross(tangentAtFrom, ch), dot(tangentAtFrom, ch));
            return std::tan(ang / 2);
        };
        PolyVertex v0;
        v0.p = p0;
        PolyVertex vj;
        vj.p = j;
        if (distance(j, p0) < 1e-9 || distance(j, p1) < 1e-9) {
            v0.bulge = 0;
            out.v.push_back(v0);
            continue;
        }
        v0.bulge = bulgeFor(p0, j, t0);
        // The second arc is the first one of the reversed pair, which ends along -t1.
        vj.bulge = -bulgeFor(p1, j, -t1);
        out.v.push_back(v0);
        out.v.push_back(vj);
    }
    if (!closed) {
        PolyVertex last;
        last.p = clean.v.back().p;
        out.v.push_back(last);
    }
    return withData(e, std::move(out));
}

Result<Entity> splineToPolyline(const Entity& e, double tolerance) {
    if (!entityAs<SplineData>(e))
        return err("Select a spline.");
    std::vector<CurveRef> curves;
    if (!geom::curvesOf(e, curves) || curves.empty())
        return err("The spline has no geometry.");
    std::vector<Vec2> pts;
    for (const CurveRef& c : curves) {
        std::vector<Vec2> part;
        geom::flatten(c, std::max(tolerance, 1e-6), part);
        for (Vec2 q : part) {
            if (pts.empty() || !samePoint(pts.back(), q, 1e-12))
                pts.push_back(q);
        }
    }
    PolylineData pl;
    pl.closed = pts.size() > 2 && samePoint(pts.front(), pts.back(), 1e-9);
    if (pl.closed)
        pts.pop_back();
    for (Vec2 q : pts)
        pl.v.push_back({q, 0, 0, 0});
    return withData(e, std::move(pl));
}

Result<EntityEdit> setPolylineWidth(const Entity& e, double width) {
    const auto* p = entityAs<PolylineData>(e);
    if (!p)
        return err("Select a polyline.");
    if (width < 0)
        return err("The width cannot be negative.");
    PolylineData out = *p;
    out.constantWidth = width;
    for (PolyVertex& v : out.v)
        v.startWidth = v.endWidth = width;
    EntityEdit r;
    r.modified = withData(e, std::move(out));
    return r;
}

// ---- EXTRIM -------------------------------------------------------------------------------------

namespace {

bool insidePolygon(const std::vector<Vec2>& poly, Vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 &a = poly[i], &b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

// Which side of the edge a point is on: +1 / -1, or 0 for "on the edge". Fails for edges without
// two sides.
std::optional<std::function<int(Vec2)>> sideFunction(const Entity& edge) {
    if (const auto* l = entityAs<LineData>(edge)) {
        if (samePoint(l->a, l->b))
            return std::nullopt;
        Vec2 a = l->a, d = l->b - l->a;
        return std::function<int(Vec2)>([a, d](Vec2 p) {
            double c = cross(d, p - a);
            return std::abs(c) < 1e-12 ? 0 : (c > 0 ? 1 : -1);
        });
    }
    auto radial = [](Vec2 c, double r) {
        return std::function<int(Vec2)>([c, r](Vec2 p) {
            double d = distance(p, c) - r;
            return std::abs(d) < 1e-12 ? 0 : (d > 0 ? 1 : -1);
        });
    };
    if (const auto* c = entityAs<CircleData>(edge))
        return radial(c->center, c->radius);
    if (const auto* a = entityAs<ArcData>(edge))
        return radial(a->center, a->radius);
    if (const auto* p = entityAs<PolylineData>(edge); p && p->closed) {
        std::vector<Vec2> pts;
        bool closed = false;
        if (!pathOf(edge, pts, closed) || pts.size() < 3)
            return std::nullopt;
        return std::function<int(Vec2)>([pts](Vec2 q) { return insidePolygon(pts, q) ? -1 : 1; });
    }
    return std::nullopt;
}

// Midpoints of the pieces of `e` that its crossings with the edge cut out.
std::vector<Vec2> pieceMidpoints(const Entity& e, const std::vector<CurveRef>& edgeCurves) {
    std::vector<Vec2> mids;
    auto built = buildPath(e);
    if (!built)
        return mids;
    const auto& [path, closed] = built.value();
    std::vector<CurveRef> mine;
    if (!geom::curvesOf(e, mine))
        return mids;
    std::vector<double> cuts;
    for (const CurveRef& a : mine) {
        for (const CurveRef& b : edgeCurves) {
            std::vector<geom::Intersection> xs;
            geom::intersect(a, b, xs, false, true);
            for (const auto& x : xs) {
                // Arclength of the closest path vertex pair projection.
                double best = 1e300, bs = 0;
                for (std::size_t i = 1; i < path.pts.size(); ++i) {
                    Vec2 d = path.pts[i] - path.pts[i - 1];
                    double l2 = dot(d, d);
                    double u =
                        l2 > 0 ? std::clamp(dot(x.p - path.pts[i - 1], d) / l2, 0.0, 1.0) : 0.0;
                    double dist = distance(path.pts[i - 1] + d * u, x.p);
                    if (dist < best) {
                        best = dist;
                        bs = path.cum[i - 1] + u * std::sqrt(l2);
                    }
                }
                cuts.push_back(bs);
            }
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(),
                           [](double a, double b) { return std::abs(a - b) < 1e-7; }),
               cuts.end());
    if (cuts.empty())
        return mids;
    double len = path.length();
    std::vector<double> bounds;
    if (!closed)
        bounds.push_back(0.0);
    for (double c : cuts)
        bounds.push_back(c);
    if (!closed)
        bounds.push_back(len);
    for (std::size_t i = 0; i + 1 < bounds.size(); ++i) {
        if (bounds[i + 1] - bounds[i] > 1e-7)
            mids.push_back(path.at(0.5 * (bounds[i] + bounds[i + 1])));
    }
    if (closed && cuts.size() >= 1) {
        double a = cuts.back(), b = cuts.front() + len;
        if (b - a > 1e-7) {
            double m = 0.5 * (a + b);
            mids.push_back(path.at(m > len ? m - len : m));
        }
    }
    return mids;
}

}  // namespace

Result<EntityEdit> extrimEntity(const Entity& target, const Entity& edge, Vec2 side) {
    auto sideOf = sideFunction(edge);
    if (!sideOf)
        return err("The cutting edge must be a line, circle, arc or closed polyline.");
    int want = (*sideOf)(side);
    if (want == 0)
        return err("Pick a point that is not on the cutting edge.");
    std::vector<CurveRef> edgeCurves;
    if (!geom::curvesOf(edge, edgeCurves) || edgeCurves.empty())
        return err("The cutting edge has no geometry.");

    std::vector<Entity> work{target};
    std::vector<Entity> done;
    bool changed = false;
    for (int guard = 0; !work.empty() && guard < 256; ++guard) {
        Entity cur = std::move(work.back());
        work.pop_back();
        std::optional<Vec2> doomed;
        for (Vec2 m : pieceMidpoints(cur, edgeCurves)) {
            if ((*sideOf)(m) == want) {
                doomed = m;
                break;
            }
        }
        if (!doomed) {
            done.push_back(std::move(cur));
            continue;
        }
        auto edit = trimEntity(cur, *doomed, {&edge}, TrimOptions{true});
        if (!edit)
            return edit.error();
        changed = true;
        if (edit.value().eraseSource)
            continue;
        if (edit.value().modified)
            work.push_back(std::move(*edit.value().modified));
        for (Entity& a : edit.value().added)
            work.push_back(std::move(a));
    }
    EntityEdit r;
    if (!changed)
        return r;  // nothing on that side
    if (done.empty()) {
        r.eraseSource = true;
        return r;
    }
    r.modified = std::move(done.front());
    for (std::size_t i = 1; i < done.size(); ++i)
        r.added.push_back(std::move(done[i]));
    return r;
}

// ---- BLEND --------------------------------------------------------------------------------------

namespace {

struct EndInfo {
    Vec2 p{};
    Vec2 out{};  // unit tangent leaving the curve at this end
};

Result<EndInfo> endNearest(const Entity& e, Vec2 pick) {
    if (auto segs = openSegs(e)) {
        bool atEnd = nearerToEnd(*segs, pick);
        if (atEnd) {
            const Seg& s = segs->back();
            return EndInfo{s.p1, segTangent(s, 1.0)};
        }
        const Seg& s = segs->front();
        Vec2 t = segTangent(s, 0.0);
        return EndInfo{s.p0, t * -1.0};
    }
    const auto* sp = entityAs<SplineData>(e);
    const auto* el = entityAs<EllipseData>(e);
    const bool openSpline = sp && !sp->closed && !sp->periodic;
    const bool openEllipse = el && normalizeAngle(el->endParam - el->startParam) > 1e-12;
    if (openSpline || openEllipse) {
        std::vector<CurveRef> cs;
        if (geom::curvesOf(e, cs) && !cs.empty()) {
            const CurveRef& f = cs.front();
            const CurveRef& l = cs.back();
            double t0 = f.t0(), t1 = l.t0() + l.sweep();
            Vec2 a = f.pointAt(t0), b = l.pointAt(t1);
            if (distance(pick, b) < distance(pick, a))
                return EndInfo{b, normalized(l.tangentAt(t1))};
            return EndInfo{a, normalized(f.tangentAt(t0)) * -1.0};
        }
    }
    return err("BLEND works on lines, arcs, elliptical arcs and open polylines and splines.");
}

}  // namespace

Result<Entity> blendCurves(const Entity& a, Vec2 pickA, const Entity& b, Vec2 pickB) {
    auto ea = endNearest(a, pickA);
    if (!ea)
        return ea.error();
    auto eb = endNearest(b, pickB);
    if (!eb)
        return eb.error();
    double chord = distance(ea.value().p, eb.value().p);
    if (chord < 1e-6)
        return err("The two ends already meet.");
    geom::InterpolateOptions io;
    io.startTangent = ea.value().out * chord;
    io.endTangent = eb.value().out * -chord;
    // A cubic needs a third point to honour both end tangents: the Hermite curve's midpoint.
    Vec2 mid = (ea.value().p + eb.value().p) * 0.5 +
               (io.startTangent.value() - io.endTangent.value()) * 0.125;
    Vec2 pts[3] = {ea.value().p, mid, eb.value().p};
    auto sp = geom::interpolateSpline(pts, io);
    if (!sp)
        return err("The blend could not be fitted: " + sp.error().message);
    return withData(a, Indirect<SplineData>(std::move(sp.value())));
}

// ---- PEDIT vertices -----------------------------------------------------------------------------

namespace {

std::size_t nearestVertex(const PolylineData& p, Vec2 q) {
    std::size_t best = 0;
    double bd = 1e300;
    for (std::size_t i = 0; i < p.v.size(); ++i) {
        double d = distance(p.v[i].p, q);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

}  // namespace

Result<EntityEdit> insertPolylineVertex(const Entity& e, Vec2 p) {
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return err("Select a polyline.");
    PolylineData clean = cleanPolyline(*pl);
    std::vector<Seg> segs = polylineSegs(clean);
    if (segs.empty())
        return err("The polyline has no segments.");
    double g = closestGlobalParam(segs, p);
    auto i = static_cast<std::size_t>(std::min<double>(std::floor(g), segs.size() - 1.0));
    double u = std::clamp(g - static_cast<double>(i), 0.0, 1.0);
    if (u < 1e-9 || u > 1.0 - 1e-9)
        return err("There is already a vertex there.");
    std::vector<Seg> chain(segs.begin(), segs.begin() + static_cast<std::ptrdiff_t>(i));
    chain.push_back(segSub(segs[i], 0.0, u));
    chain.push_back(segSub(segs[i], u, 1.0));
    chain.insert(chain.end(), segs.begin() + static_cast<std::ptrdiff_t>(i) + 1, segs.end());
    EntityEdit r;
    r.modified = withData(e, polylineFromSegs(chain, clean.closed, &clean));
    return r;
}

Result<EntityEdit> removePolylineVertex(const Entity& e, Vec2 near) {
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return err("Select a polyline.");
    std::size_t minCount = pl->closed ? 4 : 3;
    if (pl->v.size() < minCount)
        return err("The polyline would have too few vertices.");
    PolylineData out = *pl;
    std::size_t i = nearestVertex(out, near);
    if (pl->closed || i > 0) {
        // The segment that ended here and the one that started here become one straight segment.
        std::size_t prev = (i + out.v.size() - 1) % out.v.size();
        out.v[prev].bulge = 0;
    }
    out.v.erase(out.v.begin() + static_cast<std::ptrdiff_t>(i));
    EntityEdit r;
    r.modified = withData(e, std::move(out));
    return r;
}

Result<EntityEdit> movePolylineVertex(const Entity& e, Vec2 near, Vec2 to) {
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return err("Select a polyline.");
    PolylineData out = *pl;
    out.v[nearestVertex(out, near)].p = to;
    EntityEdit r;
    r.modified = withData(e, std::move(out));
    return r;
}

Result<EntityEdit> setPolylineVertexWidth(const Entity& e, Vec2 near, double startWidth,
                                          double endWidth) {
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return err("Select a polyline.");
    if (startWidth < 0 || endWidth < 0)
        return err("The width cannot be negative.");
    PolylineData out = *pl;
    const std::size_t i = nearestVertex(out, near);
    if (!out.closed && i + 1 >= out.v.size())
        return err("The last vertex of an open polyline starts no segment.");
    out.v[i].startWidth = startWidth;
    out.v[i].endWidth = endWidth;
    EntityEdit r;
    r.modified = withData(e, std::move(out));
    return r;
}

Result<EntityEdit> straightenPolyline(const Entity& e, Vec2 a, Vec2 b) {
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return err("Select a polyline.");
    std::size_t ia = nearestVertex(*pl, a), ib = nearestVertex(*pl, b);
    if (ia == ib)
        return err("Pick two different vertices.");
    if (ia > ib)
        std::swap(ia, ib);
    PolylineData out = *pl;
    out.v[ia].bulge = 0;
    // Widths of the dropped stretch go with it; the new segment takes the first one's.
    out.v.erase(out.v.begin() + static_cast<std::ptrdiff_t>(ia) + 1,
                out.v.begin() + static_cast<std::ptrdiff_t>(ib));
    if (out.v.size() < 2 || (out.closed && out.v.size() < 2))
        return err("Nothing would be left of the polyline.");
    EntityEdit r;
    r.modified = withData(e, std::move(out));
    return r;
}

Result<EntityEdit> taperPolyline(const Entity& e, double startWidth, double endWidth) {
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return err("Select a polyline.");
    if (startWidth < 0 || endWidth < 0)
        return err("The width cannot be negative.");
    PolylineData out = *pl;
    const std::vector<Seg> segs = polylineSegs(*pl);
    double total = 0;
    for (const Seg& s : segs)
        total += segLength(s);
    if (total <= tol::kLinear || segs.size() != (pl->closed ? pl->v.size() : pl->v.size() - 1))
        return err("The polyline has no length to taper over.");
    double run = 0;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const double w0 = startWidth + (endWidth - startWidth) * (run / total);
        run += segLength(segs[i]);
        const double w1 = startWidth + (endWidth - startWidth) * (run / total);
        out.v[i].startWidth = w0;
        out.v[i].endWidth = w1;
    }
    out.constantWidth = 0;
    EntityEdit r;
    r.modified = withData(e, std::move(out));
    return r;
}

// ---- SPLINEDIT ----------------------------------------------------------------------------------

Result<EntityEdit> editSplineFit(const Entity& e, SplineFitOp op, Vec2 a, Vec2 b) {
    const auto* sp = entityAs<SplineData>(e);
    if (!sp)
        return err("Select a spline.");
    if (sp->fit.size() < 2)
        return err("This spline has no fit points to edit.");
    std::vector<Vec2> fit = sp->fit;
    bool closed = sp->closed;
    auto nearest = [&](Vec2 q) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < fit.size(); ++i) {
            if (distance(fit[i], q) < distance(fit[best], q))
                best = i;
        }
        return best;
    };
    switch (op) {
        case SplineFitOp::Add: {
            std::vector<CurveRef> cs;
            if (!geom::curvesOf(e, cs) || cs.empty())
                return err("The spline has no geometry.");
            Vec2 on = cs.front().closestPoint(a);
            std::size_t segCount = closed ? fit.size() : fit.size() - 1;
            std::size_t at = 0;
            double bd = 1e300;
            for (std::size_t i = 0; i < segCount; ++i) {
                Vec2 p0 = fit[i], p1 = fit[(i + 1) % fit.size()];
                Vec2 d = p1 - p0;
                double l2 = dot(d, d);
                double u = l2 > 0 ? std::clamp(dot(on - p0, d) / l2, 0.0, 1.0) : 0.0;
                double dist = distance(p0 + d * u, on);
                if (dist < bd) {
                    bd = dist;
                    at = i;
                }
            }
            fit.insert(fit.begin() + static_cast<std::ptrdiff_t>(at) + 1, on);
            break;
        }
        case SplineFitOp::Delete:
            if (fit.size() <= (closed ? 3u : 2u))
                return err("The spline would have too few fit points.");
            fit.erase(fit.begin() + static_cast<std::ptrdiff_t>(nearest(a)));
            break;
        case SplineFitOp::Move:
            fit[nearest(a)] = b;
            break;
        case SplineFitOp::Close:
            if (fit.size() < 3)
                return err("A closed spline needs at least three fit points.");
            closed = true;
            break;
        case SplineFitOp::Open:
            closed = false;
            break;
    }
    geom::InterpolateOptions io;
    io.degree = sp->degree;
    io.closed = closed;
    auto fitted = geom::interpolateSpline(fit, io);
    if (!fitted)
        return err("The spline could not be fitted: " + fitted.error().message);
    EntityEdit r;
    r.modified = withData(e, Indirect<SplineData>(std::move(fitted.value())));
    return r;
}

namespace {

// Squared distance from a point to the nearest segment of a polyline, through a uniform grid of
// the segments: a ring of cells at a time, stopping once the next ring is farther than the best.
class PolylineGrid {
public:
    explicit PolylineGrid(const std::vector<Vec2>& pts) : pts_(pts) {
        lo_ = hi_ = pts.empty() ? Vec2{0, 0} : pts.front();
        for (const Vec2& p : pts) {
            lo_ = {std::min(lo_.x, p.x), std::min(lo_.y, p.y)};
            hi_ = {std::max(hi_.x, p.x), std::max(hi_.y, p.y)};
        }
        const double ext = std::max(hi_.x - lo_.x, hi_.y - lo_.y);
        const double segs = static_cast<double>(pts.size() > 1 ? pts.size() - 1 : 1);
        n_ = std::clamp(static_cast<int>(std::sqrt(segs)), 1, 128);
        cell_ = ext > 0 ? ext / n_ : 1.0;
        cells_.assign(static_cast<std::size_t>(n_) * static_cast<std::size_t>(n_), {});
        for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
            const Vec2 a = pts[k], b = pts[k + 1];
            const int x0 = cellOf(std::min(a.x, b.x), lo_.x), x1 = cellOf(std::max(a.x, b.x), lo_.x);
            const int y0 = cellOf(std::min(a.y, b.y), lo_.y), y1 = cellOf(std::max(a.y, b.y), lo_.y);
            for (int cy = y0; cy <= y1; ++cy)
                for (int cx = x0; cx <= x1; ++cx)
                    cells_[static_cast<std::size_t>(cy * n_ + cx)].push_back(
                        static_cast<std::uint32_t>(k));
        }
    }

    double nearestDist2(Vec2 q) const {
        double best = 1e300;
        const int qx = cellOf(q.x, lo_.x), qy = cellOf(q.y, lo_.y);
        for (int r = 0; r < n_ + 1; ++r) {
            for (int cy = qy - r; cy <= qy + r; ++cy) {
                if (cy < 0 || cy >= n_)
                    continue;
                const bool edgeRow = cy == qy - r || cy == qy + r;
                for (int cx = qx - r; cx <= qx + r; cx += edgeRow || r == 0 ? 1 : 2 * r) {
                    if (cx < 0 || cx >= n_)
                        continue;
                    for (std::uint32_t k : cells_[static_cast<std::size_t>(cy * n_ + cx)]) {
                        const Vec2 a = pts_[k], b = pts_[k + 1], ab = b - a;
                        const double l2 = dot(ab, ab);
                        const double u = l2 > 0 ? std::clamp(dot(q - a, ab) / l2, 0.0, 1.0) : 0.0;
                        const Vec2 d = q - (a + ab * u);
                        best = std::min(best, dot(d, d));
                    }
                }
            }
            if (best < 1e300 && r * cell_ >= std::sqrt(best))
                break;
        }
        return best;
    }

private:
    int cellOf(double v, double lo) const {
        return std::clamp(static_cast<int>(std::floor((v - lo) / cell_)), 0, n_ - 1);
    }

    const std::vector<Vec2>& pts_;
    Vec2 lo_, hi_;
    double cell_ = 1;
    int n_ = 1;
    std::vector<std::vector<std::uint32_t>> cells_;
};

}  // namespace

Result<EntityEdit> refitSpline(const Entity& e, double tolerance) {
    const auto* sp = entityAs<SplineData>(e);
    if (!sp)
        return err("Select a spline.");
    if (!(tolerance > 0) || !std::isfinite(tolerance))
        return err("The tolerance must be more than zero.");
    std::vector<CurveRef> cs;
    if (!geom::curvesOf(e, cs) || cs.empty())
        return err("The spline has no geometry.");
    const CurveRef& c = cs.front();
    const double len = c.length();
    if (!(len > 0))
        return err("The spline has no length.");
    const int n = static_cast<int>(std::clamp(len / tolerance, 200.0, 2000.0));
    std::vector<Vec2> samples;
    samples.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i)
        samples.push_back(c.pointAt(c.paramAtLength(len * i / n)));
    const bool closed = sp->closed;
    std::vector<int> idx =
        closed ? std::vector<int>{0, n / 4, n / 2, 3 * n / 4} : std::vector<int>{0, n};
    geom::InterpolateOptions io;
    io.degree = sp->degree;
    io.closed = closed;
    SplineData best;
    for (int round = 0; round < 500; ++round) {
        std::vector<Vec2> fit;
        for (int i : idx)
            fit.push_back(samples[static_cast<std::size_t>(i)]);
        auto fitted = geom::interpolateSpline(fit, io);
        if (!fitted)
            return err("The spline could not be fitted: " + fitted.error().message);
        best = std::move(fitted.value());
        const Entity trial = withData(e, Indirect<SplineData>(best));
        std::vector<CurveRef> nc;
        if (!geom::curvesOf(trial, nc) || nc.empty())
            return err("The new spline has no geometry.");
        const double nl = nc.front().length();
        const int m =
            static_cast<int>(std::clamp(static_cast<double>(idx.size()) * 24.0, 100.0, 4000.0));
        std::vector<Vec2> poly;
        poly.reserve(static_cast<std::size_t>(m) + 1);
        for (int i = 0; i <= m; ++i)
            poly.push_back(nc.front().pointAt(nc.front().paramAtLength(nl * i / m)));
        const PolylineGrid grid(poly);
        double worst = 0;
        int worstAt = -1;
        for (int i = 0; i <= n; ++i) {
            const double d2 = grid.nearestDist2(samples[static_cast<std::size_t>(i)]);
            if (d2 > worst) {
                worst = d2;
                worstAt = i;
            }
        }
        if (worstAt < 0 || std::sqrt(worst) <= tolerance * 0.9 || idx.size() >= samples.size())
            break;
        idx.push_back(worstAt);
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    }
    EntityEdit r;
    r.modified = withData(e, Indirect<SplineData>(std::move(best)));
    return r;
}

// ---- SPLINEDIT: control vertices ----------------------------------------------------------------

namespace {

// Clamped (or, when closed, periodic) uniform B-spline over the control vertices, as SPLINE CV.
std::optional<SplineData> controlPolygonSpline(const std::vector<Vec2>& pts,
                                               const std::vector<double>& weights, bool closed,
                                               int wantDegree) {
    const int n = static_cast<int>(pts.size());
    if (n < 2)
        return std::nullopt;
    SplineData d;
    d.degree = std::min(wantDegree, n - 1);
    d.control = pts;
    if (!weights.empty())
        d.weights = weights;
    if (closed) {
        if (n < 3)
            return std::nullopt;
        for (int i = 0; i < d.degree; ++i) {
            d.control.push_back(pts[static_cast<std::size_t>(i)]);
            if (!weights.empty())
                d.weights.push_back(weights[static_cast<std::size_t>(i)]);
        }
        const int count = static_cast<int>(d.control.size()) + d.degree + 1;
        for (int i = 0; i < count; ++i)
            d.knots.push_back(i);
        d.closed = true;
        d.periodic = true;
        return d;
    }
    for (int i = 0; i <= d.degree; ++i)
        d.knots.push_back(0);
    for (int i = 1; i <= n - d.degree - 1; ++i)
        d.knots.push_back(i);
    for (int i = 0; i <= d.degree; ++i)
        d.knots.push_back(n - d.degree);
    return d;
}

bool validKnots(const SplineData& d) {
    return d.degree >= 1 && d.control.size() >= static_cast<std::size_t>(d.degree) + 1 &&
           d.knots.size() == d.control.size() + static_cast<std::size_t>(d.degree) + 1 &&
           (d.weights.empty() || d.weights.size() == d.control.size());
}

}  // namespace

Result<EntityEdit> editSplineCv(const Entity& e, SplineCvOp op, Vec2 a, Vec2 b, double value) {
    const auto* sp = entityAs<SplineData>(e);
    if (!sp)
        return err("Select a spline.");
    SplineData d = *sp;
    if (d.control.empty() && d.fit.size() >= 2) {
        // A fit-only spline: its control vertices come from the fit.
        auto fitted = geom::interpolateSpline(d.fit, geom::InterpolateOptions{.degree = d.degree});
        if (!fitted)
            return err("The spline could not be fitted.");
        d = fitted.value();
    }
    if (!validKnots(d))
        return err("This spline has no control vertices to edit.");
    const std::size_t p = static_cast<std::size_t>(d.degree);
    // Vertices the user sees: a periodic spline repeats its first `degree` vertices at the end.
    const std::size_t shown = d.periodic ? d.control.size() - p : d.control.size();
    auto nearest = [&](Vec2 q) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < shown; ++i) {
            if (distance(d.control[i], q) < distance(d.control[best], q))
                best = i;
        }
        return best;
    };
    EntityEdit r;
    auto done = [&](SplineData out) {
        r.modified = withData(e, Indirect<SplineData>(std::move(out)));
        return r;
    };
    switch (op) {
        case SplineCvOp::Purge:
            if (d.fit.empty())
                return err("This spline has no fit points.");
            d.fit.clear();
            return done(std::move(d));
        case SplineCvOp::Move: {
            const std::size_t i = nearest(a);
            d.control[i] = b;
            if (d.periodic && i < p)
                d.control[shown + i] = b;
            d.fit.clear();
            return done(std::move(d));
        }
        case SplineCvOp::Weight: {
            if (!(value > 0) || !std::isfinite(value))
                return err("The weight must be a positive number.");
            if (d.weights.empty())
                d.weights.assign(d.control.size(), 1.0);
            const std::size_t i = nearest(a);
            d.weights[i] = value;
            if (d.periodic && i < p)
                d.weights[shown + i] = value;
            d.fit.clear();
            return done(std::move(d));
        }
        case SplineCvOp::Close:
        case SplineCvOp::Open: {
            const bool wantClosed = op == SplineCvOp::Close;
            if (d.closed == wantClosed)
                return err(wantClosed ? "The spline is already closed."
                                      : "The spline is already open.");
            std::vector<Vec2> pts(d.control.begin(),
                                  d.control.begin() + static_cast<std::ptrdiff_t>(shown));
            std::vector<double> w;
            if (!d.weights.empty())
                w.assign(d.weights.begin(), d.weights.begin() + static_cast<std::ptrdiff_t>(shown));
            auto out = controlPolygonSpline(pts, w, wantClosed, d.degree);
            if (!out)
                return err("A closed spline needs at least three control vertices.");
            return done(std::move(*out));
        }
        case SplineCvOp::ToPolyline: {
            std::vector<CurveRef> cs;
            if (!geom::curvesOf(e, cs) || cs.empty())
                return err("The spline has no geometry.");
            std::vector<Vec2> pts;
            geom::flatten(cs[0], value > 0 ? value : 0.1, pts);
            if (d.closed && pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9)
                pts.pop_back();
            if (pts.size() < 2)
                return err("The spline is too short to convert.");
            PolylineData pl;
            pl.closed = d.closed;
            for (Vec2 q : pts) {
                PolyVertex v;
                v.p = q;
                pl.v.push_back(v);
            }
            r.modified = withData(e, std::move(pl));
            return r;
        }
        case SplineCvOp::Add: {
            if (d.periodic) {
                // Wrapped vertices make an in-place insertion ambiguous: put the new vertex
                // halfway along the polygon edge nearest the point.
                std::size_t at = 0;
                double bd = 1e300;
                for (std::size_t i = 0; i < shown; ++i) {
                    Vec2 p0 = d.control[i], p1 = d.control[(i + 1) % shown];
                    Vec2 dir = p1 - p0;
                    double l2 = dot(dir, dir);
                    double u = l2 > 0 ? std::clamp(dot(a - p0, dir) / l2, 0.0, 1.0) : 0.0;
                    double dist = distance(p0 + dir * u, a);
                    if (dist < bd) {
                        bd = dist;
                        at = i;
                    }
                }
                std::vector<Vec2> pts(d.control.begin(),
                                      d.control.begin() + static_cast<std::ptrdiff_t>(shown));
                std::vector<double> w;
                if (!d.weights.empty())
                    w.assign(d.weights.begin(),
                             d.weights.begin() + static_cast<std::ptrdiff_t>(shown));
                const Vec2 mid = (pts[at] + pts[(at + 1) % shown]) * 0.5;
                pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(at) + 1, mid);
                if (!w.empty())
                    w.insert(w.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                             0.5 * (w[at] + w[(at + 1) % shown]));
                auto out = controlPolygonSpline(pts, w, true, d.degree);
                if (!out)
                    return err("The vertex could not be added.");
                return done(std::move(*out));
            }
            auto curve = geom::CurveRef::spline(d);
            if (!curve)
                return err("The spline has no geometry.");
            double u = curve.value().paramAt(a);
            const std::size_t n = d.control.size();
            // Span k with knots[k] <= u < knots[k+1], k in [p, n-1].
            std::size_t k = p;
            while (k + 1 < n && d.knots[k + 1] <= u)
                ++k;
            if (u <= d.knots[p] + 1e-9 || u >= d.knots[n] - 1e-9)
                return err("The point is at an end of the spline.");
            if (std::abs(u - d.knots[k]) < 1e-9)
                return err("There is already a vertex at that position.");
            const bool rational = !d.weights.empty();
            auto hom = [&](std::size_t i) {
                const double w = rational ? d.weights[i] : 1.0;
                return std::array<double, 3>{d.control[i].x * w, d.control[i].y * w, w};
            };
            std::vector<Vec2> Q;
            std::vector<double> W;
            Q.reserve(n + 1);
            for (std::size_t i = 0; i <= n; ++i) {
                std::array<double, 3> h;
                if (i + p <= k) {  // i <= k - p
                    h = hom(i);
                } else if (i > k) {
                    h = hom(i - 1);
                } else {
                    const double alpha = (u - d.knots[i]) / (d.knots[i + p] - d.knots[i]);
                    auto h0 = hom(i - 1), h1 = hom(i);
                    for (int c = 0; c < 3; ++c)
                        h[static_cast<std::size_t>(c)] =
                            (1 - alpha) * h0[static_cast<std::size_t>(c)] +
                            alpha * h1[static_cast<std::size_t>(c)];
                }
                Q.push_back({h[0] / h[2], h[1] / h[2]});
                W.push_back(h[2]);
            }
            d.control = std::move(Q);
            if (rational)
                d.weights = std::move(W);
            d.knots.insert(d.knots.begin() + static_cast<std::ptrdiff_t>(k) + 1, u);
            return done(std::move(d));
        }
        case SplineCvOp::Delete: {
            if (d.periodic) {
                std::vector<Vec2> pts(d.control.begin(),
                                      d.control.begin() + static_cast<std::ptrdiff_t>(shown));
                std::vector<double> w;
                if (!d.weights.empty())
                    w.assign(d.weights.begin(),
                             d.weights.begin() + static_cast<std::ptrdiff_t>(shown));
                if (pts.size() <= 3)
                    return err("A closed spline needs at least three control vertices.");
                const std::size_t i = nearest(a);
                pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i));
                if (!w.empty())
                    w.erase(w.begin() + static_cast<std::ptrdiff_t>(i));
                auto out = controlPolygonSpline(pts, w, true, d.degree);
                if (!out)
                    return err("The vertex could not be deleted.");
                return done(std::move(*out));
            }
            const std::size_t n = d.control.size();
            if (n < p + 2)
                return err("The spline would have too few control vertices.");
            const std::size_t i = nearest(a);
            d.control.erase(d.control.begin() + static_cast<std::ptrdiff_t>(i));
            if (!d.weights.empty())
                d.weights.erase(d.weights.begin() + static_cast<std::ptrdiff_t>(i));
            // Drop an interior knot next to the vertex so the ends stay clamped.
            const std::size_t kn = std::clamp<std::size_t>(i + 1, p + 1, n - 1);
            d.knots.erase(d.knots.begin() + static_cast<std::ptrdiff_t>(kn));
            d.fit.clear();
            return done(std::move(d));
        }
    }
    return err("Unsupported spline edit.");
}

// ---- FIND / replace ----------------------------------------------------------------------------

namespace {

char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

int replaceAllIn(std::string& text, std::string_view find, std::string_view repl, bool matchCase) {
    if (find.empty() || text.size() < find.size())
        return 0;
    auto same = [&](std::size_t at) {
        for (std::size_t k = 0; k < find.size(); ++k) {
            char a = text[at + k], b = find[k];
            if (matchCase ? a != b : lowerAscii(a) != lowerAscii(b))
                return false;
        }
        return true;
    };
    std::string out;
    int n = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (i + find.size() <= text.size() && same(i)) {
            out += repl;
            i += find.size();
            ++n;
        } else {
            out += text[i++];
        }
    }
    if (n)
        text = std::move(out);
    return n;
}

// Find and replace in MTEXT markup. The search runs over the text as it is read (formatting codes
// skipped, `\\ \{ \}` as the characters they stand for, `\P` as a newline), so a word is found
// even where bold or a colour starts in the middle of it, and codes are never matched or damaged.
// The replacement is inserted where the match starts (it takes that piece's formatting) and the
// codes inside the match stay, so the pieces around it keep their look. Special characters in
// the replacement are escaped.
int replaceInMarkup(std::string& markup, std::string_view find, std::string_view repl,
                    bool matchCase) {
    if (find.empty())
        return 0;
    struct Atom {
        std::size_t begin = 0, end = 0;  // the atom's bytes in `markup`
        char ch = 0;                     // its character; 0 for a formatting code
    };
    std::vector<Atom> atoms;
    for (std::size_t i = 0; i < markup.size();) {
        const char c = markup[i];
        if (c == '\\' && i + 1 < markup.size()) {
            const char k = markup[i + 1];
            if (k == '\\' || k == '{' || k == '}') {
                atoms.push_back({i, i + 2, k});
                i += 2;
            } else if (k == 'P') {
                atoms.push_back({i, i + 2, '\n'});
                i += 2;
            } else if (k == 'b' || k == 'i' || k == 'H' || k == 'C' || k == 'S') {
                const std::size_t semi = markup.find(';', i + 2);
                if (semi == std::string::npos) {
                    atoms.push_back({i, i + 1, c});  // no ';': a literal backslash
                    ++i;
                } else {
                    atoms.push_back({i, semi + 1, 0});
                    i = semi + 1;
                }
            } else if (k == 'L' || k == 'l' || k == 'O' || k == 'o') {
                atoms.push_back({i, i + 2, 0});
                i += 2;
            } else {
                atoms.push_back({i, i + 1, c});
                ++i;
            }
        } else if (c == '{' || c == '}') {
            atoms.push_back({i, i + 1, 0});
            ++i;
        } else {
            atoms.push_back({i, i + 1, c});
            ++i;
        }
    }
    std::vector<std::size_t> text;  // indexes of the character atoms, in reading order
    for (std::size_t a = 0; a < atoms.size(); ++a)
        if (atoms[a].ch != 0)
            text.push_back(a);
    if (text.size() < find.size())
        return 0;
    std::string safe;
    for (char c : repl) {
        if (c == '\\' || c == '{' || c == '}')
            safe += '\\';
        if (c == '\n') {
            safe += "\\P";
            continue;
        }
        safe += c;
    }
    std::vector<bool> drop(atoms.size(), false);
    std::vector<std::string> before(atoms.size());  // the replacement goes before this atom
    int n = 0;
    for (std::size_t t = 0; t + find.size() <= text.size();) {
        bool same = true;
        for (std::size_t k = 0; k < find.size() && same; ++k) {
            const char a = atoms[text[t + k]].ch, b = find[k];
            same = matchCase ? a == b : lowerAscii(a) == lowerAscii(b);
        }
        if (!same) {
            ++t;
            continue;
        }
        before[text[t]] = safe;
        for (std::size_t k = 0; k < find.size(); ++k)
            drop[text[t + k]] = true;
        t += find.size();
        ++n;
    }
    if (n == 0)
        return 0;
    std::string out;
    for (std::size_t a = 0; a < atoms.size(); ++a) {
        out += before[a];
        if (!drop[a])
            out.append(markup, atoms[a].begin, atoms[a].end - atoms[a].begin);
    }
    markup = std::move(out);
    return n;
}

}  // namespace

std::optional<TextReplace> replaceText(const Entity& e, std::string_view find,
                                       std::string_view replacement, bool matchCase) {
    TextReplace r{e, 0};
    auto edit = [&](std::string& s) { r.count += replaceAllIn(s, find, replacement, matchCase); };
    auto editMarkup = [&](std::string& s) {
        r.count += replaceInMarkup(s, find, replacement, matchCase);
    };
    if (auto* t = entityAs<TextData>(r.entity))
        edit(t->utf8);
    else if (auto* m = entityAs<MTextData>(r.entity))
        editMarkup(m->utf8);
    else if (auto* l = entityAs<LeaderData>(r.entity))
        edit(l->utf8);
    else if (auto* ml = entityAs<MLeaderData>(r.entity))
        edit(ml->utf8);
    else if (auto* d = entityAs<DimensionData>(r.entity))
        edit(d->textOverride);
    else if (auto* ins = entityAs<InsertData>(r.entity))
        for (AttribValue& a : ins->attribs)
            edit(a.value);
    else if (auto* tb = entityAs<TableData>(r.entity))
        for (TableCell& c : tb->cells)
            editMarkup(c.utf8);
    if (r.count == 0)
        return std::nullopt;
    return r;
}

}  // namespace tbcad::modify

#include "tbcad/modify/trim.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "tbcad/geom/box_grid.h"

namespace tbcad::modify {

using geom::CurveRef;

namespace {

Error err(std::string m) {
    return Error{ErrorCode::InvalidArgument, std::move(m)};
}

std::vector<CurveRef> edgeCurves(const std::vector<const Entity*>& edges) {
    std::vector<CurveRef> out;
    for (const Entity* e : edges) {
        if (e)
            geom::curvesOf(*e, out);
    }
    return out;
}

// The edges that can meet a box. With many edges (a long polyline used as the cutting edge) a grid
// answers without testing them all; an extending edge has no useful box, so then all are returned.
class EdgeNear {
public:
    EdgeNear(const std::vector<CurveRef>& edges, bool extend)
        : edges_(edges), all_(extend || edges.size() <= 64) {
        if (all_)
            return;
        boxes_.reserve(edges.size());
        for (const CurveRef& e : edges)
            boxes_.push_back(e.bounds().inflated(1e-6));
        grid_ = std::make_unique<geom::BoxGrid>(boxes_);
    }
    template <class F>
    void forEach(const BBox2& q, F&& f) const {
        if (all_) {
            for (const CurveRef& e : edges_)
                f(e);
            return;
        }
        grid_->query(q, [&](std::size_t i) { f(edges_[i]); });
    }

private:
    const std::vector<CurveRef>& edges_;
    bool all_;
    std::vector<BBox2> boxes_;
    std::unique_ptr<geom::BoxGrid> grid_;
};

void sortUnique(std::vector<double>& v, double eps = 1e-9) {
    std::sort(v.begin(), v.end());
    std::vector<double> out;
    for (double x : v) {
        if (out.empty() || x - out.back() > eps)
            out.push_back(x);
    }
    v = std::move(out);
}

// Fraction of the curve's parameter range, in [0, 1].
double curveFraction(const CurveRef& c, double t) {
    if (c.isLine())
        return std::clamp(t, 0.0, 1.0);
    if (c.kind() == CurveRef::Kind::Spline)
        return std::clamp((t - c.t0()) / c.sweep(), 0.0, 1.0);
    double off = normalizeAngle(t - c.t0());
    if (off > c.sweep())
        return (off - c.sweep() < kTwoPi - off) ? 1.0 : 0.0;
    return off / c.sweep();
}
double curveParamAt(const CurveRef& c, double u) {
    return c.isLine() ? u : c.t0() + u * c.sweep();
}

Result<EntityEdit> finishPieces(const Entity& target, std::vector<EntityData> pieces) {
    EntityEdit r;
    if (pieces.empty()) {
        r.eraseSource = true;
        return r;
    }
    r.modified = withData(target, std::move(pieces[0]));
    for (std::size_t i = 1; i < pieces.size(); ++i)
        r.added.push_back(withData(target, std::move(pieces[i])));
    return r;
}

std::optional<EntityData> pieceOf(const Entity& target, const CurveRef& c, double u0, double u1) {
    if (u1 - u0 <= 1e-12)
        return std::nullopt;
    const double ta = curveParamAt(c, u0), tb = curveParamAt(c, u1);
    auto sub = c.subcurve(ta, tb);
    if (!sub || sub.value().length() <= tol::kLinear)
        return std::nullopt;
    if (c.kind() == CurveRef::Kind::Spline) {
        auto sp = geom::splineSubcurve(*entityAs<SplineData>(target), ta, tb);
        if (!sp)
            return std::nullopt;
        return EntityData{Indirect<SplineData>(std::move(sp.value()))};
    }
    return dataFromCurve(sub.value());
}

Result<EntityEdit> trimSingle(const Entity& target, const CurveRef& c, Vec2 pick,
                              const std::vector<CurveRef>& edges, const TrimOptions& opt) {
    const bool isSpline = c.kind() == CurveRef::Kind::Spline;
    const bool periodic = !c.isLine() && !isSpline && c.sweep() >= kTwoPi - 1e-9;
    std::vector<double> cuts;
    const EdgeNear near(edges, opt.extendEdges);
    near.forEach(c.bounds().inflated(1e-6), [&](const CurveRef& e) {
        std::vector<geom::Intersection> xs;
        geom::intersect(c, e, xs, false, opt.extendEdges);
        for (const auto& x : xs) {
            double u = periodic ? normalizeAngle(x.tA - c.t0()) / kTwoPi : curveFraction(c, x.tA);
            cuts.push_back(u);
        }
    });
    sortUnique(cuts);
    double up = curveFraction(c, c.paramAt(pick));
    std::vector<EntityData> pieces;

    if (isSpline && c.isClosed()) {
        // The seam is not a cut: what is left runs from the cut after the pick round to the one
        // before it, possibly across the seam.
        cuts.erase(
            std::remove_if(cuts.begin(), cuts.end(), [](double u) { return u > 1.0 - 1e-9; }),
            cuts.end());
        sortUnique(cuts);
        if (cuts.size() < 2)
            return err("A closed curve needs two cutting points to be trimmed.");
        double lo = cuts.back();
        for (double x : cuts) {
            if (x <= up)
                lo = x;
        }
        auto it = std::upper_bound(cuts.begin(), cuts.end(), up);
        const double hi = it == cuts.end() ? cuts.front() : *it;
        if (lo > hi) {
            if (auto d = pieceOf(target, c, hi, lo))
                pieces.push_back(*d);
        } else {
            auto tail = pieceOf(target, c, hi, 1.0);
            auto head = pieceOf(target, c, 0.0, lo);
            if (tail && head) {
                pieces.push_back(EntityData{
                    Indirect<SplineData>(chainSplines(*std::get<Indirect<SplineData>>(*tail),
                                                      *std::get<Indirect<SplineData>>(*head)))});
            } else if (tail) {
                pieces.push_back(*tail);
            } else if (head) {
                pieces.push_back(*head);
            }
        }
        return finishPieces(target, std::move(pieces));
    }

    if (periodic) {
        if (cuts.size() > 1 && 1.0 - cuts.back() + cuts.front() <= 1e-9)
            cuts.pop_back();
        if (cuts.size() < 2)
            return err("A closed curve needs two cutting points to be trimmed.");
        double lo = cuts.back(), hi = cuts.front();
        for (double x : cuts) {
            if (x <= up)
                lo = x;
        }
        auto it = std::upper_bound(cuts.begin(), cuts.end(), up);
        hi = it == cuts.end() ? cuts.front() : *it;
        double sweep = (lo - hi) * kTwoPi;
        if (sweep <= 0)
            sweep += kTwoPi;
        CurveRef rest = c.kind() == CurveRef::Kind::Arc
                            ? CurveRef::arc(c.center(), c.radius(), c.t0() + hi * kTwoPi, sweep)
                            : CurveRef::ellipse(c.center(), c.majorAxis(), c.ratio(),
                                                c.t0() + hi * kTwoPi, sweep);
        if (auto d = dataFromCurve(rest))
            pieces.push_back(*d);
        return finishPieces(target, std::move(pieces));
    }

    const double eps = 1e-9;
    std::optional<double> lo, hi;
    for (double x : cuts) {
        if (x < up - eps)
            lo = x;
    }
    for (double x : cuts) {
        if (x >= up - eps) {
            hi = x;
            break;
        }
    }
    if (!lo && !hi)
        return err("No cutting edge crosses this object next to the pick point.");
    if (lo) {
        if (auto d = pieceOf(target, c, 0.0, *lo))
            pieces.push_back(*d);
    }
    if (hi) {
        if (auto d = pieceOf(target, c, *hi, 1.0))
            pieces.push_back(*d);
    }
    return finishPieces(target, std::move(pieces));
}

Result<EntityEdit> trimPolyline(const Entity& target, const PolylineData& raw, Vec2 pick,
                                const std::vector<CurveRef>& edges, const TrimOptions& opt) {
    PolylineData pl = cleanPolyline(raw);
    std::vector<Seg> segs = polylineSegs(pl);
    if (segs.empty())
        return err("The polyline has no length.");
    const double n = static_cast<double>(segs.size());
    std::vector<double> cuts;
    const EdgeNear near(edges, opt.extendEdges);
    for (std::size_t i = 0; i < segs.size(); ++i) {
        CurveRef c = segCurve(segs[i]);
        near.forEach(c.bounds().inflated(1e-6), [&](const CurveRef& e) {
            std::vector<geom::Intersection> xs;
            geom::intersect(c, e, xs, false, opt.extendEdges);
            for (const auto& x : xs)
                cuts.push_back(static_cast<double>(i) + segParam(segs[i], x.p));
        });
    }
    if (pl.closed) {
        for (double& x : cuts) {
            if (x >= n - 1e-9)
                x = 0;
        }
    }
    sortUnique(cuts);
    double sp = closestGlobalParam(segs, pick);
    std::vector<std::vector<Seg>> parts;
    const double eps = 1e-9;

    if (pl.closed) {
        if (cuts.size() < 2)
            return err("A closed polyline needs two cutting points to be trimmed.");
        double lo = cuts.back();
        for (double x : cuts) {
            if (x <= sp)
                lo = x;
        }
        auto it = std::upper_bound(cuts.begin(), cuts.end(), sp);
        double hi = it == cuts.end() ? cuts.front() : *it;
        parts.push_back(sliceCyclic(segs, hi, lo));
    } else {
        std::optional<double> lo, hi;
        for (double x : cuts) {
            if (x < sp - eps)
                lo = x;
        }
        for (double x : cuts) {
            if (x >= sp - eps) {
                hi = x;
                break;
            }
        }
        if (!lo && !hi)
            return err("No cutting edge crosses this object next to the pick point.");
        if (lo)
            parts.push_back(sliceSegs(segs, 0, *lo));
        if (hi)
            parts.push_back(sliceSegs(segs, *hi, n));
    }
    std::vector<EntityData> pieces;
    for (const auto& p : parts) {
        if (!p.empty())
            pieces.push_back(polylineFromSegs(p, false, &pl));
    }
    return finishPieces(target, std::move(pieces));
}

// A ray or construction line is cut at the edges: the part under the pick goes, a ray keeps a
// finite line towards its start and a new ray beyond the far cut.
Result<EntityEdit> trimRay(const Entity& target, const RayData& r, Vec2 pick,
                           const std::vector<CurveRef>& edges, const TrimOptions& opt) {
    if (tol::isZeroLength(r.dir))
        return err("The line has no direction.");
    const CurveRef c = CurveRef::line(rayStart(r), rayEnd(r));
    const double len = distance(c.lineA(), c.lineB());
    const double first = r.infinite ? -kRayReach : 0.0;
    std::vector<double> ts;
    for (const CurveRef& e : edges) {
        std::vector<geom::Intersection> xs;
        geom::intersect(c, e, xs, false, opt.extendEdges);
        for (const auto& x : xs)
            ts.push_back(first + x.tA * len);
    }
    sortUnique(ts, 1e-7);
    const double tp = dot(pick - r.origin, r.dir);
    std::optional<double> lo, hi;
    for (double t : ts) {
        if (t < tp - 1e-7)
            lo = t;
    }
    for (double t : ts) {
        if (t >= tp - 1e-7) {
            hi = t;
            break;
        }
    }
    if (!lo && !hi)
        return err("No cutting edge crosses this object next to the pick point.");
    auto at = [&](double t) { return r.origin + r.dir * t; };
    std::vector<EntityData> pieces;
    if (lo) {
        if (r.infinite)
            pieces.push_back(RayData{at(*lo), -r.dir, false});
        else if (*lo > tol::kLinear)
            pieces.push_back(LineData{r.origin, at(*lo)});
    }
    if (hi)
        pieces.push_back(RayData{at(*hi), r.dir, false});
    return finishPieces(target, std::move(pieces));
}

}  // namespace

Result<EntityEdit> trimEntity(const Entity& target, Vec2 pick,
                              const std::vector<const Entity*>& edges, const TrimOptions& opt) {
    auto ec = edgeCurves(edges);
    if (ec.empty())
        return err("There are no cutting edges.");
    switch (kind(target)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            std::vector<CurveRef> cs;
            if (!geom::curvesOf(target, cs) || cs.size() != 1)
                return err("This object cannot be trimmed.");
            return trimSingle(target, cs[0], pick, ec, opt);
        }
        case EntityKind::Ray:
            return trimRay(target, *entityAs<RayData>(target), pick, ec, opt);
        case EntityKind::Polyline:
            return trimPolyline(target, *entityAs<PolylineData>(target), pick, ec, opt);
        default:
            return err("This kind of object cannot be trimmed.");
    }
}

// ---- Extend -----------------------------------------------------------------------------------

namespace {

Result<EntityEdit> extendSeg(const Entity& target, Seg seg, bool atEnd,
                             const std::vector<CurveRef>& edges, const TrimOptions& opt,
                             Seg& result) {
    CurveRef c = segCurve(seg);
    std::optional<Vec2> best;
    double bestU =
        atEnd ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
    for (const CurveRef& e : edges) {
        std::vector<geom::Intersection> xs;
        geom::intersect(c, e, xs, true, opt.extendEdges);
        for (const auto& x : xs) {
            double u = segParamUnbounded(seg, x.p);
            // Candidates lie beyond the chosen end and not on the existing curve.
            if (atEnd ? (u > 1.0 + 1e-9 && u < bestU) : (u < -1e-9 && u > bestU)) {
                bestU = u;
                best = x.p;
            }
        }
    }
    if (!best)
        return err("No boundary edge lies ahead of this end of the object.");
    result = atEnd ? segWithEnd(seg, *best) : segWithStart(seg, *best);
    (void)target;
    return EntityEdit{};
}

// An elliptical arc grows along its ellipse to the nearest boundary crossing in the undrawn part.
Result<EntityEdit> extendEllipse(const Entity& target, const EllipseData& el,
                                 const std::vector<CurveRef>& edges, Vec2 pick,
                                 const TrimOptions& opt) {
    double sweep = normalizeAngle(el.endParam - el.startParam);
    if (sweep < 1e-12)
        return err("A full ellipse has no ends to extend.");
    const CurveRef full =
        CurveRef::ellipse(el.center, el.majorAxis, el.ratio, el.startParam, kTwoPi);
    const CurveRef drawn =
        CurveRef::ellipse(el.center, el.majorAxis, el.ratio, el.startParam, sweep);
    const bool atEnd = distance(pick, drawn.endPoint()) <= distance(pick, drawn.startPoint());
    const double gap = kTwoPi - sweep;
    double bestDelta = std::numeric_limits<double>::infinity(), bestT = 0;
    for (const CurveRef& e : edges) {
        std::vector<geom::Intersection> xs;
        geom::intersect(full, e, xs, true, opt.extendEdges);
        for (const auto& x : xs) {
            const double t = full.paramAt(x.p);
            // How far past the chosen end, going away from the drawn part, the crossing lies.
            double delta =
                atEnd ? normalizeAngle(t - el.endParam) : normalizeAngle(el.startParam - t);
            if (delta > 1e-9 && delta < gap - 1e-9 && delta < bestDelta) {
                bestDelta = delta;
                bestT = t;
            }
        }
    }
    if (!std::isfinite(bestDelta))
        return err("No boundary edge lies ahead of this end of the object.");
    EllipseData out = el;
    if (atEnd)
        out.endParam = normalizeAngle(bestT);
    else
        out.startParam = normalizeAngle(bestT);
    EntityEdit edit;
    edit.modified = withData(target, out);
    return edit;
}

// A spline is continued along its end tangent as a straight run, joined with the same degree so
// the result stays one spline (the original span is untouched).
Result<EntityEdit> extendSpline(const Entity& target, const SplineData& sd,
                                const std::vector<CurveRef>& edges, Vec2 pick,
                                const TrimOptions& opt) {
    if (sd.closed || sd.periodic)
        return err("A closed spline cannot be extended.");
    auto made = CurveRef::spline(sd);
    if (!made)
        return err("The spline is not valid.");
    const CurveRef& c = made.value();
    const bool atEnd = distance(pick, c.endPoint()) <= distance(pick, c.startPoint());
    const Vec2 from = atEnd ? c.endPoint() : c.startPoint();
    Vec2 dir = c.tangentAt(atEnd ? c.t1() : c.t0());
    if (!atEnd)
        dir = dir * -1.0;
    if (tol::isZeroLength(dir))
        return err("The spline has no direction at this end.");
    BBox2 box = c.bounds();
    for (const CurveRef& e : edges)
        box.merge(e.bounds());
    const double reach =
        2.0 * (distance(box.min, box.max) + distance(from, box.min) + distance(from, box.max)) +
        1.0;
    const CurveRef ray = CurveRef::line(from, from + dir * reach);
    double best = std::numeric_limits<double>::infinity();
    Vec2 hit{};
    for (const CurveRef& e : edges) {
        std::vector<geom::Intersection> xs;
        geom::intersect(ray, e, xs, false, opt.extendEdges);
        for (const auto& x : xs) {
            const double dd = dot(x.p - from, dir);
            if (dd > 1e-9 && dd < best) {
                best = dd;
                hit = x.p;
            }
        }
    }
    if (!std::isfinite(best))
        return err("No boundary edge lies ahead of this end of the object.");
    SplineData out = splineWithStraightRun(sd, atEnd, hit);
    EntityEdit edit;
    edit.modified = withData(target, Indirect<SplineData>(std::move(out)));
    return edit;
}

}  // namespace

Result<EntityEdit> extendEntity(const Entity& target, Vec2 pick,
                                const std::vector<const Entity*>& edges, const TrimOptions& opt) {
    auto ec = edgeCurves(edges);
    if (ec.empty())
        return err("There are no boundary edges.");
    switch (kind(target)) {
        case EntityKind::Line: {
            const auto& l = *entityAs<LineData>(target);
            Seg s{l.a, l.b, 0};
            if (tol::isZeroLength(l.b - l.a))
                return err("The line has no length.");
            bool atEnd = segParam(s, pick) >= 0.5;
            Seg r;
            auto res = extendSeg(target, s, atEnd, ec, opt, r);
            if (!res)
                return res;
            EntityEdit edit;
            edit.modified = withData(target, LineData{r.p0, r.p1});
            return edit;
        }
        case EntityKind::Arc: {
            const auto& a = *entityAs<ArcData>(target);
            double sweep = normalizeAngle(a.endAngle - a.startAngle);
            if (sweep < 1e-12)
                sweep = kTwoPi;
            Vec2 p0 = a.center + Vec2{std::cos(a.startAngle), std::sin(a.startAngle)} * a.radius;
            Vec2 p1 = a.center + Vec2{std::cos(a.endAngle), std::sin(a.endAngle)} * a.radius;
            Seg s{p0, p1, std::tan(sweep / 4.0)};
            bool atEnd = segParam(s, pick) >= 0.5;
            Seg r;
            auto res = extendSeg(target, s, atEnd, ec, opt, r);
            if (!res)
                return res;
            ArcData out = a;
            if (atEnd)
                out.endAngle = normalizeAngle(std::atan2(r.p1.y - a.center.y, r.p1.x - a.center.x));
            else
                out.startAngle =
                    normalizeAngle(std::atan2(r.p0.y - a.center.y, r.p0.x - a.center.x));
            EntityEdit edit;
            edit.modified = withData(target, out);
            return edit;
        }
        case EntityKind::Polyline: {
            PolylineData pl = cleanPolyline(*entityAs<PolylineData>(target));
            if (pl.closed)
                return err("A closed polyline cannot be extended.");
            std::vector<Seg> segs = polylineSegs(pl);
            if (segs.empty())
                return err("The polyline has no length.");
            bool atEnd = closestGlobalParam(segs, pick) >= static_cast<double>(segs.size()) * 0.5;
            Seg& s = atEnd ? segs.back() : segs.front();
            Seg r;
            auto res = extendSeg(target, s, atEnd, ec, opt, r);
            if (!res)
                return res;
            s = r;
            EntityEdit edit;
            edit.modified = withData(target, polylineFromSegs(segs, false, &pl));
            return edit;
        }
        case EntityKind::Ellipse:
            return extendEllipse(target, *entityAs<EllipseData>(target), ec, pick, opt);
        case EntityKind::Spline:
            return extendSpline(target, *entityAs<SplineData>(target), ec, pick, opt);
        case EntityKind::Ray: {
            const auto& r = *entityAs<RayData>(target);
            if (r.infinite)
                return err("A construction line already runs on without end.");
            // A ray runs on without end ahead; only its start can reach back to a boundary.
            Seg s{r.origin, r.origin + r.dir, 0};
            Seg out;
            auto res = extendSeg(target, s, false, ec, opt, out);
            if (!res)
                return res;
            EntityEdit edit;
            edit.modified = withData(target, RayData{out.p0, r.dir, false});
            return edit;
        }
        default:
            return err("This kind of object cannot be extended.");
    }
}

}  // namespace tbcad::modify

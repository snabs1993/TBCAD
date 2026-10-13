#include "tbcad/modify/trim.h"

#include <algorithm>
#include <cmath>

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

std::optional<EntityData> pieceOf(const CurveRef& c, double u0, double u1) {
    if (u1 - u0 <= 1e-12)
        return std::nullopt;
    auto sub = c.subcurve(curveParamAt(c, u0), curveParamAt(c, u1));
    if (!sub || sub.value().length() <= tol::kLinear)
        return std::nullopt;
    return dataFromCurve(sub.value());
}

Result<EntityEdit> trimSingle(const Entity& target, const CurveRef& c, Vec2 pick,
                              const std::vector<CurveRef>& edges, const TrimOptions& opt) {
    const bool periodic = !c.isLine() && c.sweep() >= kTwoPi - 1e-9;
    std::vector<double> cuts;
    for (const CurveRef& e : edges) {
        std::vector<geom::Intersection> xs;
        geom::intersect(c, e, xs, false, opt.extendEdges);
        for (const auto& x : xs) {
            double u = periodic ? normalizeAngle(x.tA - c.t0()) / kTwoPi : curveFraction(c, x.tA);
            cuts.push_back(u);
        }
    }
    sortUnique(cuts);
    double up = curveFraction(c, c.paramAt(pick));
    std::vector<EntityData> pieces;

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
        if (auto d = pieceOf(c, 0.0, *lo))
            pieces.push_back(*d);
    }
    if (hi) {
        if (auto d = pieceOf(c, *hi, 1.0))
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
    for (std::size_t i = 0; i < segs.size(); ++i) {
        CurveRef c = segCurve(segs[i]);
        for (const CurveRef& e : edges) {
            std::vector<geom::Intersection> xs;
            geom::intersect(c, e, xs, false, opt.extendEdges);
            for (const auto& x : xs)
                cuts.push_back(static_cast<double>(i) + segParam(segs[i], x.p));
        }
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
        case EntityKind::Ellipse: {
            std::vector<CurveRef> cs;
            if (!geom::curvesOf(target, cs) || cs.size() != 1)
                return err("This object cannot be trimmed.");
            return trimSingle(target, cs[0], pick, ec, opt);
        }
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
        default:
            return err("This kind of object cannot be extended.");
    }
}

}  // namespace tbcad::modify

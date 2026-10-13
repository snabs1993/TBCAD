#include "tbcad/modify/offset.h"

#include <algorithm>
#include <cmath>

namespace tbcad::modify {

using geom::CurveRef;

namespace {

constexpr double kJoinTol = 1e-5;

Error err(std::string m) {
    return Error{ErrorCode::InvalidArgument, std::move(m)};
}

Seg offsetSeg(const Seg& s, double d, bool& ok) {
    ok = true;
    auto g = arcGeom(s);
    if (!g) {
        Vec2 n = perp(normalized(s.p1 - s.p0)) * d;
        return {s.p0 + n, s.p1 + n, 0};
    }
    double r = g->theta > 0 ? g->radius - d : g->radius + d;
    if (r <= 1e-9) {
        ok = false;
        return s;
    }
    Vec2 u0 = normalized(s.p0 - g->center), u1 = normalized(s.p1 - g->center);
    return {g->center + u0 * r, g->center + u1 * r, s.bulge};
}

double distanceToSegs(const std::vector<Seg>& segs, Vec2 p) {
    double best = std::numeric_limits<double>::infinity();
    for (const Seg& s : segs)
        best = std::min(best, distance(segPoint(s, segParam(s, p)), p));
    return best;
}

bool straightOn(const Seg& a, const Seg& b) {
    if (a.isArc() || b.isArc())
        return false;
    Vec2 d0 = normalized(a.p1 - a.p0), d1 = normalized(b.p1 - b.p0);
    return std::abs(cross(d0, d1)) <= 1e-9 && dot(d0, d1) > 0;
}

// Fuses consecutive collinear lines (left over from cutting at intersections).
void mergeCollinear(std::vector<Seg>& run, bool closed) {
    for (std::size_t i = 0; i + 1 < run.size();) {
        if (straightOn(run[i], run[i + 1])) {
            run[i].p1 = run[i + 1].p1;
            run.erase(run.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        } else {
            ++i;
        }
    }
    if (closed && run.size() > 2 && straightOn(run.back(), run.front())) {
        run.front().p0 = run.back().p0;
        run.pop_back();
    }
}

Result<std::vector<Entity>> offsetPolyline(const Entity& src, const PolylineData& raw, double d,
                                           const OffsetOptions& opt) {
    PolylineData pl = cleanPolyline(raw);
    if (pl.v.size() < 2)
        return err("The polyline has no length to offset.");
    if (pl.closed && pl.v.size() < 3 && !std::any_of(pl.v.begin(), pl.v.end(), [](const auto& v) {
            return std::abs(v.bulge) > 1e-12;
        }))
        return err("The polyline has no area to offset.");
    const std::vector<Seg> segs = polylineSegs(pl);
    const std::size_t n = segs.size();
    const bool closed = pl.closed;

    // 1. Offset every segment on its own.
    struct Off {
        Seg s;
        std::size_t src;
    };
    std::vector<Off> offs;
    for (std::size_t i = 0; i < n; ++i) {
        bool ok = false;
        Seg o = offsetSeg(segs[i], d, ok);
        if (ok)
            offs.push_back({o, i});
    }
    if (offs.empty())
        return err("The offset distance is too large: nothing is left.");

    // 2. Corners. Mitre joins edit the segment ends; everything else gets a connector.
    const std::size_t m = offs.size();
    const std::size_t corners = closed ? m : m - 1;
    std::vector<std::optional<Seg>> connector(corners);
    for (std::size_t k = 0; k < corners; ++k) {
        Off& a = offs[k];
        Off& b = offs[(k + 1) % m];
        if (samePoint(a.s.p1, b.s.p0, kJoinTol))
            continue;
        const bool adjacent = (a.src + 1) % n == b.src;
        Vec2 t0 = segTangent(segs[a.src], 1.0), t1 = segTangent(segs[b.src], 0.0);
        Vec2 v = segs[a.src].p1;
        double c = cross(t0, t1), dt = dot(t0, t1);
        bool reversal = std::abs(c) <= 1e-9 && dt < 0;
        bool convex = reversal || c * d < -1e-12;
        if (convex && adjacent) {
            if (opt.roundCorners) {
                double turn = std::atan2(std::abs(c), dt);
                double sign = reversal ? (d > 0 ? -1.0 : 1.0) : (c > 0 ? 1.0 : -1.0);
                connector[k] = Seg{a.s.p1, b.s.p0, std::tan(sign * turn / 4.0)};
                continue;
            }
            std::vector<geom::Intersection> xs;
            geom::intersect(segCurve(a.s), segCurve(b.s), xs, true, true);
            double bestD = std::numeric_limits<double>::infinity();
            std::optional<Vec2> mitre;
            for (const auto& x : xs) {
                if (segParamUnbounded(a.s, x.p) < 1.0 - 1e-9 ||
                    segParamUnbounded(b.s, x.p) > 1e-9)
                    continue;
                double dv = distance(x.p, v);
                if (dv < bestD) {
                    bestD = dv;
                    mitre = x.p;
                }
            }
            if (mitre) {
                a.s = segWithEnd(a.s, *mitre);
                b.s = segWithStart(b.s, *mitre);
                continue;
            }
        }
        connector[k] = Seg{a.s.p1, b.s.p0, 0};
    }

    // 3. The raw offset chain.
    std::vector<Seg> chain;
    for (std::size_t k = 0; k < m; ++k) {
        chain.push_back(offs[k].s);
        if (k < corners && connector[k])
            chain.push_back(*connector[k]);
    }

    // 4. Cut the chain at every self-intersection.
    const std::size_t cn = chain.size();
    std::vector<CurveRef> curves;
    std::vector<BBox2> boxes;
    for (const Seg& s : chain) {
        curves.push_back(segCurve(s));
        boxes.push_back(curves.back().bounds().inflated(1e-7));
    }
    std::vector<std::vector<double>> cuts(cn);
    for (std::size_t i = 0; i < cn; ++i) {
        for (std::size_t j = i + 1; j < cn; ++j) {
            if (!boxes[i].intersects(boxes[j]))
                continue;
            std::vector<geom::Intersection> xs;
            geom::intersect(curves[i], curves[j], xs);
            for (const auto& x : xs) {
                double ui = segParam(chain[i], x.p), uj = segParam(chain[j], x.p);
                if (ui > 1e-9 && ui < 1.0 - 1e-9)
                    cuts[i].push_back(ui);
                if (uj > 1e-9 && uj < 1.0 - 1e-9)
                    cuts[j].push_back(uj);
            }
        }
    }

    // 5. Keep the pieces that stay at least |d| away from the source.
    const double absd = std::abs(d);
    const double validTol = 1e-6 * std::max(1.0, absd);
    std::vector<Seg> valid;
    for (std::size_t i = 0; i < cn; ++i) {
        std::vector<double> bounds{0.0};
        std::sort(cuts[i].begin(), cuts[i].end());
        for (double u : cuts[i]) {
            if (u - bounds.back() > 1e-9)
                bounds.push_back(u);
        }
        bounds.push_back(1.0);
        for (std::size_t b = 0; b + 1 < bounds.size(); ++b) {
            Seg piece = segSub(chain[i], bounds[b], bounds[b + 1]);
            if (segLength(piece) <= tol::kLinear)
                continue;
            Vec2 mid = segPoint(chain[i], 0.5 * (bounds[b] + bounds[b + 1]));
            if (distanceToSegs(segs, mid) < absd - validTol)
                continue;  // inside the offset distance: part of a loop that gets cut away
            // Two raw segments can run over the same stretch (a vanishing bridge): keep it once.
            bool duplicate = std::any_of(valid.begin(), valid.end(), [&](const Seg& v) {
                return (samePoint(v.p0, piece.p0, kJoinTol) && samePoint(v.p1, piece.p1, kJoinTol) &&
                        std::abs(v.bulge - piece.bulge) < 1e-7) ||
                       (samePoint(v.p0, piece.p1, kJoinTol) && samePoint(v.p1, piece.p0, kJoinTol) &&
                        std::abs(v.bulge + piece.bulge) < 1e-7);
            });
            if (!duplicate)
                valid.push_back(piece);
        }
    }

    // 6. Join the surviving pieces into chains (pieces keep their raw order where possible).
    std::vector<std::vector<Seg>> runs;
    std::vector<bool> used(valid.size(), false);
    for (std::size_t i = 0; i < valid.size(); ++i) {
        if (used[i])
            continue;
        used[i] = true;
        std::vector<Seg> run{valid[i]};
        for (bool grew = true; grew;) {
            grew = false;
            for (std::size_t j = 0; j < valid.size(); ++j) {
                if (used[j])
                    continue;
                if (samePoint(run.back().p1, valid[j].p0, kJoinTol)) {
                    run.push_back(valid[j]);
                } else if (samePoint(run.front().p0, valid[j].p1, kJoinTol)) {
                    run.insert(run.begin(), valid[j]);
                } else {
                    continue;
                }
                used[j] = true;
                grew = true;
                break;
            }
        }
        runs.push_back(std::move(run));
    }
    if (runs.empty())
        return err("The offset distance is too large: nothing is left.");

    std::vector<Entity> out;
    for (auto& run : runs) {
        bool isClosed = run.size() > 1 && samePoint(run.back().p1, run.front().p0, kJoinTol);
        if (isClosed && !closed)
            isClosed = false;  // an open source stays open unless the offset really closes
        mergeCollinear(run, isClosed);
        out.push_back(withData(src, polylineFromSegs(run, isClosed, &pl)));
    }
    return out;
}

Result<std::vector<Entity>> offsetSampled(const Entity& src, const CurveRef& c, bool closed,
                                          double d, const OffsetOptions& opt) {
    std::vector<Vec2> pts;
    std::vector<double> params;
    geom::flatten(c, std::max(opt.tolerance, 1e-4), pts, &params);
    std::vector<Vec2> q;
    for (std::size_t i = 0; i < pts.size(); ++i)
        q.push_back(pts[i] + perp(c.tangentAt(params[i])) * d);
    if (closed && q.size() > 1)
        q.pop_back();
    if (q.size() < 3)
        return err("The curve is too short to offset.");
    geom::InterpolateOptions io;
    io.closed = closed;
    auto sp = geom::interpolateSpline(q, io);
    if (!sp)
        return err("The offset curve could not be fitted: " + sp.error().message);
    std::vector<Entity> out;
    out.push_back(withData(src, Indirect<SplineData>(std::move(sp.value()))));
    return out;
}

}  // namespace

int sideOfEntity(const Entity& e, Vec2 p) {
    auto sign = [](double v) { return v > 1e-12 ? 1 : (v < -1e-12 ? -1 : 0); };
    if (const auto* pl = entityAs<PolylineData>(e)) {
        auto segs = polylineSegs(cleanPolyline(*pl));
        if (segs.empty())
            return 0;
        double g = closestGlobalParam(segs, p);
        std::size_t i = std::min(static_cast<std::size_t>(g), segs.size() - 1);
        double u = g - static_cast<double>(i);
        Vec2 cp = segPoint(segs[i], u);
        return sign(cross(segTangent(segs[i], u), p - cp));
    }
    std::vector<CurveRef> cs;
    if (!geom::curvesOf(e, cs) || cs.empty())
        return 0;
    const CurveRef* best = &cs[0];
    double bd = best->distanceTo(p);
    for (const auto& c : cs) {
        double dd = c.distanceTo(p);
        if (dd < bd) {
            bd = dd;
            best = &c;
        }
    }
    double t = best->paramAt(p);
    return sign(cross(best->tangentAt(t), p - best->pointAt(t)));
}

Result<std::vector<Entity>> offsetEntity(const Entity& e, double d, const OffsetOptions& opt) {
    if (!std::isfinite(d))
        return err("The offset distance is not a number.");
    if (std::abs(d) <= tol::kLinear) {
        return err("The offset distance is zero.");
    }
    std::vector<Entity> out;
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& l = *entityAs<LineData>(e);
            if (tol::isZeroLength(l.b - l.a))
                return err("The line has no length.");
            Vec2 n = perp(normalized(l.b - l.a)) * d;
            out.push_back(withData(e, LineData{l.a + n, l.b + n}));
            return out;
        }
        case EntityKind::Circle: {
            const auto& c = *entityAs<CircleData>(e);
            double r = c.radius - d;
            if (r <= tol::kLinear)
                return err("The offset distance is too large: the circle collapses.");
            out.push_back(withData(e, CircleData{c.center, r}));
            return out;
        }
        case EntityKind::Arc: {
            auto a = *entityAs<ArcData>(e);
            double r = a.radius - d;
            if (r <= tol::kLinear)
                return err("The offset distance is too large: the arc collapses.");
            a.radius = r;
            out.push_back(withData(e, a));
            return out;
        }
        case EntityKind::Polyline:
            return offsetPolyline(e, *entityAs<PolylineData>(e), d, opt);
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            std::vector<CurveRef> cs;
            if (!geom::curvesOf(e, cs) || cs.empty())
                return err("The curve cannot be offset.");
            return offsetSampled(e, cs[0], cs[0].isClosed(), d, opt);
        }
        default:
            return err("This kind of object cannot be offset.");
    }
}

Result<std::vector<Entity>> offsetToward(const Entity& e, Vec2 sidePoint, double distance,
                                         const OffsetOptions& opt) {
    int side = sideOfEntity(e, sidePoint);
    if (side == 0)
        return err("The side point lies on the object; pick a point to one side of it.");
    return offsetEntity(e, side * std::abs(distance), opt);
}

Result<std::vector<Entity>> offsetThrough(const Entity& e, Vec2 through, const OffsetOptions& opt) {
    int side = sideOfEntity(e, through);
    double dist = geom::distance(e, through);
    if (side == 0 || !(dist > tol::kLinear) || !std::isfinite(dist))
        return err("The point lies on the object; pick a point away from it.");
    return offsetEntity(e, side * dist, opt);
}

}  // namespace tbcad::modify

#include "tbcad/modify/offset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "tbcad/geom/box_grid.h"

namespace tbcad::modify {

using geom::BoxGrid;
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


// Hash of a point on a grid of `kJoinTol * 8`, so points within kJoinTol of each other are in the
// same or an adjacent cell.
struct PointIndex {
    static long long cell(double v) { return static_cast<long long>(std::floor(v / (kJoinTol * 8))); }
    static std::uint64_t key(long long x, long long y) {
        return static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(y) * 0xC2B2AE3D27D4EB4Full;
    }
    std::unordered_multimap<std::uint64_t, std::size_t> map;
    void add(Vec2 p, std::size_t i) { map.emplace(key(cell(p.x), cell(p.y)), i); }
    template <class F>
    void near(Vec2 p, F&& f) const {
        const long long cx = cell(p.x), cy = cell(p.y);
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                auto r = map.equal_range(key(cx + dx, cy + dy));
                for (auto it = r.first; it != r.second; ++it)
                    f(it->second);
            }
    }
};

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
    std::vector<Seg> out;
    out.reserve(run.size());
    for (const Seg& s : run) {
        if (!out.empty() && straightOn(out.back(), s))
            out.back().p1 = s.p1;
        else
            out.push_back(s);
    }
    run = std::move(out);
    if (closed && run.size() > 2 && straightOn(run.back(), run.front())) {
        run.front().p0 = run.back().p0;
        run.pop_back();
    }
}

// The offset keeps the widths of the source: each new segment takes the width of the source
// segment nearest to it, interpolated along that segment.
void carryWidths(const PolylineData& source, const std::vector<Seg>& segs, PolylineData& made) {
    if (segs.empty() || source.v.size() < segs.size())
        return;
    const std::vector<Seg> out = polylineSegs(made);
    auto widthAt = [&](double g, bool start) {
        const std::size_t i = std::min(static_cast<std::size_t>(g), segs.size() - 1);
        const double u = std::clamp(g - static_cast<double>(i), 0.0, 1.0);
        (void)start;
        return source.v[i].startWidth + (source.v[i].endWidth - source.v[i].startWidth) * u;
    };
    for (std::size_t k = 0; k < out.size() && k < made.v.size(); ++k) {
        // Sample inside the segment so a vertex shared with a neighbour does not decide.
        const double g0 = closestGlobalParam(segs, segPoint(out[k], 0.02));
        const double g1 = closestGlobalParam(segs, segPoint(out[k], 0.98));
        const double w0 = widthAt(g0, true), w1 = widthAt(g1, false);
        // Extend the interpolation back to the segment ends.
        made.v[k].startWidth = w0 + (w0 - w1) * 0.02 / 0.96;
        made.v[k].endWidth = w1 + (w1 - w0) * 0.02 / 0.96;
        made.v[k].startWidth = std::max(0.0, made.v[k].startWidth);
        made.v[k].endWidth = std::max(0.0, made.v[k].endWidth);
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
                if (segParamUnbounded(a.s, x.p) < 1.0 - 1e-9 || segParamUnbounded(b.s, x.p) > 1e-9)
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
    const bool useGrid = !opt.scanAll && cn >= 64;
    const auto cutPair = [&](std::size_t i, std::size_t j) {
        std::vector<geom::Intersection> xs;
        geom::intersect(curves[i], curves[j], xs);
        for (const auto& x : xs) {
            double ui = segParam(chain[i], x.p), uj = segParam(chain[j], x.p);
            if (ui > 1e-9 && ui < 1.0 - 1e-9)
                cuts[i].push_back(ui);
            if (uj > 1e-9 && uj < 1.0 - 1e-9)
                cuts[j].push_back(uj);
        }
    };
    if (useGrid) {
        const BoxGrid grid(boxes);
        for (std::size_t i = 0; i < cn; ++i)
            grid.query(boxes[i], [&](std::size_t j) {
                if (j > i)
                    cutPair(i, j);
            });
    } else {
        for (std::size_t i = 0; i < cn; ++i)
            for (std::size_t j = i + 1; j < cn; ++j)
                if (boxes[i].intersects(boxes[j]))
                    cutPair(i, j);
    }

    // 4b. A raw segment can also run into the offset distance of the opposite side of the source
    // (a thin feature, the tip of a spike) without crossing any other raw segment. The edge of the
    // zone closer than |d| to the source is made of the offsets of both sides and of circles
    // around the vertices; where a raw segment meets those it may enter or leave the zone. These
    // extra cuts are only kept where they separate a surviving piece from a discarded one.
    const double absd = std::abs(d);
    std::vector<Seg> zoneEdge;
    for (const Seg& s0 : segs) {
        bool ok = false;
        const Seg o = offsetSeg(s0, -d, ok);
        if (ok)
            zoneEdge.push_back(o);
        for (const Vec2 v : {s0.p0, s0.p1}) {
            zoneEdge.push_back(Seg{v + Vec2{absd, 0}, v - Vec2{absd, 0}, 1.0});
            zoneEdge.push_back(Seg{v - Vec2{absd, 0}, v + Vec2{absd, 0}, 1.0});
        }
    }
    std::vector<std::vector<double>> extras(cn);
    if (useGrid || cn * zoneEdge.size() <= 4000000) {
        std::vector<CurveRef> zc;
        std::vector<BBox2> zb;
        for (const Seg& z : zoneEdge) {
            zc.push_back(segCurve(z));
            zb.push_back(zc.back().bounds().inflated(1e-7));
        }
        const auto cutZone = [&](std::size_t i, std::size_t j) {
            std::vector<geom::Intersection> xs;
            geom::intersect(curves[i], zc[j], xs);
            for (const auto& x : xs) {
                const double ui = segParam(chain[i], x.p);
                if (ui > 1e-9 && ui < 1.0 - 1e-9)
                    extras[i].push_back(ui);
            }
        };
        if (useGrid) {
            const BoxGrid grid(zb);
            for (std::size_t i = 0; i < cn; ++i)
                grid.query(boxes[i], [&](std::size_t j) { cutZone(i, j); });
        } else {
            for (std::size_t i = 0; i < cn; ++i)
                for (std::size_t j = 0; j < zc.size(); ++j)
                    if (boxes[i].intersects(zb[j]))
                        cutZone(i, j);
        }
    }

    // 5. Keep the pieces that stay at least |d| away from the source.
    const double validTol = 1e-6 * std::max(1.0, absd);
    std::vector<Seg> valid;
    PointIndex validStart;  // the surviving pieces by their start point
    const auto addValid = [&](const Seg& piece) {
        // Two raw segments can run over the same stretch (a vanishing bridge): keep it once.
        bool duplicate = false;
        const auto same = [&](std::size_t k) {
            const Seg& v = valid[k];
            duplicate = duplicate ||
                        (samePoint(v.p0, piece.p0, kJoinTol) && samePoint(v.p1, piece.p1, kJoinTol) &&
                         std::abs(v.bulge - piece.bulge) < 1e-7) ||
                        (samePoint(v.p0, piece.p1, kJoinTol) && samePoint(v.p1, piece.p0, kJoinTol) &&
                         std::abs(v.bulge + piece.bulge) < 1e-7);
        };
        validStart.near(piece.p0, same);
        validStart.near(piece.p1, same);
        if (!duplicate) {
            validStart.add(piece.p0, valid.size());
            valid.push_back(piece);
        }
    };
    // Distance from `p` to the source, looking only at the segments that can be closer than |d|.
    std::vector<BBox2> segBoxes;
    std::optional<BoxGrid> segGrid;
    if (useGrid) {
        for (const Seg& s0 : segs)
            segBoxes.push_back(segCurve(s0).bounds());
        segGrid.emplace(segBoxes);
    }
    const auto nearSource = [&](Vec2 mid) {
        if (!segGrid)
            return distanceToSegs(segs, mid);
        double best = std::numeric_limits<double>::infinity();
        segGrid->query(BBox2{mid, mid}.inflated(absd + 1e-6), [&](std::size_t k) {
            best = std::min(best, distance(segPoint(segs[k], segParam(segs[k], mid)), mid));
        });
        return best;
    };
    for (std::size_t i = 0; i < cn; ++i) {
        // Every cut with a flag: true for the extra ones.
        std::vector<std::pair<double, bool>> all;
        for (double u : cuts[i])
            all.emplace_back(u, false);
        for (double u : extras[i])
            all.emplace_back(u, true);
        std::sort(all.begin(), all.end());
        std::vector<std::pair<double, bool>> bounds{{0.0, false}};
        for (const auto& c : all) {
            if (c.first - bounds.back().first > 1e-9)
                bounds.push_back(c);
            else if (!c.second)
                bounds.back().second = false;  // a real cut at the same place
        }
        bounds.emplace_back(1.0, false);
        const std::size_t count = bounds.size() - 1;
        std::vector<char> good(count, 0);
        for (std::size_t b = 0; b < count; ++b) {
            const Seg piece = segSub(chain[i], bounds[b].first, bounds[b + 1].first);
            if (segLength(piece) <= tol::kLinear)
                continue;
            const Vec2 mid = segPoint(chain[i], 0.5 * (bounds[b].first + bounds[b + 1].first));
            good[b] = nearSource(mid) >= absd - validTol;  // else inside the distance
        }
        for (std::size_t b = 0; b < count;) {
            if (!good[b]) {
                ++b;
                continue;
            }
            std::size_t e = b + 1;
            // An extra cut between two surviving pieces is not a cut at all.
            while (e < count && good[e] && bounds[e].second)
                ++e;
            addValid(segSub(chain[i], bounds[b].first, bounds[e].first));
            b = e;
        }
    }

    // 6. Join the surviving pieces into chains (pieces keep their raw order where possible).
    std::vector<std::vector<Seg>> runs;
    std::vector<bool> used(valid.size(), false);
    PointIndex byEnd;  // the pieces by their end point (validStart has them by their start)
    for (std::size_t i = 0; i < valid.size(); ++i)
        byEnd.add(valid[i].p1, i);
    for (std::size_t i = 0; i < valid.size(); ++i) {
        if (used[i])
            continue;
        used[i] = true;
        std::vector<Seg> run{valid[i]};
        // Run ends grow at both ends; `front` holds the pieces put before the start, newest last.
        std::vector<Seg> front;
        for (bool grew = true; grew;) {
            grew = false;
            const Vec2 tail = run.back().p1;
            const Vec2 head = front.empty() ? run.front().p0 : front.back().p0;
            // The lowest-numbered unused piece that continues either end.
            std::size_t best = valid.size();
            bool atTail = false;
            validStart.near(tail, [&](std::size_t j) {
                if (!used[j] && j < best && samePoint(tail, valid[j].p0, kJoinTol)) {
                    best = j;
                    atTail = true;
                }
            });
            byEnd.near(head, [&](std::size_t j) {
                if (!used[j] && j < best && samePoint(head, valid[j].p1, kJoinTol)) {
                    best = j;
                    atTail = false;
                }
            });
            if (best == valid.size())
                break;
            if (atTail)
                run.push_back(valid[best]);
            else
                front.push_back(valid[best]);
            used[best] = true;
            grew = true;
        }
        run.insert(run.begin(), front.rbegin(), front.rend());
        runs.push_back(std::move(run));
    }
    if (runs.empty())
        return err("The offset distance is too large: nothing is left.");

    const bool wide = std::any_of(pl.v.begin(), pl.v.end(), [](const PolyVertex& v) {
        return v.startWidth != 0 || v.endWidth != 0;
    });
    std::vector<Entity> out;
    for (auto& run : runs) {
        bool isClosed = run.size() > 1 && samePoint(run.back().p1, run.front().p0, kJoinTol);
        if (isClosed && !closed)
            isClosed = false;  // an open source stays open unless the offset really closes
        mergeCollinear(run, isClosed);
        PolylineData made = polylineFromSegs(run, isClosed, &pl);
        if (wide)
            carryWidths(pl, segs, made);
        out.push_back(withData(src, std::move(made)));
    }
    return out;
}

Result<std::vector<Entity>> offsetSampled(const Entity& src, const CurveRef& c, bool closed,
                                          double d, const OffsetOptions& opt) {
    std::vector<Vec2> pts;
    std::vector<double> params;
    // A fixed 0.01 tolerance on a 20 m ellipse drawn in mm means thousands of samples and a very
    // slow spline fit; keep the sample count independent of the size.
    const BBox2 box = c.bounds();
    const double extent = std::max(box.max.x - box.min.x, box.max.y - box.min.y);
    geom::flatten(c, std::max({opt.tolerance, 1e-4, extent * 5e-6}), pts, &params);
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
        case EntityKind::Ray: {
            auto r = *entityAs<RayData>(e);
            if (tol::isZeroLength(r.dir))
                return err("The line has no direction.");
            r.origin = r.origin + perp(normalized(r.dir)) * d;
            out.push_back(withData(e, r));
            return out;
        }
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

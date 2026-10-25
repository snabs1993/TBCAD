#include "tbcad/drafting/osnap.h"

#include <algorithm>
#include <cmath>
#include <deque>

#include "tbcad/geom/entity.h"

namespace tbcad::drafting {

namespace {

constexpr double kTieMm = 1e-6;

void add(std::vector<SnapCandidate>& out, const SnapRequest& rq, SnapMode mode, Vec2 p, EntityId id,
         EntityId id2 = {}) {
    if (!(rq.modes & maskOf(mode)))
        return;
    const double d = distance(rq.cursor, p);
    if (d > rq.apertureMm)
        return;
    out.push_back({p, mode, d, id, id2});
}

// Points of an ellipse or spline where `f(t)` changes sign, found by sampling the parameter
// range and bisecting each bracket.
template <class F>
void curveRoots(const geom::CurveRef& c, F f, std::vector<double>& out) {
    constexpr int kSteps = 256;
    const double t0 = c.t0(), t1 = c.t1();
    double ta = t0, fa = f(ta);
    for (int i = 1; i <= kSteps; ++i) {
        const double tb = t0 + (t1 - t0) * i / kSteps;
        const double fb = f(tb);
        if (fa == 0) {
            out.push_back(ta);
        } else if ((fa < 0) != (fb < 0) && fb != 0) {
            double lo = ta, hi = tb, flo = fa;
            for (int k = 0; k < 60; ++k) {
                const double mid = 0.5 * (lo + hi);
                const double fm = f(mid);
                if ((flo < 0) == (fm < 0)) {
                    lo = mid;
                    flo = fm;
                } else {
                    hi = mid;
                }
            }
            out.push_back(0.5 * (lo + hi));
        }
        ta = tb;
        fa = fb;
    }
    if (fa == 0)
        out.push_back(ta);
}

// Feet of the perpendiculars from `from` to the curve.
void perpendicularFeet(const geom::CurveRef& c, Vec2 from, std::vector<Vec2>& out) {
    switch (c.kind()) {
        case geom::CurveRef::Kind::Line: {
            const Vec2 a = c.lineA(), ab = c.lineB() - a;
            const double len2 = lengthSquared(ab);
            if (len2 <= 0)
                return;
            const double u = dot(from - a, ab) / len2;
            if (u >= -1e-9 && u <= 1 + 1e-9)
                out.push_back(a + ab * std::clamp(u, 0.0, 1.0));
            return;
        }
        case geom::CurveRef::Kind::Arc: {
            const Vec2 v = from - c.center();
            if (length(v) <= tol::kLinear)
                return;  // every point of the circle is "perpendicular" from the center
            const double ang = std::atan2(v.y, v.x);
            for (double a : {ang, ang + kPi}) {
                const double t = c.wrapParam(a);
                if (c.containsParam(t))
                    out.push_back(c.pointAt(t));
            }
            return;
        }
        default: {
            // Ellipse, spline: every parameter where the chord to `from` is normal to the curve.
            std::vector<double> ts;
            curveRoots(
                c, [&](double t) { return dot(c.derivativeAt(t), from - c.pointAt(t)); }, ts);
            if (ts.empty())
                out.push_back(c.closestPoint(from));  // the nearest point is an end
            for (double t : ts)
                out.push_back(c.pointAt(t));
            return;
        }
    }
}

// Points where the line from `from` touches the circle / arc.
void tangentPoints(const geom::CurveRef& c, Vec2 from, std::vector<Vec2>& out) {
    if (c.kind() == geom::CurveRef::Kind::Ellipse || c.kind() == geom::CurveRef::Kind::Spline) {
        std::vector<double> ts;
        curveRoots(c, [&](double t) { return cross(c.derivativeAt(t), from - c.pointAt(t)); }, ts);
        for (double t : ts) {
            const Vec2 p = c.pointAt(t);
            if (distance(p, from) > tol::kLinear && lengthSquared(c.derivativeAt(t)) > 0)
                out.push_back(p);
        }
        return;
    }
    if (c.kind() != geom::CurveRef::Kind::Arc)
        return;
    const Vec2 v = from - c.center();
    const double d = length(v);
    const double r = c.radius();
    if (d <= r * (1 + 1e-9) || r <= 0)
        return;
    const double base = std::atan2(v.y, v.x);
    const double off = std::acos(r / d);
    for (double a : {base + off, base - off}) {
        const double t = c.wrapParam(a);
        if (c.containsParam(t))
            out.push_back(c.pointAt(t));
    }
}

// How close the geometry of `e` comes to `p`, without building its snap geometry: a lower bound of
// the distance to anything it can snap to. Straight and round shapes are measured exactly (their
// bounding boxes are poor stand-ins for a long diagonal line); other kinds fall back on the box.
double segmentDistance(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double len2 = lengthSquared(ab);
    const double u = len2 > 0 ? std::clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    return distance(p, a + ab * u);
}

// Distance to the counter-clockwise arc from `start` through `sweep` radians.
double arcDistance(Vec2 p, Vec2 c, double r, double start, double sweep) {
    const Vec2 v = p - c;
    const double d = length(v);
    if (sweep >= kTwoPi - 1e-12 || d == 0)
        return std::abs(d - r);
    double a = std::atan2(v.y, v.x) - start;
    a -= kTwoPi * std::floor(a / kTwoPi);  // [0, 2pi)
    if (a <= sweep)
        return std::abs(d - r);
    const double s0 = start, s1 = start + sweep;
    return std::min(distance(p, c + Vec2{std::cos(s0), std::sin(s0)} * r),
                    distance(p, c + Vec2{std::cos(s1), std::sin(s1)} * r));
}

double reach(const Document& doc, const SnapGeometryRegistry& providers, const Entity& e, Vec2 p,
             double boxDistance) {
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto* l = entityAs<LineData>(e);
            return segmentDistance(p, l->a, l->b);
        }
        case EntityKind::Circle: {
            const auto* c = entityAs<CircleData>(e);
            const double d = distance(p, c->center);
            return std::min(d, std::abs(d - c->radius));  // the centre snaps too
        }
        case EntityKind::Arc: {
            const auto* a = entityAs<ArcData>(e);
            double sweep = a->endAngle - a->startAngle;
            sweep -= kTwoPi * std::floor(sweep / kTwoPi);
            if (sweep <= 0)
                sweep = kTwoPi;
            return std::min(distance(p, a->center),
                            arcDistance(p, a->center, a->radius, a->startAngle, sweep));
        }
        case EntityKind::Polyline: {
            const auto* pl = entityAs<PolylineData>(e);
            const std::size_t n = pl->v.size();
            if (n == 0)
                return boxDistance;
            if (n == 1)
                return distance(p, pl->v[0].p);
            double best = 1e300;
            const std::size_t segs = pl->closed ? n : n - 1;
            for (std::size_t i = 0; i < segs; ++i) {
                const PolyVertex& v0 = pl->v[i];
                const Vec2 q = pl->v[(i + 1) % n].p;
                if (const auto arc = geom::bulgeArc(v0.p, q, v0.bulge))
                    best = std::min(best, arcDistance(p, arc->center, arc->radius, arc->startAngle,
                                                      arc->sweep));
                else
                    best = std::min(best, segmentDistance(p, v0.p, q));
            }
            return best;
        }
        case EntityKind::Ellipse:
        case EntityKind::Spline:
        case EntityKind::Hatch: {
            // Their boxes are big and mostly empty: measure the curves, or the box of every
            // such object around the cursor would count as reaching it.
            SnapGeometry g;
            if (!providers.collect(doc, e, g))
                return boxDistance;
            double best = 1e300;
            for (const geom::CurveRef& c : g.curves)
                best = std::min(best, c.distanceTo(p));
            // The centre of an ellipse and the like snap although they are not on the curve.
            for (const auto* pts :
                 {&g.endpoints, &g.midpoints, &g.centers, &g.quadrants, &g.geometricCenters})
                for (const Vec2& q : *pts)
                    best = std::min(best, distance(p, q));
            return best < 1e300 ? best : boxDistance;
        }
        default:
            return boxDistance;
    }
}

// The part of a curve's box that matters for intersections inside the aperture square `ap`: for
// a straight curve the box of the piece inside the square, else the box clipped to it.
BBox2 relevantBox(const geom::CurveRef& c, const BBox2& ap) {
    BBox2 box = c.bounds();
    if (c.kind() == geom::CurveRef::Kind::Line) {
        // Liang-Barsky against the square.
        const Vec2 a = c.lineA(), d = c.lineB() - a;
        double t0 = 0, t1 = 1;
        auto clip = [&](double p, double q) {
            if (p == 0)
                return q >= 0;
            const double r = q / p;
            if (p < 0) {
                if (r > t1)
                    return false;
                t0 = std::max(t0, r);
            } else {
                if (r < t0)
                    return false;
                t1 = std::min(t1, r);
            }
            return true;
        };
        if (!(clip(-d.x, a.x - ap.min.x) && clip(d.x, ap.max.x - a.x) &&
              clip(-d.y, a.y - ap.min.y) && clip(d.y, ap.max.y - a.y)))
            return BBox2{};
        box = BBox2{};
        box.expand(a + d * t0);
        box.expand(a + d * t1);
        return box;
    }
    box.min.x = std::max(box.min.x, ap.min.x);
    box.min.y = std::max(box.min.y, ap.min.y);
    box.max.x = std::min(box.max.x, ap.max.x);
    box.max.y = std::min(box.max.y, ap.max.y);
    return box;
}

}  // namespace

void curveSnapPoints(const geom::CurveRef& curve, const SnapRequest& rq,
                     std::vector<SnapCandidate>& out, EntityId id) {
    if (rq.modes & maskOf(SnapMode::Nearest))
        add(out, rq, SnapMode::Nearest, curve.closestPoint(rq.cursor), id);
    if (rq.base) {
        std::vector<Vec2> pts;
        if (rq.modes & maskOf(SnapMode::Perpendicular)) {
            perpendicularFeet(curve, *rq.base, pts);
            for (Vec2 p : pts)
                add(out, rq, SnapMode::Perpendicular, p, id);
        }
        if (rq.modes & maskOf(SnapMode::Tangent)) {
            pts.clear();
            tangentPoints(curve, *rq.base, pts);
            for (Vec2 p : pts)
                add(out, rq, SnapMode::Tangent, p, id);
        }
    }
}

// An ellipse or spline can run for kilometres through a drawing and be flattened into a thousand
// pieces to intersect it, while only the stretch inside the aperture matters. The parts of the
// curve that come within the aperture box, as sub-curves (a coarse flattening finds them).
static void clipToAperture(const geom::CurveRef& c, const BBox2& ap,
                           std::deque<geom::CurveRef>& out,
                           std::vector<const geom::CurveRef*>& clipped) {
    using Kind = geom::CurveRef::Kind;
    if (c.kind() != Kind::Ellipse && c.kind() != Kind::Spline) {
        clipped.push_back(&c);
        return;
    }
    const double tol = std::max(1e-3, (ap.max.x - ap.min.x) * 0.05);
    std::vector<Vec2> pts;
    std::vector<double> ts;
    geom::flatten(c, tol, pts, &ts);
    const BBox2 grown = ap.inflated(2 * tol);
    std::size_t i = 0;
    const std::size_t pieces = pts.size() < 2 ? 0 : pts.size() - 1;
    while (i < pieces) {
        auto hit = [&](std::size_t k) {
            BBox2 b;
            b.expand(pts[k]);
            b.expand(pts[k + 1]);
            return b.inflated(2 * tol).intersects(grown);
        };
        if (!hit(i)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j + 1 < pieces && hit(j + 1))
            ++j;
        // One piece of slack each side, so a crossing at a joint is kept.
        const std::size_t lo = i > 0 ? i - 1 : 0, hi = std::min(pieces, j + 2);
        if (lo == 0 && hi == pieces) {
            clipped.push_back(&c);
            return;
        }
        auto sub = c.subcurve(ts[lo], ts[hi]);
        if (sub.ok()) {
            out.push_back(sub.value());
            clipped.push_back(&out.back());
        }
        i = j + 1;
    }
}

std::vector<SnapCandidate> findSnapCandidates(const Document& doc, const index::SpatialIndex& index,
                                              const SnapRequest& rq,
                                              const SnapGeometryRegistry& providers) {
    std::vector<SnapCandidate> out;
    if (rq.modes == 0 || !(rq.apertureMm > 0))
        return out;

    // Entities whose geometry reaches the aperture, nearest first, capped. The index finds the
    // ones whose box does; a long diagonal line has a huge box, so each is measured for real
    // before any snap geometry is built.
    // Zoomed far out the aperture can hold thousands of boxes; only the nearest few by box are
    // measured.
    std::vector<index::Neighbor> near =
        index.nearest(rq.cursor, std::max<std::size_t>(4 * rq.maxEntities, 256), rq.apertureMm);
    {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < near.size(); ++i) {
            const Entity* e = doc.find(near[i].id);
            if (!e || !e->common.visible)
                continue;
            const double d = reach(doc, providers, *e, rq.cursor, near[i].distance);
            if (d > rq.apertureMm)
                continue;
            near[kept++] = {near[i].id, d};
        }
        near.resize(kept);
    }
    const auto byDistance = [](const index::Neighbor& a, const index::Neighbor& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.id < b.id;
    };
    if (near.size() > rq.maxEntities) {
        std::nth_element(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(rq.maxEntities),
                         near.end(), byDistance);
        near.resize(rq.maxEntities);
    }
    std::sort(near.begin(), near.end(), byDistance);

    struct Item {
        EntityId id;
        SnapGeometry geometry;
    };
    std::vector<Item> items;
    for (const index::Neighbor& n : near) {
        const Entity* e = doc.find(n.id);
        if (!e || !e->common.visible || !doc.effective(e->common).live())
            continue;
        Item item{n.id, {}};
        if (!providers.collect(doc, *e, item.geometry))
            continue;
        const SnapGeometry& g = item.geometry;
        for (Vec2 p : g.endpoints)
            add(out, rq, SnapMode::Endpoint, p, n.id);
        for (Vec2 p : g.midpoints)
            add(out, rq, SnapMode::Midpoint, p, n.id);
        for (Vec2 p : g.centers)
            add(out, rq, SnapMode::Center, p, n.id);
        for (Vec2 p : g.quadrants)
            add(out, rq, SnapMode::Quadrant, p, n.id);
        for (Vec2 p : g.geometricCenters)
            add(out, rq, SnapMode::GeometricCenter, p, n.id);
        for (Vec2 p : g.nodes)
            add(out, rq, SnapMode::Node, p, n.id);
        for (Vec2 p : g.insertions)
            add(out, rq, SnapMode::Insertion, p, n.id);
        for (const geom::CurveRef& c : g.curves)
            curveSnapPoints(c, rq, out, n.id);
        items.push_back(std::move(item));
    }

    // Intersections are worked out among the nearest few objects only: that is where the wanted
    // one is, and every pair costs.
    constexpr std::size_t kIntersectItems = 40;
    if (rq.modes & maskOf(SnapMode::Intersection)) {
        // Only curves that pass through the aperture can produce an intersection inside it. Of
        // those, only pairs whose boxes overlap are intersected (a sweep along x).
        struct Cand {
            std::size_t item;
            const geom::CurveRef* curve;
            BBox2 box;
        };
        std::vector<Cand> curves;
        const BBox2 ap{{rq.cursor.x - rq.apertureMm, rq.cursor.y - rq.apertureMm},
                       {rq.cursor.x + rq.apertureMm, rq.cursor.y + rq.apertureMm}};
        std::deque<geom::CurveRef> pieces;  // clipped copies; the pointers below stay valid
        std::vector<const geom::CurveRef*> clipped;
        for (std::size_t i = 0; i < std::min(items.size(), kIntersectItems); ++i)
            for (const geom::CurveRef& whole : items[i].geometry.curves) {
                clipped.clear();
                clipToAperture(whole, ap, pieces, clipped);
                for (const geom::CurveRef* c : clipped) {
                    const BBox2 box = relevantBox(*c, ap);
                    if (!box.empty() && box.min.x <= box.max.x && box.min.y <= box.max.y)
                        curves.push_back({i, c, box});
                }
            }
        std::sort(curves.begin(), curves.end(),
                  [](const Cand& a, const Cand& b) { return a.box.min.x < b.box.min.x; });
        std::vector<geom::Intersection> hits;
        auto crossings = [&](const Cand& x, const Cand& y) {
            hits.clear();
            geom::intersect(*x.curve, *y.curve, hits);
            for (const geom::Intersection& h : hits)
                add(out, rq, SnapMode::Intersection, h.p, items[x.item].id, items[y.item].id);
        };
        // Lines, arcs and line-ellipse pairs have closed forms; the rest (splines, two ellipses)
        // are worked out numerically, which costs a hundred times more. Of those only the pairs
        // among the nearest objects are done, so a crowd of splines cannot stall the cursor.
        using Kind = geom::CurveRef::Kind;
        auto closedForm = [](const Cand& x, const Cand& y) {
            const Kind a = x.curve->kind(), b = y.curve->kind();
            if (a == Kind::Spline || b == Kind::Spline)
                return false;
            if (a == Kind::Ellipse && b == Kind::Ellipse)
                return false;
            return true;
        };
        std::vector<std::pair<std::size_t, std::pair<std::size_t, std::size_t>>> heavy;
        for (std::size_t a = 0; a < curves.size(); ++a)
            for (std::size_t b = a + 1;
                 b < curves.size() && curves[b].box.min.x <= curves[a].box.max.x; ++b) {
                if (curves[a].item == curves[b].item)
                    continue;
                if (curves[b].box.min.y > curves[a].box.max.y ||
                    curves[a].box.min.y > curves[b].box.max.y)
                    continue;
                if (closedForm(curves[a], curves[b]))
                    crossings(curves[a], curves[b]);
                else
                    heavy.push_back({std::max(curves[a].item, curves[b].item), {a, b}});
            }
        constexpr std::size_t kNumericPairs = 16;
        if (heavy.size() > kNumericPairs) {
            std::nth_element(heavy.begin(), heavy.begin() + kNumericPairs, heavy.end(),
                             [](const auto& l, const auto& r) { return l.first < r.first; });
            heavy.resize(kNumericPairs);
        }
        for (const auto& h : heavy)
            crossings(curves[h.second.first], curves[h.second.second]);
    }

    if (rq.modes & maskOf(SnapMode::ApparentIntersection)) {
        // Where two objects that pass near the cursor would meet if they were long enough: a line
        // runs on as an unbounded line, an arc as a full circle. A real crossing is the
        // Intersection mode's business.
        struct Cand {
            std::size_t item;
            const geom::CurveRef* curve;
        };
        std::vector<Cand> curves;
        // Only the few nearest objects: this is for a corner that does not quite close.
        constexpr std::size_t kApparentItems = 12;
        for (std::size_t i = 0; i < std::min(items.size(), kApparentItems); ++i)
            for (const geom::CurveRef& c : items[i].geometry.curves)
                if ((c.kind() == geom::CurveRef::Kind::Line ||
                     c.kind() == geom::CurveRef::Kind::Arc) &&
                    c.distanceTo(rq.cursor) <= 2 * rq.apertureMm)
                    curves.push_back({i, &c});
        std::vector<geom::Intersection> hits;
        for (std::size_t a = 0; a < curves.size(); ++a)
            for (std::size_t b = a + 1; b < curves.size(); ++b) {
                if (curves[a].item == curves[b].item)
                    continue;
                hits.clear();
                geom::intersect(*curves[a].curve, *curves[b].curve, hits, true, true);
                for (const geom::Intersection& h : hits) {
                    if (distance(h.p, rq.cursor) > rq.apertureMm)
                        continue;
                    const double eps = 1e-9 * (1 + std::abs(h.p.x) + std::abs(h.p.y));
                    if (curves[a].curve->distanceTo(h.p) <= eps &&
                        curves[b].curve->distanceTo(h.p) <= eps)
                        continue;
                    add(out, rq, SnapMode::ApparentIntersection, h.p, items[curves[a].item].id,
                        items[curves[b].item].id);
                }
            }
    }

    {
        // Total order, so the cut below does not depend on the sort implementation.
        const auto closer = [](const SnapCandidate& a, const SnapCandidate& b) {
            if (a.distance != b.distance)
                return a.distance < b.distance;
            if (a.mode != b.mode)
                return a.mode < b.mode;
            if (a.entity != b.entity)
                return a.entity < b.entity;
            return a.point.x != b.point.x ? a.point.x < b.point.x : a.point.y < b.point.y;
        };
        if (out.size() > rq.maxCandidates) {
            // Keep the best few, plus whatever ties with the last one so the mode order below
            // still decides between equals.
            const auto cut = out.begin() + static_cast<std::ptrdiff_t>(rq.maxCandidates);
            std::nth_element(out.begin(), cut, out.end(), closer);
            const double limit = cut->distance + kTieMm;
            std::size_t kept = 0;
            for (std::size_t k = 0; k < out.size(); ++k)
                if (out[k].distance <= limit)
                    out[kept++] = out[k];
            out.resize(kept);
        }
        std::sort(out.begin(), out.end(), closer);
    }
    // Candidates within a hair of each other in distance: the mode order decides.
    for (std::size_t i = 0; i < out.size();) {
        std::size_t j = i + 1;
        while (j < out.size() && out[j].distance - out[i].distance <= kTieMm)
            ++j;
        std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(i),
                         out.begin() + static_cast<std::ptrdiff_t>(j),
                         [](const SnapCandidate& a, const SnapCandidate& b) {
                             return static_cast<int>(a.mode) < static_cast<int>(b.mode);
                         });
        i = j;
    }
    // The same point reached through two entities (a shared endpoint) is listed once per mode.
    // Equal points are equally far from the cursor, so only the tail of the sorted list needs to
    // be looked at.
    std::vector<SnapCandidate> unique;
    unique.reserve(out.size());
    for (const SnapCandidate& c : out) {
        bool dup = false;
        for (std::size_t k = unique.size();
             k-- > 0 && c.distance - unique[k].distance <= 2 * kTieMm;)
            if (unique[k].mode == c.mode && distance(unique[k].point, c.point) <= kTieMm) {
                dup = true;
                break;
            }
        if (!dup)
            unique.push_back(c);
    }
    return unique;
}

}  // namespace tbcad::drafting

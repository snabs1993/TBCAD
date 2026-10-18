#include "tbcad/drafting/osnap.h"

#include <algorithm>
#include <cmath>

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

std::vector<SnapCandidate> findSnapCandidates(const Document& doc, const index::SpatialIndex& index,
                                              const SnapRequest& rq,
                                              const SnapGeometryRegistry& providers) {
    std::vector<SnapCandidate> out;
    if (rq.modes == 0 || !(rq.apertureMm > 0))
        return out;

    // Entities whose box reaches the aperture, nearest first, capped.
    std::vector<index::Neighbor> near;
    index.queryPoint(rq.cursor, rq.apertureMm, [&](EntityId id, const BBox2& box) {
        double dx = std::max({box.min.x - rq.cursor.x, 0.0, rq.cursor.x - box.max.x});
        double dy = std::max({box.min.y - rq.cursor.y, 0.0, rq.cursor.y - box.max.y});
        near.push_back({id, std::hypot(dx, dy)});
    });
    std::stable_sort(
        near.begin(), near.end(),
        [](const index::Neighbor& a, const index::Neighbor& b) { return a.distance < b.distance; });
    if (near.size() > rq.maxEntities)
        near.resize(rq.maxEntities);

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
        for (Vec2 p : g.nodes)
            add(out, rq, SnapMode::Node, p, n.id);
        for (const geom::CurveRef& c : g.curves)
            curveSnapPoints(c, rq, out, n.id);
        items.push_back(std::move(item));
    }

    if (rq.modes & maskOf(SnapMode::Intersection)) {
        // Only curves that pass near the cursor can produce an intersection inside the aperture.
        struct Cand {
            std::size_t item;
            const geom::CurveRef* curve;
        };
        std::vector<Cand> curves;
        for (std::size_t i = 0; i < items.size(); ++i)
            for (const geom::CurveRef& c : items[i].geometry.curves)
                if (c.bounds().inflated(rq.apertureMm).contains(rq.cursor))
                    curves.push_back({i, &c});
        std::vector<geom::Intersection> hits;
        for (std::size_t a = 0; a < curves.size(); ++a)
            for (std::size_t b = a + 1; b < curves.size(); ++b) {
                if (curves[a].item == curves[b].item)
                    continue;
                hits.clear();
                geom::intersect(*curves[a].curve, *curves[b].curve, hits);
                for (const geom::Intersection& h : hits)
                    add(out, rq, SnapMode::Intersection, h.p, items[curves[a].item].id,
                        items[curves[b].item].id);
            }
    }

    std::stable_sort(out.begin(), out.end(), [](const SnapCandidate& a, const SnapCandidate& b) {
        return a.distance < b.distance;
    });
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
    std::vector<SnapCandidate> unique;
    for (const SnapCandidate& c : out) {
        bool dup = std::any_of(unique.begin(), unique.end(), [&](const SnapCandidate& u) {
            return u.mode == c.mode && distance(u.point, c.point) <= kTieMm;
        });
        if (!dup)
            unique.push_back(c);
    }
    return unique;
}

}  // namespace tbcad::drafting

#include <algorithm>
#include <cmath>

#include "extra.h"
#include "tbcad/geom/geom.h"

namespace tbcad::inquiry {

namespace {

struct Pair {
    double dist = 1e300;
    Vec2 a{}, b{};
};

std::vector<geom::CurveRef> curvesFor(const Entity& e) {
    std::vector<geom::CurveRef> c;
    if (!geom::curvesOf(e, c))
        c.clear();
    return c;
}

// Shortest span between the curves of two entities: samples one side and refines on the other.
Pair closestSpan(const std::vector<geom::CurveRef>& A, const std::vector<geom::CurveRef>& B) {
    Pair best;
    auto scan = [&](const std::vector<geom::CurveRef>& from, const std::vector<geom::CurveRef>& to,
                    bool swap) {
        for (const auto& ca : from) {
            std::vector<Vec2> pts;
            geom::flatten(ca, 0.05, pts);
            for (const Vec2& p : pts) {
                for (const auto& cb : to) {
                    const Vec2 q = cb.closestPoint(p);
                    const Vec2 p2 = ca.closestPoint(q);
                    const double d = distance(p2, q);
                    if (d < best.dist) {
                        best.dist = d;
                        best.a = swap ? q : p2;
                        best.b = swap ? p2 : q;
                    }
                }
            }
        }
    };
    scan(A, B, false);
    scan(B, A, true);
    return best;
}

}  // namespace

std::vector<Measurement> quickMeasure(const Document& doc, const std::vector<const Entity*>& nearby,
                                      Vec2 cursor, Precision precision) {
    std::vector<Measurement> out;
    struct Ranked {
        const Entity* e;
        double d;
        std::vector<geom::CurveRef> curves;
    };
    std::vector<Ranked> ranked;
    for (const Entity* e : nearby) {
        auto curves = curvesFor(*e);
        if (curves.empty())
            continue;
        double d = 1e300;
        for (const auto& c : curves)
            d = std::min(d, c.distanceTo(cursor));
        ranked.push_back({e, d, std::move(curves)});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& x, const Ranked& y) { return x.d < y.d; });
    if (ranked.size() > 2)
        ranked.resize(2);

    for (const Ranked& r : ranked) {
        Measurement m;
        if (const auto* l = entityAs<LineData>(*r.e)) {
            m.kind = Measurement::Kind::Length;
            m.a = l->a;
            m.b = l->b;
            m.value = distance(l->a, l->b);
            m.text = "Length " + formatLengthIn(doc, m.value, precision);
        } else if (const auto* c = entityAs<CircleData>(*r.e)) {
            m.kind = Measurement::Kind::Radius;
            m.a = c->center;
            m.b = r.curves.front().closestPoint(cursor);
            m.value = c->radius;
            m.text = "Radius " + formatLengthIn(doc, m.value, precision);
        } else if (const auto* a = entityAs<ArcData>(*r.e)) {
            m.kind = Measurement::Kind::Radius;
            m.a = a->center;
            m.b = r.curves.front().closestPoint(cursor);
            m.value = a->radius;
            m.text = "Radius " + formatLengthIn(doc, m.value, precision);
        } else if (const auto* pl = entityAs<PolylineData>(*r.e)) {
            // The straight segment nearest the cursor.
            double bestD = 1e300;
            const std::size_t n = pl->v.size();
            const std::size_t segs = pl->closed ? n : (n ? n - 1 : 0);
            for (std::size_t i = 0; i < segs; ++i) {
                const PolyVertex& v0 = pl->v[i];
                const Vec2 p1 = pl->v[(i + 1) % n].p;
                if (v0.bulge != 0)
                    continue;
                const auto seg = geom::CurveRef::line(v0.p, p1);
                const double d = seg.distanceTo(cursor);
                if (d < bestD) {
                    bestD = d;
                    m.kind = Measurement::Kind::Length;
                    m.a = v0.p;
                    m.b = p1;
                    m.value = distance(v0.p, p1);
                    m.text = "Length " + formatLengthIn(doc, m.value, precision);
                }
            }
            if (bestD > 1e299)
                continue;
        } else {
            continue;
        }
        out.push_back(std::move(m));
    }

    if (ranked.size() == 2) {
        const Pair p = closestSpan(ranked[0].curves, ranked[1].curves);
        if (p.dist > 1e-6 && p.dist < 1e299) {
            Measurement m;
            m.kind = Measurement::Kind::Distance;
            m.a = p.a;
            m.b = p.b;
            m.value = p.dist;
            m.text = "Distance " + formatLengthIn(doc, p.dist, precision);
            out.push_back(std::move(m));
        }
    }
    return out;
}

}  // namespace tbcad::inquiry

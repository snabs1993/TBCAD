#include "tbcad/hatch/fill.h"

#include <algorithm>
#include <cmath>

#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"

namespace tbcad::hatch {

using geom::CurveRef;

namespace {

double sweepBetween(double from, double to) {
    double s = normalizeAngle(to - from);
    return s < 1e-12 ? kTwoPi : s;  // equal angles mean a full turn
}

}  // namespace

std::vector<CurveRef> loopCurves(const HatchLoop& loop) {
    std::vector<CurveRef> out;
    for (const HatchEdge& edge : loop.edges) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, HatchLineEdge>) {
                    if (distance(e.a, e.b) > 0)
                        out.push_back(CurveRef::line(e.a, e.b));
                } else if constexpr (std::is_same_v<T, HatchArcEdge>) {
                    if (e.radius > 0) {
                        double from = e.ccw ? e.startAngle : e.endAngle;
                        double to = e.ccw ? e.endAngle : e.startAngle;
                        out.push_back(CurveRef::arc(e.center, e.radius, normalizeAngle(from),
                                                    sweepBetween(from, to)));
                    }
                } else if constexpr (std::is_same_v<T, HatchEllipseEdge>) {
                    double from = e.ccw ? e.startParam : e.endParam;
                    double to = e.ccw ? e.endParam : e.startParam;
                    out.push_back(CurveRef::ellipse(e.center, e.majorAxis, e.ratio,
                                                    normalizeAngle(from), sweepBetween(from, to)));
                } else if constexpr (std::is_same_v<T, HatchSplineEdge>) {
                    auto c = CurveRef::spline(e.spline);
                    if (c.ok())
                        out.push_back(std::move(c).value());
                } else {
                    PolylineData pl;
                    pl.v = e.v;
                    pl.closed = e.closed;
                    for (const geom::Segment& s : geom::segments(pl))
                        out.push_back(s.curve);
                }
            },
            edge);
    }
    return out;
}

namespace {

void appendPoint(std::vector<Vec2>& ring, Vec2 p) {
    if (ring.empty() || distance(ring.back(), p) > 1e-9)
        ring.push_back(p);
}

void appendFlattened(const CurveRef& c, bool reversed, double tol, std::vector<Vec2>& ring) {
    std::vector<Vec2> pts;
    geom::flatten(c, tol, pts);
    if (reversed)
        std::reverse(pts.begin(), pts.end());
    for (const Vec2& p : pts)
        appendPoint(ring, p);
}

// Points of one edge in the direction the loop travels it.
void appendEdge(const HatchEdge& edge, double tol, std::vector<Vec2>& ring) {
    HatchLoop one;
    one.edges.push_back(edge);
    std::visit(
        [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, HatchLineEdge>) {
                appendPoint(ring, e.a);
                appendPoint(ring, e.b);
            } else if constexpr (std::is_same_v<T, HatchArcEdge> ||
                                 std::is_same_v<T, HatchEllipseEdge>) {
                for (const CurveRef& c : loopCurves(one))
                    appendFlattened(c, !e.ccw, tol, ring);
            } else if constexpr (std::is_same_v<T, HatchSplineEdge>) {
                for (const CurveRef& c : loopCurves(one))
                    appendFlattened(c, false, tol, ring);
            } else {
                PolylineData pl;
                pl.v = e.v;
                pl.closed = e.closed;
                for (const geom::Segment& seg : geom::segments(pl))
                    appendFlattened(seg.curve, seg.reversed, tol, ring);
            }
        },
        edge);
}

}  // namespace

std::vector<std::vector<Vec2>> loopRings(const HatchData& hatch, double tolerance) {
    std::vector<std::vector<Vec2>> rings;
    for (const HatchLoop& loop : hatch.loops) {
        std::vector<Vec2> ring;
        for (const HatchEdge& edge : loop.edges)
            appendEdge(edge, tolerance, ring);
        while (ring.size() > 1 && distance(ring.front(), ring.back()) <= 1e-9)
            ring.pop_back();
        if (ring.size() >= 3)
            rings.push_back(std::move(ring));
    }
    return rings;
}

double hatchArea(const HatchData& hatch) {
    const BBox2 box = hatchBounds(hatch);
    if (box.empty())
        return 0;
    const double tol = std::max(box.width(), box.height()) * 1e-5;
    const auto rings = loopRings(hatch, tol > 0 ? tol : 1e-6);
    auto signedArea = [](const std::vector<Vec2>& r) {
        double a = 0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            const Vec2& p = r[i];
            const Vec2& q = r[(i + 1) % r.size()];
            a += p.x * q.y - q.x * p.y;
        }
        return a / 2;
    };
    auto contains = [](const std::vector<Vec2>& r, Vec2 pt) {
        bool in = false;
        for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            if ((r[i].y > pt.y) != (r[j].y > pt.y) &&
                pt.x < (r[j].x - r[i].x) * (pt.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
                in = !in;
        return in;
    };
    double total = 0;
    for (std::size_t i = 0; i < rings.size(); ++i) {
        int depth = 0;
        for (std::size_t j = 0; j < rings.size(); ++j)
            if (j != i && contains(rings[j], rings[i][0]))
                ++depth;
        total += (depth % 2 == 0 ? 1 : -1) * std::fabs(signedArea(rings[i]));
    }
    return std::max(total, 0.0);
}

namespace {

// Range of line indices [lo, hi] whose line may meet a box. `spacing` is signed.
void lineRange(const BBox2& box, Vec2 origin, Vec2 n, double spacing, long long& lo,
               long long& hi) {
    double cmin = 1e300, cmax = -1e300;
    for (Vec2 c : {Vec2{box.min.x, box.min.y}, Vec2{box.max.x, box.min.y},
                   Vec2{box.max.x, box.max.y}, Vec2{box.min.x, box.max.y}}) {
        double v = dot(c - origin, n);
        cmin = std::min(cmin, v);
        cmax = std::max(cmax, v);
    }
    double a = cmin / spacing, b = cmax / spacing;
    if (a > b)
        std::swap(a, b);
    lo = static_cast<long long>(std::ceil(a - 1e-9));
    hi = static_cast<long long>(std::floor(b + 1e-9));
}

struct EdgeRange {
    const CurveRef* curve;
    long long lo, hi;
};

struct Emitter {
    FillResult& out;
    std::size_t maxSegments;
    bool overflow = false;

    bool full() const { return out.segments.size() + out.dots.size() >= maxSegments; }
};

// Draws the interval [s0, s1] of the line through q with direction u, applying the dash cycle
// (anchored at s = 0).
void emitInterval(Emitter& em, const PatternFamily& f, double scale, Vec2 q, Vec2 u, double s0,
                  double s1) {
    if (s1 - s0 < 1e-9)
        return;
    if (f.dashes.empty()) {
        if (em.full()) {
            em.overflow = true;
            return;
        }
        em.out.segments.emplace_back(q + u * s0, q + u * s1);
        return;
    }
    double cycle = f.cycleLength() * scale;
    double cycles = (s1 - s0) / cycle + 2;
    double visible = static_cast<double>(
        std::count_if(f.dashes.begin(), f.dashes.end(), [](double d) { return d >= 0; }));
    if (cycles * visible + static_cast<double>(em.out.segments.size() + em.out.dots.size()) >
        static_cast<double>(em.maxSegments) * 1.5) {
        em.overflow = true;
        return;
    }
    double base = std::floor(s0 / cycle) * cycle;
    for (; base <= s1 && !em.overflow; base += cycle) {
        double pos = base;
        for (double d : f.dashes) {
            double len = std::abs(d) * scale;
            if (d > 0) {
                double a = std::max(pos, s0), b = std::min(pos + len, s1);
                if (b - a > 1e-9) {
                    if (em.full()) {
                        em.overflow = true;
                        return;
                    }
                    em.out.segments.emplace_back(q + u * a, q + u * b);
                }
            } else if (d == 0) {
                if (pos >= s0 && pos <= s1) {
                    if (em.full()) {
                        em.overflow = true;
                        return;
                    }
                    em.out.dots.push_back(q + u * pos);
                }
            }
            pos += len;
        }
    }
}

// Parameters (distance along u from q) where the infinite line meets the active edges. Sets
// `ambiguous` when the line runs through an end point of an edge or touches one tangentially, in
// which case the even-odd pairing of the parameters cannot be trusted.
std::vector<double> crossings(Vec2 q, Vec2 u, const std::vector<const CurveRef*>& active,
                              double eps, bool& ambiguous) {
    std::vector<double> s;
    std::vector<geom::Intersection> hits;
    CurveRef line = CurveRef::line(q, q + u);
    ambiguous = false;
    for (const CurveRef* c : active) {
        hits.clear();
        geom::intersect(line, *c, hits, true, false);
        for (const geom::Intersection& h : hits) {
            s.push_back(h.tA);
            if (distance(h.p, c->startPoint()) <= eps || distance(h.p, c->endPoint()) <= eps)
                ambiguous = true;
        }
    }
    std::sort(s.begin(), s.end());
    if (s.size() % 2 != 0)
        ambiguous = true;
    return s;
}

}  // namespace

FillResult generateFill(const HatchData& hatch, const Pattern& patternIn, const FillOptions& opt) {
    FillResult res;
    // A double hatch draws every family again, turned a quarter turn about the hatch origin.
    Pattern doubledPattern;
    if (hatch.doubled && !hatch.solid && !patternIn.solid) {
        doubledPattern = patternIn;
        for (const PatternFamily& f : patternIn.families) {
            PatternFamily g = f;
            g.angle += 1.5707963267948966;
            g.origin = {-f.origin.y, f.origin.x};
            g.delta = f.delta;  // (along, perpendicular) turn with the line
            doubledPattern.families.push_back(std::move(g));
        }
    }
    const Pattern& pattern = doubledPattern.families.empty() ? patternIn : doubledPattern;
    if (pattern.solid || hatch.solid || pattern.families.empty()) {
        res.mode = FillResult::Mode::Solid;
        res.rings = loopRings(hatch, opt.tolerance);
        return res;
    }
    auto tone = [&](const char* why) {
        FillResult r;
        r.mode = FillResult::Mode::SolidTone;
        r.rings = loopRings(hatch, opt.tolerance);
        r.reason = why;
        return r;
    };

    std::vector<CurveRef> curves;
    BBox2 box;
    for (const HatchLoop& l : hatch.loops) {
        for (CurveRef& c : loopCurves(l)) {
            box.merge(c.bounds());
            curves.push_back(std::move(c));
        }
    }
    if (curves.empty() || box.empty()) {
        res.mode = FillResult::Mode::Lines;
        return res;
    }

    double scale = hatch.patternScale > 0 ? hatch.patternScale : 1;
    double cs = std::cos(hatch.patternAngle), sn = std::sin(hatch.patternAngle);
    auto rot = [&](Vec2 v) { return Vec2{cs * v.x - sn * v.y, sn * v.x + cs * v.y}; };

    // Pre-check every family so that the cheap fallbacks happen before any real work.
    std::size_t totalLines = 0;
    for (const PatternFamily& f : pattern.families) {
        double spacing = std::abs(f.delta.y) * scale;
        if (!(spacing > 1e-9))
            continue;
        if (opt.pixelSizeMm > 0 && spacing < opt.minSpacingPx * opt.pixelSizeMm)
            return tone("too dense");
        double span = std::max(box.width(), box.height()) * 1.5;
        totalLines += static_cast<std::size_t>(span / spacing) + 2;
        if (totalLines > opt.maxLines)
            return tone("too many lines");
    }

    FillResult lines;
    lines.mode = FillResult::Mode::Lines;
    Emitter em{lines, opt.maxSegments};
    for (const PatternFamily& f : pattern.families) {
        double spacing = f.delta.y * scale;
        if (!(std::abs(spacing) > 1e-9))
            continue;
        double a = f.angle + hatch.patternAngle;
        Vec2 u{std::cos(a), std::sin(a)};
        Vec2 n = perp(u);
        Vec2 origin = hatch.patternOrigin + rot(f.origin) * scale;
        double shift = f.delta.x * scale;

        long long kLo = 0, kHi = -1;
        lineRange(box, origin, n, spacing, kLo, kHi);
        if (kHi < kLo)
            continue;
        std::vector<EdgeRange> ranges;
        ranges.reserve(curves.size());
        for (const CurveRef& c : curves) {
            EdgeRange r{&c, 0, -1};
            lineRange(c.bounds(), origin, n, spacing, r.lo, r.hi);
            if (r.hi >= r.lo)
                ranges.push_back(r);
        }
        std::sort(ranges.begin(), ranges.end(),
                  [](const EdgeRange& x, const EdgeRange& y) { return x.lo < y.lo; });
        std::size_t next = 0;
        std::vector<EdgeRange> live;
        std::vector<const CurveRef*> active;
        double eps = std::max(1e-7, std::abs(spacing) * 1e-9);

        for (long long k = kLo; k <= kHi && !em.overflow; ++k) {
            while (next < ranges.size() && ranges[next].lo <= k)
                live.push_back(ranges[next++]);
            live.erase(std::remove_if(live.begin(), live.end(),
                                      [k](const EdgeRange& r) { return r.hi < k; }),
                       live.end());
            if (live.empty())
                continue;
            active.clear();
            for (const EdgeRange& r : live)
                active.push_back(r.curve);

            double kd = static_cast<double>(k);
            Vec2 q = origin + n * (kd * spacing) + u * (kd * shift);
            bool ambiguous = false;
            std::vector<double> s = crossings(q, u, active, eps, ambiguous);
            // A line through a vertex or tangent to an arc: nudge it sideways by a hair.
            for (int attempt = 1; ambiguous && attempt <= 4; ++attempt) {
                double nudge = std::abs(spacing) * 1e-5 * attempt * (attempt % 2 ? 1 : -1);
                s = crossings(q + n * nudge, u, active, eps, ambiguous);
            }
            if (ambiguous && s.size() % 2 != 0)
                continue;  // give up on this one line rather than pair wrongly
            for (std::size_t i = 0; i + 1 < s.size() && !em.overflow; i += 2)
                emitInterval(em, f, scale, q, u, s[i], s[i + 1]);
        }
        if (em.overflow)
            return tone("too many segments");
    }
    return lines;
}

}  // namespace tbcad::hatch

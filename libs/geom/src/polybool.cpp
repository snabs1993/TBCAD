#include "tbcad/geom/polybool.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace tbcad::geom {

namespace {

double ringArea(const Ring& r) {
    // Measured from the first vertex: with survey-sized coordinates (1e9 mm) the products of
    // absolute coordinates would have lost every digit of an area of a few thousand mm2.
    double a = 0;
    for (std::size_t i = 1; i + 1 < r.size(); ++i)
        a += cross(r[i] - r[0], r[i + 1] - r[0]);
    return 0.5 * a;
}

double distToSegment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double len2 = dot(ab, ab);
    const double t = len2 > 0 ? std::clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    return std::hypot(p.x - (a.x + ab.x * t), p.y - (a.y + ab.y * t));
}

bool parityInside(const std::vector<Ring>& rings, Vec2 p) {
    bool in = false;
    for (const Ring& r : rings) {
        for (std::size_t i = 0; i < r.size(); ++i) {
            const Vec2 a = r[i], b = r[(i + 1) % r.size()];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                in = !in;
        }
    }
    return in;
}

Ring cleaned(const Ring& in, double eps) {
    Ring r;
    for (const Vec2& p : in)
        if (r.empty() || distance(r.back(), p) > eps)
            r.push_back(p);
    while (r.size() > 1 && distance(r.front(), r.back()) <= eps)
        r.pop_back();
    // Drop spikes and collinear middle vertices until nothing changes.
    for (bool changed = true; changed && r.size() >= 3;) {
        changed = false;
        for (std::size_t i = 0; i < r.size() && r.size() >= 3; ++i) {
            const Vec2 a = r[(i + r.size() - 1) % r.size()], b = r[i], c = r[(i + 1) % r.size()];
            const double len = std::max(distance(a, c), eps);
            if (std::fabs(cross(b - a, c - a)) / len <= eps) {
                r.erase(r.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                --i;
            }
        }
    }
    return r;
}

struct Edge {
    Vec2 a{}, b{};
    int owner = 0;  // 0 = region A, 1 = region B
};

// Parameters t in (0,1) at which segment a-b meets the other segment, including the ends of a
// collinear overlap.
void splitParams(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double eps, std::vector<double>& ts) {
    const Vec2 r = b - a, s = d - c;
    const double rl = std::hypot(r.x, r.y);
    if (rl <= eps)
        return;
    const double den = cross(r, s);
    const double sl = std::hypot(s.x, s.y);
    if (std::fabs(den) > 1e-12 * rl * std::max(sl, 1e-300)) {
        const double t = cross(c - a, s) / den;
        const double u = cross(c - a, r) / den;
        const double slack = eps / rl;
        const double uslack = eps / std::max(sl, eps);
        if (t > slack && t < 1 - slack && u >= -uslack && u <= 1 + uslack)
            ts.push_back(t);
        return;
    }
    // Parallel: collinear when c is on the line.
    if (std::fabs(cross(c - a, r)) / rl > eps)
        return;
    for (Vec2 p : {c, d}) {
        const double t = dot(p - a, r) / (rl * rl);
        if (t > eps / rl && t < 1 - eps / rl)
            ts.push_back(t);
    }
}

std::vector<Edge> splitEdges(const std::vector<Ring>& mine, const std::vector<Ring>& other,
                             int owner, double eps) {
    std::vector<Edge> out;
    for (const Ring& r : mine) {
        for (std::size_t i = 0; i < r.size(); ++i) {
            const Vec2 a = r[i], b = r[(i + 1) % r.size()];
            std::vector<double> ts;
            for (const Ring& o : other)
                for (std::size_t k = 0; k < o.size(); ++k)
                    splitParams(a, b, o[k], o[(k + 1) % o.size()], eps, ts);
            std::sort(ts.begin(), ts.end());
            Vec2 prev = a;
            for (double t : ts) {
                const Vec2 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (distance(prev, p) > eps) {
                    out.push_back({prev, p, owner});
                    prev = p;
                }
            }
            if (distance(prev, b) > eps)
                out.push_back({prev, b, owner});
        }
    }
    return out;
}

enum class Where { Out, In, SameDir, OppositeDir };

// Where an edge of one region lies relative to the other (normalised) region.
Where classify(const Edge& e, const std::vector<Ring>& other, double eps) {
    const Vec2 mid{(e.a.x + e.b.x) / 2, (e.a.y + e.b.y) / 2};
    double best = eps;
    Vec2 dir{};
    bool on = false;
    for (const Ring& r : other) {
        for (std::size_t i = 0; i < r.size(); ++i) {
            const Vec2 a = r[i], b = r[(i + 1) % r.size()];
            const double d = distToSegment(mid, a, b);
            if (d <= best) {
                // Only edges that run along ours count as shared; one that merely passes
                // through the midpoint at an angle does not (the midpoint is then not on it).
                const Vec2 u = b - a, v = e.b - e.a;
                const double ul = std::hypot(u.x, u.y), vl = std::hypot(v.x, v.y);
                if (ul > 0 && vl > 0 && std::fabs(cross(u, v)) / (ul * vl) < 1e-6) {
                    best = d;
                    dir = u;
                    on = true;
                }
            }
        }
    }
    if (on)
        return dot(dir, e.b - e.a) > 0 ? Where::SameDir : Where::OppositeDir;
    return parityInside(other, mid) ? Where::In : Where::Out;
}

struct Canon {
    double eps;
    std::map<std::pair<long long, long long>, std::vector<Vec2>> grid;
    Vec2 get(Vec2 p) {
        const long long cx = std::llround(p.x / (eps * 4)), cy = std::llround(p.y / (eps * 4));
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                auto it = grid.find({cx + dx, cy + dy});
                if (it == grid.end())
                    continue;
                for (const Vec2& q : it->second)
                    if (distance(p, q) <= eps * 2)
                        return q;
            }
        grid[{cx, cy}].push_back(p);
        return p;
    }
};

}  // namespace

Region normalized(Region region, double eps) {
    std::vector<Ring> rings;
    for (Ring& r : region.rings) {
        Ring c = cleaned(r, eps);
        if (c.size() >= 3 && std::fabs(ringArea(c)) > eps * eps)
            rings.push_back(std::move(c));
    }
    Region out;
    for (std::size_t i = 0; i < rings.size(); ++i) {
        int depth = 0;
        // A point of this ring that is not on another ring: the middle of its longest edge.
        const Ring& r = rings[i];
        std::size_t best = 0;
        double bestLen = -1;
        for (std::size_t k = 0; k < r.size(); ++k) {
            const double l = distance(r[k], r[(k + 1) % r.size()]);
            if (l > bestLen) {
                bestLen = l;
                best = k;
            }
        }
        const Vec2 probe{(r[best].x + r[(best + 1) % r.size()].x) / 2,
                         (r[best].y + r[(best + 1) % r.size()].y) / 2};
        for (std::size_t j = 0; j < rings.size(); ++j)
            if (j != i && parityInside({rings[j]}, probe))
                ++depth;
        Ring oriented = r;
        const bool ccw = ringArea(oriented) > 0;
        if (ccw != (depth % 2 == 0))
            std::reverse(oriented.begin(), oriented.end());
        out.rings.push_back(std::move(oriented));
    }
    return out;
}

double area(const Region& region) {
    double a = 0;
    for (const Ring& r : normalized(region).rings)
        a += ringArea(r);
    return a;
}

bool contains(const Region& region, Vec2 p, double eps) {
    for (const Ring& r : region.rings)
        for (std::size_t i = 0; i < r.size(); ++i)
            if (distToSegment(p, r[i], r[(i + 1) % r.size()]) <= eps)
                return true;
    return parityInside(region.rings, p);
}

namespace {

Region booleanLocal(const Region& ina, const Region& inb, BoolOp op, double eps);

}  // namespace

// The work is done around a local origin near the shapes: coordinates of 1e9 mm leave a double
// only 2e-7 mm, and the tolerances and cross products below need much more than that.
Region boolean(const Region& ina, const Region& inb, BoolOp op, double eps) {
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const Region* r : {&ina, &inb})
        for (const Ring& ring : r->rings)
            for (Vec2 p : ring) {
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
    if (minX > maxX)
        return booleanLocal(ina, inb, op, eps);
    const Vec2 origin{std::round((minX + maxX) / 2), std::round((minY + maxY) / 2)};
    if (std::fabs(origin.x) < 1e5 && std::fabs(origin.y) < 1e5)
        return booleanLocal(ina, inb, op, eps);
    auto shift = [&](const Region& in, Vec2 by) {
        Region out;
        for (const Ring& ring : in.rings) {
            Ring r;
            r.reserve(ring.size());
            for (Vec2 p : ring)
                r.push_back(p + by);
            out.rings.push_back(std::move(r));
        }
        return out;
    };
    return shift(booleanLocal(shift(ina, {-origin.x, -origin.y}),
                              shift(inb, {-origin.x, -origin.y}), op, eps),
                 origin);
}

namespace {

Region booleanLocal(const Region& ina, const Region& inb, BoolOp op, double eps) {
    const Region a = normalized(ina, eps), b = normalized(inb, eps);
    if (a.rings.empty() || b.rings.empty()) {
        if (op == BoolOp::Union)
            return a.rings.empty() ? b : a;
        if (op == BoolOp::Subtract)
            return a;
        return {};
    }
    std::vector<Edge> ea = splitEdges(a.rings, b.rings, 0, eps);
    std::vector<Edge> eb = splitEdges(b.rings, a.rings, 1, eps);

    std::vector<Edge> keep;
    for (const Edge& e : ea) {
        const Where w = classify(e, b.rings, eps);
        bool k = false;
        switch (op) {
            case BoolOp::Union:
                k = w == Where::Out || w == Where::SameDir;
                break;
            case BoolOp::Intersect:
                k = w == Where::In || w == Where::SameDir;
                break;
            case BoolOp::Subtract:
                k = w == Where::Out || w == Where::OppositeDir;
                break;
        }
        if (k)
            keep.push_back(e);
    }
    for (const Edge& e : eb) {
        const Where w = classify(e, a.rings, eps);
        switch (op) {
            case BoolOp::Union:
                if (w == Where::Out)
                    keep.push_back(e);
                break;
            case BoolOp::Intersect:
                if (w == Where::In)
                    keep.push_back(e);
                break;
            case BoolOp::Subtract:
                if (w == Where::In)
                    keep.push_back({e.b, e.a, 1});  // the cut edge runs the other way round
                break;
        }
    }

    Canon canon{eps, {}};
    for (Edge& e : keep) {
        e.a = canon.get(e.a);
        e.b = canon.get(e.b);
    }
    std::vector<bool> used(keep.size(), false);
    Region out;
    for (std::size_t i = 0; i < keep.size(); ++i) {
        if (used[i])
            continue;
        Ring ring;
        std::size_t cur = i;
        const Vec2 start = keep[i].a;
        for (std::size_t guard = 0; guard <= keep.size(); ++guard) {
            used[cur] = true;
            ring.push_back(keep[cur].a);
            const Vec2 end = keep[cur].b;
            if (distance(end, start) <= eps)
                break;
            std::size_t next = keep.size();
            for (std::size_t k = 0; k < keep.size(); ++k)
                if (!used[k] && distance(keep[k].a, end) <= eps) {
                    next = k;
                    break;
                }
            if (next == keep.size())
                break;  // open chain: a degenerate result, dropped by normalisation
            cur = next;
        }
        if (ring.size() >= 3 && distance(keep[cur].b, start) <= eps)
            out.rings.push_back(std::move(ring));
    }
    return normalized(std::move(out), eps);
}

}  // namespace

}  // namespace tbcad::geom

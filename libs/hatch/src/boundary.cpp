#include "tbcad/hatch/boundary.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "tbcad/geom/geom.h"

namespace tbcad::hatch {

const char* boundaryErrorText(BoundaryError e) {
    switch (e) {
        case BoundaryError::NoClosedBoundary:
            return "Could not find a closed boundary around that point.";
        case BoundaryError::TooManyEdges:
            return "Too many objects in view to find a boundary; zoom in and try again.";
        case BoundaryError::NoGeometry:
            return "No usable objects to build a boundary from.";
    }
    return "";
}

namespace {

using geom::CurveRef;

constexpr double kMerge = 1e-5;  // end points closer than this are the same node (mm)

Error makeBoundaryError(BoundaryError e) {
    return makeError(e == BoundaryError::TooManyEdges ? ErrorCode::OutOfRange : ErrorCode::Generic,
                     boundaryErrorText(e));
}

// ---- Nodes ---------------------------------------------------------------------------------

class NodeTable {
public:
    int find(Vec2 p) {
        auto [ix, iy] = cellOf(p);
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                auto it = grid_.find(key(ix + dx, iy + dy));
                if (it == grid_.end())
                    continue;
                for (int n : it->second)
                    if (distance(pts_[static_cast<std::size_t>(n)], p) <= kMerge)
                        return n;
            }
        }
        int id = static_cast<int>(pts_.size());
        pts_.push_back(p);
        grid_[key(ix, iy)].push_back(id);
        return id;
    }
    const std::vector<Vec2>& points() const { return pts_; }

private:
    static constexpr double kCell = 4 * kMerge;
    static std::pair<std::int64_t, std::int64_t> cellOf(Vec2 p) {
        return {static_cast<std::int64_t>(std::floor(p.x / kCell)),
                static_cast<std::int64_t>(std::floor(p.y / kCell))};
    }
    static std::uint64_t key(std::int64_t ix, std::int64_t iy) {
        return static_cast<std::uint64_t>(ix) * 0x9E3779B97F4A7C15ull ^
               static_cast<std::uint64_t>(iy) * 0xC2B2AE3D27D4EB4Full;
    }
    std::vector<Vec2> pts_{};
    std::unordered_map<std::uint64_t, std::vector<int>> grid_{};
};

// ---- Graph ---------------------------------------------------------------------------------

struct Edge {
    int a = 0, b = 0;  // nodes at the start (t0) and end (t1) of the curve
    CurveRef curve{};
    int cand = 0;                          // index into the candidates
    const SplineData* spline = nullptr;    // set when the edge is a whole, unsplit spline
};

// Half-edge h = 2 * edge (a -> b) or 2 * edge + 1 (b -> a).
struct Graph {
    std::vector<Vec2> nodes{};
    std::vector<Edge> edges{};
    std::vector<std::vector<Vec2>> poly{};  // flattened, in curve direction
    std::vector<std::vector<int>> out{};    // per node: outgoing half-edges, counter-clockwise
    std::vector<int> pos{};                 // per half-edge: index in out[from]

    int from(int h) const { return (h & 1) ? edges[h >> 1].b : edges[h >> 1].a; }
    int to(int h) const { return (h & 1) ? edges[h >> 1].a : edges[h >> 1].b; }
};

struct Loop {
    std::vector<int> half{};  // half-edges, counter-clockwise
    double area = 0;
    BBox2 box{};
    std::vector<Vec2> ring{};     // flattened polygon (no repeated first point)
    std::vector<Vec2> samples{};  // few points on the loop for containment tests
};

struct LoopSet {
    Graph graph{};
    std::vector<Loop> loops{};
};

struct Piece {
    CurveRef curve{};
    int cand = 0;
};

Vec2 startDirection(const CurveRef& c, bool forward) {
    Vec2 t = forward ? c.tangentAt(c.t0()) : -c.tangentAt(c.t1());
    if (lengthSquared(t) > 0)
        return t;
    Vec2 chord = forward ? c.endPoint() - c.startPoint() : c.startPoint() - c.endPoint();
    return normalized(chord);
}

// Signed curvature-like measure to order edges that leave a node in the same direction:
// cross(t, d) / |d|^2 equals half the curvature for a circular arc, 0 for a line.
double bend(const CurveRef& c, bool forward, Vec2 tangent) {
    double len = c.length();
    if (!(len > 0))
        return 0;
    double s = len * 0.1;
    double u = c.paramAtLength(forward ? s : len - s);
    Vec2 d = c.pointAt(u) - (forward ? c.startPoint() : c.endPoint());
    double dd = dot(d, d);
    return dd > 0 ? cross(tangent, d) / dd : 0;
}

double ringArea(const std::vector<Vec2>& ring) {
    if (ring.size() < 3)
        return 0;
    double sum = 0;
    Vec2 o = ring[0];
    for (std::size_t i = 0; i + 1 < ring.size(); ++i)
        sum += cross(ring[i] - o, ring[i + 1] - o);
    return sum * 0.5;
}

Result<LoopSet> buildLoops(const std::vector<Candidate>& cands, const BoundaryOptions& opt) {
    // 1. Curve pieces of every candidate.
    std::vector<Piece> pieces;
    std::vector<CurveRef> tmp;
    for (std::size_t i = 0; i < cands.size(); ++i) {
        if (!cands[i].entity)
            continue;
        tmp.clear();
        if (!geom::curvesOf(*cands[i].entity, tmp))
            continue;
        for (CurveRef& c : tmp)
            if (c.length() > kMerge)
                pieces.push_back({std::move(c), static_cast<int>(i)});
    }
    if (pieces.empty())
        return makeBoundaryError(BoundaryError::NoGeometry);
    if (pieces.size() > opt.maxEdges * 2)
        return makeBoundaryError(BoundaryError::TooManyEdges);

    // 2. Intersections of every pair whose boxes overlap (sweep over x).
    std::vector<BBox2> boxes(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i)
        boxes[i] = pieces[i].curve.bounds().inflated(kMerge * 10);
    std::vector<int> order(pieces.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return boxes[static_cast<std::size_t>(a)].min.x <
                                         boxes[static_cast<std::size_t>(b)].min.x; });
    std::vector<std::vector<double>> splits(pieces.size());
    std::vector<geom::Intersection> hits;
    std::size_t hitCount = 0;
    for (std::size_t ii = 0; ii < order.size(); ++ii) {
        auto i = static_cast<std::size_t>(order[ii]);
        const BBox2& bi = boxes[i];
        for (std::size_t jj = ii + 1; jj < order.size(); ++jj) {
            auto j = static_cast<std::size_t>(order[jj]);
            const BBox2& bj = boxes[j];
            if (bj.min.x > bi.max.x)
                break;
            if (bj.min.y > bi.max.y || bj.max.y < bi.min.y)
                continue;
            hits.clear();
            geom::intersect(pieces[i].curve, pieces[j].curve, hits);
            for (const geom::Intersection& h : hits) {
                splits[i].push_back(h.tA);
                splits[j].push_back(h.tB);
            }
            hitCount += hits.size();
            // Every crossing adds an edge, so a crowded view can be refused without finishing.
            if (hitCount > opt.maxEdges * 2)
                return makeBoundaryError(BoundaryError::TooManyEdges);
        }
    }

    // 3. Split the pieces and make the edges; equal end points share a node.
    Graph g;
    NodeTable nodes;
    std::unordered_map<std::uint64_t, std::vector<int>> byPair;  // for duplicate removal
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const CurveRef& c = pieces[i].curve;
        std::vector<double> cuts;
        for (double p : splits[i]) {
            if (c.kind() == CurveRef::Kind::Arc || c.kind() == CurveRef::Kind::Ellipse)
                p = c.wrapParam(p);
            if (!c.containsParam(p, 0) || p <= c.t0() || p >= c.t1())
                continue;
            Vec2 at = c.pointAt(p);
            if (distance(at, c.startPoint()) <= kMerge || distance(at, c.endPoint()) <= kMerge)
                continue;
            cuts.push_back(p);
        }
        std::sort(cuts.begin(), cuts.end());
        std::vector<double> ts{c.t0()};
        for (double p : cuts)
            if (distance(c.pointAt(p), c.pointAt(ts.back())) > kMerge)
                ts.push_back(p);
        ts.push_back(c.t1());
        bool whole = ts.size() == 2;
        for (std::size_t k = 0; k + 1 < ts.size(); ++k) {
            CurveRef sub = c;
            if (!whole) {
                auto r = c.subcurve(ts[k], ts[k + 1]);
                if (!r.ok())
                    continue;
                sub = std::move(r).value();
            }
            Edge e;
            e.a = nodes.find(sub.startPoint());
            e.b = nodes.find(sub.endPoint());
            if (e.a == e.b && sub.length() < 10 * kMerge)
                continue;
            e.curve = sub;
            e.cand = pieces[i].cand;
            if (whole && c.kind() == CurveRef::Kind::Spline) {
                const Entity* ent = cands[static_cast<std::size_t>(e.cand)].entity;
                if (ent && kind(*ent) == EntityKind::Spline)
                    e.spline = entityAs<SplineData>(*ent);
            }
            // Drop exact duplicates (overlapping collinear lines, the same circle twice).
            std::uint64_t key = (static_cast<std::uint64_t>(std::min(e.a, e.b)) << 32) |
                                static_cast<std::uint32_t>(std::max(e.a, e.b));
            Vec2 mid = sub.pointAt((sub.t0() + sub.t1()) * 0.5);
            bool dup = false;
            for (int other : byPair[key]) {
                const Edge& o = g.edges[static_cast<std::size_t>(other)];
                if (distance(o.curve.pointAt((o.curve.t0() + o.curve.t1()) * 0.5), mid) <=
                    kMerge * 10) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            byPair[key].push_back(static_cast<int>(g.edges.size()));
            g.edges.push_back(std::move(e));
        }
    }
    if (g.edges.size() > opt.maxEdges)
        return makeBoundaryError(BoundaryError::TooManyEdges);
    g.nodes = nodes.points();

    // 4. Flatten and order the edges around every node.
    g.poly.resize(g.edges.size());
    for (std::size_t i = 0; i < g.edges.size(); ++i)
        geom::flatten(g.edges[i].curve, opt.flattenTolerance, g.poly[i]);
    g.out.resize(g.nodes.size());
    g.pos.assign(g.edges.size() * 2, 0);
    struct Key {
        double angle, bend;
    };
    std::vector<Key> keys(g.edges.size() * 2);
    for (std::size_t i = 0; i < g.edges.size(); ++i) {
        for (int dir = 0; dir < 2; ++dir) {
            bool forward = dir == 0;
            Vec2 t = startDirection(g.edges[i].curve, forward);
            double ang = std::atan2(t.y, t.x);
            if (ang < -kPi + 1e-9)
                ang += kTwoPi;
            auto h = static_cast<int>(2 * i + static_cast<std::size_t>(dir));
            keys[static_cast<std::size_t>(h)] = {ang, bend(g.edges[i].curve, forward, t)};
            g.out[static_cast<std::size_t>(g.from(h))].push_back(h);
        }
    }
    for (std::vector<int>& list : g.out) {
        std::sort(list.begin(), list.end(), [&](int x, int y) {
            const Key& kx = keys[static_cast<std::size_t>(x)];
            const Key& ky = keys[static_cast<std::size_t>(y)];
            if (std::abs(kx.angle - ky.angle) > 1e-9)
                return kx.angle < ky.angle;
            if (kx.bend != ky.bend)
                return kx.bend < ky.bend;
            return x < y;
        });
        for (std::size_t k = 0; k < list.size(); ++k)
            g.pos[static_cast<std::size_t>(list[k])] = static_cast<int>(k);
    }

    // 5. Walk the faces (the face lies to the left of every half-edge) and cut each walk into
    // simple cycles: a bridge or an antenna is traversed twice in one walk and falls out as a
    // zero-area cycle. Only counter-clockwise cycles are kept: the clockwise ones are the same
    // regions seen from the other side.
    LoopSet set;
    set.graph = std::move(g);
    const Graph& G = set.graph;
    std::vector<char> seen(G.edges.size() * 2, 0);
    std::vector<int> mark(G.nodes.size(), -1);
    std::vector<int> walk, stack;
    for (int start = 0; start < static_cast<int>(G.edges.size() * 2); ++start) {
        if (seen[static_cast<std::size_t>(start)])
            continue;
        walk.clear();
        int h = start;
        do {
            seen[static_cast<std::size_t>(h)] = 1;
            walk.push_back(h);
            int v = G.to(h);
            const std::vector<int>& list = G.out[static_cast<std::size_t>(v)];
            int p = G.pos[static_cast<std::size_t>(h ^ 1)];
            int n = static_cast<int>(list.size());
            h = list[static_cast<std::size_t>((p - 1 + n) % n)];
        } while (h != start);

        stack.clear();
        for (int w : walk) {
            mark[static_cast<std::size_t>(G.from(w))] = static_cast<int>(stack.size());
            stack.push_back(w);
            int end = G.to(w);
            int at = mark[static_cast<std::size_t>(end)];
            if (at < 0)
                continue;
            Loop loop;
            loop.half.assign(stack.begin() + at, stack.end());
            for (std::size_t k = static_cast<std::size_t>(at); k < stack.size(); ++k)
                mark[static_cast<std::size_t>(G.from(stack[k]))] = -1;
            stack.resize(static_cast<std::size_t>(at));
            for (int hh : loop.half) {
                const std::vector<Vec2>& pl = G.poly[static_cast<std::size_t>(hh >> 1)];
                if (hh & 1) {
                    for (std::size_t k = pl.size() - 1; k > 0; --k)
                        loop.ring.push_back(pl[k]);
                } else {
                    for (std::size_t k = 0; k + 1 < pl.size(); ++k)
                        loop.ring.push_back(pl[k]);
                }
            }
            loop.area = ringArea(loop.ring);
            if (loop.area <= 1e-9)
                continue;
            for (const Vec2& p : loop.ring)
                loop.box.expand(p);
            // Samples: the nodes and the middle of every edge, thinned to a few.
            std::size_t nh = loop.half.size();
            std::size_t step = std::max<std::size_t>(1, nh / 16);
            for (std::size_t k = 0; k < nh; k += step) {
                int hh = loop.half[k];
                const Edge& e = G.edges[static_cast<std::size_t>(hh >> 1)];
                loop.samples.push_back(G.nodes[static_cast<std::size_t>(G.from(hh))]);
                loop.samples.push_back(e.curve.pointAt((e.curve.t0() + e.curve.t1()) * 0.5));
            }
            set.loops.push_back(std::move(loop));
        }
    }
    return set;
}

// ---- Containment ---------------------------------------------------------------------------

enum class Where { Outside, Boundary, Inside };

double distanceToSegment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    double len2 = dot(ab, ab);
    double t = len2 > 0 ? std::clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    return distance(p, a + ab * t);
}

Where classify(Vec2 p, const std::vector<Vec2>& ring, double boundaryTol) {
    bool inside = false;
    std::size_t n = ring.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2& a = ring[j];
        const Vec2& b = ring[i];
        if (distanceToSegment(p, a, b) <= boundaryTol)
            return Where::Boundary;
        if ((a.y > p.y) != (b.y > p.y)) {
            double x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (p.x < x)
                inside = !inside;
        }
    }
    return inside ? Where::Inside : Where::Outside;
}

// Is loop `inner` inside loop `outer`: no sample outside, and at least one strictly inside.
bool nestedIn(const Loop& inner, const Loop& outer, double boundaryTol) {
    if (!outer.box.containsBox(inner.box) || inner.area >= outer.area)
        return false;
    bool strictly = false;
    for (const Vec2& s : inner.samples) {
        Where w = classify(s, outer.ring, boundaryTol);
        if (w == Where::Outside)
            return false;
        if (w == Where::Inside)
            strictly = true;
    }
    return strictly;
}

// ---- Conversion to hatch edges ---------------------------------------------------------------

SplineData reversedSpline(const SplineData& s) {
    SplineData r = s;
    std::reverse(r.control.begin(), r.control.end());
    std::reverse(r.weights.begin(), r.weights.end());
    std::reverse(r.fit.begin(), r.fit.end());
    if (!s.knots.empty()) {
        double sum = s.knots.front() + s.knots.back();
        r.knots.clear();
        for (auto it = s.knots.rbegin(); it != s.knots.rend(); ++it)
            r.knots.push_back(sum - *it);
    }
    return r;
}

HatchEdge toHatchEdge(const Edge& e, bool reversed) {
    const CurveRef& c = e.curve;
    switch (c.kind()) {
        case CurveRef::Kind::Line:
            return reversed ? HatchLineEdge{c.lineB(), c.lineA()}
                            : HatchLineEdge{c.lineA(), c.lineB()};
        case CurveRef::Kind::Arc: {
            double s = normalizeAngle(c.t0());
            double t = normalizeAngle(c.t1());
            HatchArcEdge a;
            a.center = c.center();
            a.radius = c.radius();
            a.startAngle = reversed ? t : s;
            a.endAngle = reversed ? s : t;
            a.ccw = !reversed;
            return a;
        }
        case CurveRef::Kind::Ellipse: {
            HatchEllipseEdge a;
            a.center = c.center();
            a.majorAxis = c.majorAxis();
            a.ratio = c.ratio();
            double s = normalizeAngle(c.t0());
            double t = normalizeAngle(c.t1());
            a.startParam = reversed ? t : s;
            a.endParam = reversed ? s : t;
            a.ccw = !reversed;
            return a;
        }
        case CurveRef::Kind::Spline:
            break;
    }
    if (e.spline)
        return HatchSplineEdge{reversed ? reversedSpline(*e.spline) : *e.spline};
    // A piece of a spline: keep it as a fine polyline.
    std::vector<Vec2> pts;
    geom::flatten(c, 1e-4, pts);
    if (reversed)
        std::reverse(pts.begin(), pts.end());
    HatchPolylineEdge pe;
    pe.closed = false;
    for (const Vec2& p : pts)
        pe.v.push_back(PolyVertex{p, 0, 0, 0});
    return pe;
}

HatchLoop toHatchLoop(const Graph& g, const Loop& loop, bool outer) {
    HatchLoop hl;
    hl.outer = outer;
    for (int h : loop.half)
        hl.edges.push_back(toHatchEdge(g.edges[static_cast<std::size_t>(h >> 1)], (h & 1) != 0));
    return hl;
}

// The loop at index `outerIdx` and everything nested in it, as a Boundary.
Boundary makeBoundary(const std::vector<Candidate>& cands, const LoopSet& set,
                      std::size_t outerIdx, double boundaryTol) {
    const Graph& g = set.graph;
    const Loop& outer = set.loops[outerIdx];
    std::vector<std::size_t> inner;
    for (std::size_t i = 0; i < set.loops.size(); ++i)
        if (i != outerIdx && nestedIn(set.loops[i], outer, boundaryTol))
            inner.push_back(i);
    std::sort(inner.begin(), inner.end(), [&](std::size_t a, std::size_t b) {
        return set.loops[a].area > set.loops[b].area;
    });

    Boundary result;
    result.graphEdges = g.edges.size();
    result.loops.push_back(toHatchLoop(g, outer, true));
    for (int h : outer.half) {
        EntityId id = cands[static_cast<std::size_t>(g.edges[static_cast<std::size_t>(h >> 1)].cand)].id;
        if (std::find(result.sources.begin(), result.sources.end(), id) == result.sources.end())
            result.sources.push_back(id);
    }
    // Nesting depth among the inner loops decides hole (odd) or filled again (even).
    for (std::size_t a = 0; a < inner.size(); ++a) {
        int depth = 1;
        for (std::size_t b = 0; b < inner.size(); ++b)
            if (b != a && nestedIn(set.loops[inner[a]], set.loops[inner[b]], boundaryTol))
                ++depth;
        result.loops.push_back(toHatchLoop(g, set.loops[inner[a]], depth % 2 == 0));
    }
    return result;
}

}  // namespace

Result<Boundary> findBoundary(const std::vector<Candidate>& candidates, Vec2 pick,
                              const BoundaryOptions& options) {
    auto built = buildLoops(candidates, options);
    if (!built.ok())
        return built.error();
    const LoopSet& set = built.value();
    double btol = options.flattenTolerance * 2 + kMerge;
    std::size_t best = set.loops.size();
    for (std::size_t i = 0; i < set.loops.size(); ++i) {
        const Loop& l = set.loops[i];
        if (!l.box.contains(pick))
            continue;
        if (best != set.loops.size() && l.area >= set.loops[best].area)
            continue;
        if (classify(pick, l.ring, btol) == Where::Inside)
            best = i;
    }
    if (best == set.loops.size())
        return makeBoundaryError(BoundaryError::NoClosedBoundary);
    return makeBoundary(candidates, set, best, btol);
}

Result<std::vector<Boundary>> boundaryFromObjects(const std::vector<Candidate>& objects,
                                                  const BoundaryOptions& options) {
    auto built = buildLoops(objects, options);
    if (!built.ok())
        return built.error();
    const LoopSet& set = built.value();
    double btol = options.flattenTolerance * 2 + kMerge;
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < set.loops.size(); ++i) {
        bool covered = false;
        for (std::size_t j = 0; j < set.loops.size() && !covered; ++j)
            covered = j != i && nestedIn(set.loops[i], set.loops[j], btol);
        if (!covered)
            roots.push_back(i);
    }
    if (roots.empty())
        return makeBoundaryError(BoundaryError::NoClosedBoundary);
    std::sort(roots.begin(), roots.end(), [&](std::size_t a, std::size_t b) {
        const BBox2& x = set.loops[a].box;
        const BBox2& y = set.loops[b].box;
        return x.min.x != y.min.x ? x.min.x < y.min.x : x.min.y < y.min.y;
    });
    std::vector<Boundary> out;
    for (std::size_t r : roots)
        out.push_back(makeBoundary(objects, set, r, btol));
    return out;
}

}  // namespace tbcad::hatch

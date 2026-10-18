#include "tbcad/modify/fillet.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "tbcad/modify/break_join.h"

namespace tbcad::modify {

using geom::CurveRef;

namespace {

Error err(std::string m) {
    return Error{ErrorCode::InvalidArgument, std::move(m)};
}

// ---- Primitives -------------------------------------------------------------------------------

struct Prim {
    bool line = true;
    Vec2 a{}, b{};  // line
    Vec2 c{};       // circle / arc
    double r = 0;
    bool full = false;  // a complete circle
    double start = 0, sweep = kTwoPi;
};

std::optional<Prim> primOf(const Entity& e) {
    Prim p;
    if (const auto* l = entityAs<LineData>(e)) {
        if (tol::isZeroLength(l->b - l->a))
            return std::nullopt;
        p.a = l->a;
        p.b = l->b;
        return p;
    }
    if (const auto* c = entityAs<CircleData>(e)) {
        p.line = false;
        p.c = c->center;
        p.r = c->radius;
        p.full = true;
        return p;
    }
    if (const auto* a = entityAs<ArcData>(e)) {
        p.line = false;
        p.c = a->center;
        p.r = a->radius;
        p.start = a->startAngle;
        p.sweep = normalizeAngle(a->endAngle - a->startAngle);
        if (p.sweep < 1e-12)
            p.sweep = kTwoPi;
        return p;
    }
    return std::nullopt;
}

Prim primOfSeg(const Seg& s) {
    Prim p;
    if (auto g = arcGeom(s)) {
        p.line = false;
        p.c = g->center;
        p.r = g->radius;
        p.full = true;  // geometry only; trimming is done on the segment
        return p;
    }
    p.a = s.p0;
    p.b = s.p1;
    return p;
}

std::optional<CurveRef> offsetPrim(const Prim& p, double shift) {
    if (p.line) {
        Vec2 n = perp(normalized(p.b - p.a)) * shift;
        return CurveRef::line(p.a + n, p.b + n);
    }
    double r = std::abs(p.r + shift);
    if (r < 1e-9)
        return std::nullopt;
    return CurveRef::circle(p.c, r);
}

CurveRef curveOfPrim(const Prim& p) {
    return p.line ? CurveRef::line(p.a, p.b) : CurveRef::circle(p.c, p.r);
}

std::optional<Vec2> tangentPoint(const Prim& p, Vec2 center) {
    if (p.line) {
        Vec2 d = p.b - p.a;
        return p.a + d * (dot(center - p.a, d) / lengthSquared(d));
    }
    Vec2 v = center - p.c;
    if (tol::isZeroLength(v))
        return std::nullopt;
    return p.c + normalized(v) * p.r;
}

struct Sol {
    Vec2 center{}, tA{}, tB{};
    std::optional<ArcData> arc{};
};

ArcData minorArc(Vec2 center, double r, Vec2 t1, Vec2 t2) {
    double a1 = std::atan2(t1.y - center.y, t1.x - center.x);
    double a2 = std::atan2(t2.y - center.y, t2.x - center.x);
    ArcData arc;
    arc.center = center;
    arc.radius = r;
    if (normalizeAngle(a2 - a1) <= kPi) {
        arc.startAngle = normalizeAngle(a1);
        arc.endAngle = normalizeAngle(a2);
    } else {
        arc.startAngle = normalizeAngle(a2);
        arc.endAngle = normalizeAngle(a1);
    }
    return arc;
}

using TangentCheck = std::function<bool(Vec2 tA, Vec2 tB)>;

std::optional<Sol> solveFillet(const Prim& A, const Prim& B, double R, Vec2 pa, Vec2 pb,
                               const TangentCheck& valid = {}) {
    std::optional<Sol> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (double sa : {1.0, -1.0}) {
        auto oa = offsetPrim(A, sa * R);
        if (!oa)
            continue;
        for (double sb : {1.0, -1.0}) {
            auto ob = offsetPrim(B, sb * R);
            if (!ob)
                continue;
            std::vector<geom::Intersection> xs;
            geom::intersect(*oa, *ob, xs, true, true);
            for (const auto& x : xs) {
                auto ta = tangentPoint(A, x.p), tb = tangentPoint(B, x.p);
                if (!ta || !tb || samePoint(*ta, *tb, 1e-9))
                    continue;
                if (std::abs(distance(x.p, *ta) - R) > 1e-6 * std::max(1.0, R) ||
                    std::abs(distance(x.p, *tb) - R) > 1e-6 * std::max(1.0, R))
                    continue;
                if (valid && !valid(*ta, *tb))
                    continue;
                double score = distance(*ta, pa) + distance(*tb, pb);
                if (score < bestScore) {
                    bestScore = score;
                    best = Sol{x.p, *ta, *tb, minorArc(x.p, R, *ta, *tb)};
                }
            }
        }
    }
    return best;
}

std::optional<Sol> solveSharp(const Prim& A, const Prim& B, Vec2 pa, Vec2 pb) {
    std::vector<geom::Intersection> xs;
    geom::intersect(curveOfPrim(A), curveOfPrim(B), xs, true, true);
    std::optional<Sol> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (const auto& x : xs) {
        double score = distance(x.p, pa) + distance(x.p, pb);
        if (score < bestScore) {
            bestScore = score;
            best = Sol{x.p, x.p, x.p, std::nullopt};
        }
    }
    return best;
}

// Keeps the part of the entity that holds `pick`, cut (or extended) at the point T.
Result<std::optional<Entity>> cutAt(const Entity& e, const Prim& p, Vec2 T, Vec2 pick) {
    if (p.full)
        return std::optional<Entity>{};
    if (p.line) {
        Vec2 d = p.b - p.a;
        double dd = lengthSquared(d);
        double uT = dot(T - p.a, d) / dd;
        double uP = std::clamp(dot(pick - p.a, d) / dd, 0.0, 1.0);
        LineData out{p.a, p.b};
        if (uP < uT)
            out.b = T;
        else
            out.a = T;
        if (tol::isZeroLength(out.b - out.a))
            return err("The trimmed object would have no length.");
        return std::optional<Entity>(withData(e, out));
    }
    double sweep = p.sweep;
    double angT = std::atan2(T.y - p.c.y, T.x - p.c.x);
    double oT = normalizeAngle(angT - p.start);
    if (oT > sweep && kTwoPi - oT < oT - sweep)
        oT -= kTwoPi;
    double angP = std::atan2(pick.y - p.c.y, pick.x - p.c.x);
    double oP = normalizeAngle(angP - p.start);
    if (oP > sweep)
        oP = (oP - sweep < kTwoPi - oP) ? sweep : 0.0;
    ArcData out;
    out.center = p.c;
    out.radius = p.r;
    out.startAngle = p.start;
    out.endAngle = normalizeAngle(p.start + sweep);
    if (oP < oT) {
        out.endAngle = normalizeAngle(angT);
        if (oT <= 1e-9)
            return err("The trimmed object would have no length.");
    } else {
        out.startAngle = normalizeAngle(angT);
        if (sweep - oT <= 1e-9)
            return err("The trimmed object would have no length.");
    }
    return std::optional<Entity>(withData(e, out));
}

bool parallelLines(const Prim& A, const Prim& B) {
    return A.line && B.line &&
           std::abs(cross(normalized(A.b - A.a), normalized(B.b - B.a))) <= 1e-9;
}

// ---- Fillet between any two curves ----------------------------------------------------------
//
// Ellipses, splines and the segments of polylines have no closed form. The centre of a fillet
// circle lies on the offset curve of each object (by +-R); crossings of the sampled offsets give
// starting values that Newton's method sharpens on the two curve parameters.

struct Hit {
    double tA = 0, tB = 0;
    Vec2 center{}, pa{}, pb{};
    double score = 0;
};

Vec2 offsetPoint(const CurveRef& c, double t, double side, double r) {
    return c.pointAt(t) + perp(c.tangentAt(t)) * (side * r);
}

bool segmentCross(Vec2 p, Vec2 p2, Vec2 q, Vec2 q2, double& fa, double& fb) {
    const Vec2 r = p2 - p, s = q2 - q;
    const double den = cross(r, s);
    if (std::abs(den) < 1e-300)
        return false;
    fa = cross(q - p, s) / den;
    fb = cross(q - p, r) / den;
    return fa >= -1e-9 && fa <= 1 + 1e-9 && fb >= -1e-9 && fb <= 1 + 1e-9;
}

std::vector<CurveRef> nearestPieces(const Entity& e, Vec2 pick, std::size_t keep) {
    std::vector<CurveRef> all;
    geom::curvesOf(e, all);
    std::sort(all.begin(), all.end(), [&](const CurveRef& x, const CurveRef& y) {
        return x.distanceTo(pick) < y.distanceTo(pick);
    });
    if (all.size() > keep)
        all.resize(keep);
    return all;
}

void filletHits(const CurveRef& A, const CurveRef& B, double R, Vec2 pickA, Vec2 pickB,
                std::vector<Hit>& hits) {
    constexpr int kN = 160;
    const double a0 = A.t0(), aw = A.t1() - A.t0(), b0 = B.t0(), bw = B.t1() - B.t0();
    for (double sA : {1.0, -1.0}) {
        for (double sB : {1.0, -1.0}) {
            std::vector<Vec2> oa, ob;
            for (int i = 0; i <= kN; ++i) {
                oa.push_back(offsetPoint(A, a0 + aw * i / kN, sA, R));
                ob.push_back(offsetPoint(B, b0 + bw * i / kN, sB, R));
            }
            for (int i = 0; i < kN; ++i) {
                BBox2 bi;
                bi.merge(BBox2{oa[i], oa[i]});
                bi.merge(BBox2{oa[i + 1], oa[i + 1]});
                for (int j = 0; j < kN; ++j) {
                    BBox2 bj;
                    bj.merge(BBox2{ob[j], ob[j]});
                    bj.merge(BBox2{ob[j + 1], ob[j + 1]});
                    if (!bi.inflated(1e-9).intersects(bj))
                        continue;
                    double fa = 0, fb = 0;
                    if (!segmentCross(oa[i], oa[i + 1], ob[j], ob[j + 1], fa, fb))
                        continue;
                    double ta = a0 + aw * (i + fa) / kN, tb = b0 + bw * (j + fb) / kN;
                    const double ha = std::max(std::abs(aw), 1e-9) * 1e-6;
                    const double hb = std::max(std::abs(bw), 1e-9) * 1e-6;
                    bool ok = false;
                    for (int it = 0; it < 12; ++it) {
                        const Vec2 F = offsetPoint(A, ta, sA, R) - offsetPoint(B, tb, sB, R);
                        const Vec2 dA =
                            (offsetPoint(A, ta + ha, sA, R) - offsetPoint(A, ta - ha, sA, R)) /
                            (2 * ha);
                        const Vec2 dB =
                            (offsetPoint(B, tb + hb, sB, R) - offsetPoint(B, tb - hb, sB, R)) /
                            (2 * hb);
                        // F + dA*dta - dB*dtb = 0
                        const double det = cross(dA, dB * -1.0);
                        if (std::abs(det) < 1e-300)
                            break;
                        const double dta = cross(F * -1.0, dB * -1.0) / det;
                        const double dtb = cross(dA, F * -1.0) / det;
                        ta += dta;
                        tb += dtb;
                        if (std::abs(dta) < 1e-13 * (1 + std::abs(ta)) &&
                            std::abs(dtb) < 1e-13 * (1 + std::abs(tb))) {
                            ok = true;
                            break;
                        }
                    }
                    ta = std::clamp(ta, std::min(a0, a0 + aw), std::max(a0, a0 + aw));
                    tb = std::clamp(tb, std::min(b0, b0 + bw), std::max(b0, b0 + bw));
                    const Vec2 ca = offsetPoint(A, ta, sA, R), cb = offsetPoint(B, tb, sB, R);
                    const double scale = 1.0 + R + std::abs(ca.x) + std::abs(ca.y);
                    if (!ok && distance(ca, cb) > 1e-9 * scale)
                        continue;
                    if (distance(ca, cb) > 1e-7 * scale)
                        continue;
                    Hit h;
                    h.tA = ta;
                    h.tB = tb;
                    h.center = (ca + cb) * 0.5;
                    h.pa = A.pointAt(ta);
                    h.pb = B.pointAt(tb);
                    h.score = distance(h.pa, pickA) + distance(h.pb, pickB);
                    hits.push_back(h);
                }
            }
        }
    }
}

// The part of `e` that holds `pick`, cut at T. Closed objects and cuts at an end leave it as it is.
std::optional<Entity> keepPartAt(const Entity& e, Vec2 T, Vec2 pick) {
    auto r = breakAtPoint(e, T);
    if (!r || !r.value().modified || r.value().added.empty())
        return std::nullopt;
    const Entity& first = *r.value().modified;
    const Entity& second = r.value().added.front();
    return geom::distance(first, pick) <= geom::distance(second, pick) ? first : second;
}

Result<PairEdit> filletCurves(const Entity& a, Vec2 pickA, const Entity& b, Vec2 pickB,
                              const FilletOptions& opt) {
    const double R = std::max(opt.radius, 0.0);
    std::vector<Hit> hits;
    std::vector<Hit> sharp;
    for (const CurveRef& A : nearestPieces(a, pickA, 3)) {
        for (const CurveRef& B : nearestPieces(b, pickB, 3)) {
            if (R > tol::kLinear) {
                filletHits(A, B, R, pickA, pickB, hits);
            } else {
                std::vector<geom::Intersection> xs;
                geom::intersect(A, B, xs);
                for (const auto& x : xs) {
                    Hit h;
                    h.pa = h.pb = h.center = x.p;
                    h.score = distance(x.p, pickA) + distance(x.p, pickB);
                    hits.push_back(h);
                }
            }
        }
    }
    if (hits.empty())
        return err(R > tol::kLinear ? "There is no fillet of this radius between the two objects."
                                    : "The two objects do not meet.");
    const Hit& h = *std::min_element(hits.begin(), hits.end(),
                                     [](const Hit& x, const Hit& y) { return x.score < y.score; });
    PairEdit out;
    if (opt.trim) {
        out.modifiedA = keepPartAt(a, h.pa, pickA);
        out.modifiedB = keepPartAt(b, h.pb, pickB);
    }
    if (R > tol::kLinear) {
        const double aa = std::atan2(h.pa.y - h.center.y, h.pa.x - h.center.x);
        const double ab = std::atan2(h.pb.y - h.center.y, h.pb.x - h.center.x);
        ArcData arc;
        arc.center = h.center;
        arc.radius = R;
        if (normalizeAngle(ab - aa) <= kPi) {
            arc.startAngle = normalizeAngle(aa);
            arc.endAngle = normalizeAngle(ab);
        } else {
            arc.startAngle = normalizeAngle(ab);
            arc.endAngle = normalizeAngle(aa);
        }
        out.added.push_back(withData(a, arc));
    }
    return out;
}

// ---- Chamfer between any two curves ----------------------------------------------------------

// The point `d` along the curve from the parameter `t` towards the side holding `pick`.
std::optional<Vec2> walkFrom(const CurveRef& c, double t, Vec2 pick, double d) {
    const bool forward = c.paramAt(pick) >= t;
    auto part = forward ? c.subcurve(t, c.t1()) : c.subcurve(c.t0(), t);
    if (!part)
        return std::nullopt;
    const double len = part.value().length();
    if (d > len - tol::kLinear)
        return std::nullopt;
    const CurveRef& q = part.value();
    return q.pointAt(q.paramAtLength(forward ? d : len - d));
}

Result<PairEdit> chamferCurves(const Entity& a, Vec2 pickA, const Entity& b, Vec2 pickB,
                               const ChamferOptions& opt) {
    if (opt.distance1 < 0 || opt.distance2 < 0)
        return err("Chamfer distances cannot be negative.");
    struct Cand {
        CurveRef A, B;
        Vec2 x;
        double tA, tB, score;
    };
    std::optional<Cand> best;
    for (const CurveRef& A : nearestPieces(a, pickA, 3)) {
        for (const CurveRef& B : nearestPieces(b, pickB, 3)) {
            std::vector<geom::Intersection> xs;
            geom::intersect(A, B, xs);
            for (const auto& x : xs) {
                const double score = distance(x.p, pickA) + distance(x.p, pickB);
                if (!best || score < best->score)
                    best = Cand{A, B, x.p, A.paramAt(x.p), B.paramAt(x.p), score};
            }
        }
    }
    if (!best)
        return err("The two objects do not meet.");
    double d1 = opt.distance1, d2 = opt.distance2;
    if (opt.byAngle) {
        // The angle is measured against the first object's tangent at the corner.
        const Vec2 ta = best->A.tangentAt(best->tA), tb = best->B.tangentAt(best->tB);
        Vec2 va = ta * (best->A.paramAt(pickA) >= best->tA ? 1.0 : -1.0);
        Vec2 vb = tb * (best->B.paramAt(pickB) >= best->tB ? 1.0 : -1.0);
        const double gamma = std::acos(std::clamp(dot(va, vb), -1.0, 1.0));
        if (opt.angle <= 0 || opt.angle + gamma >= kPi - 1e-9)
            return err("That angle does not fit between the two objects.");
        d2 = d1 * std::sin(opt.angle) / std::sin(gamma + opt.angle);
    }
    auto p1 = walkFrom(best->A, best->tA, pickA, d1);
    auto p2 = walkFrom(best->B, best->tB, pickB, d2);
    if (!p1 || !p2)
        return err("The chamfer distance is longer than the object.");
    PairEdit out;
    if (opt.trim) {
        if (d1 > tol::kLinear)
            out.modifiedA = keepPartAt(a, *p1, pickA);
        if (d2 > tol::kLinear)
            out.modifiedB = keepPartAt(b, *p2, pickB);
    }
    if (!tol::isZeroLength(*p2 - *p1))
        out.added.push_back(withData(a, LineData{*p1, *p2}));
    return out;
}

}  // namespace

Result<PairEdit> filletEntities(const Entity& a, Vec2 pickA, const Entity& b, Vec2 pickB,
                                const FilletOptions& opt) {
    auto pa = primOf(a), pb = primOf(b);
    if (!pa || !pb) {
        std::vector<CurveRef> ca, cb;
        if (!geom::curvesOf(a, ca) || ca.empty() || !geom::curvesOf(b, cb) || cb.empty())
            return err("Fillet works on lines, arcs, circles, ellipses, splines and polylines.");
        return filletCurves(a, pickA, b, pickB, opt);
    }
    const double R = std::max(opt.radius, 0.0);
    std::optional<Sol> sol;
    if (parallelLines(*pa, *pb)) {
        Vec2 dir = normalized(pa->b - pa->a);
        double gap = cross(dir, pb->a - pa->a);
        if (std::abs(gap) <= 1e-6)
            return err("The lines are collinear; there is no corner to fillet.");
        // Semicircle at the end of A nearest the pick; B is cut where it meets the diameter.
        Vec2 tA = distance(pa->a, pickA) <= distance(pa->b, pickA) ? pa->a : pa->b;
        Vec2 outward = normalized(tA - (pa->a + pa->b) * 0.5);
        Vec2 tB = tA + perp(dir) * gap;
        Vec2 center = (tA + tB) * 0.5;
        double mid = std::atan2(outward.y, outward.x);
        ArcData arc;
        arc.center = center;
        arc.radius = std::abs(gap) * 0.5;
        arc.startAngle = normalizeAngle(mid - kPi / 2);
        arc.endAngle = normalizeAngle(mid + kPi / 2);
        sol = Sol{center, tA, tB, arc};
        if (R <= 0)
            sol->arc.reset();
    } else if (R <= tol::kLinear) {
        sol = solveSharp(*pa, *pb, pickA, pickB);
        if (!sol && pa->full && pb->full)
            return err("Two circles cannot be joined with a sharp corner.");
    } else {
        sol = solveFillet(*pa, *pb, R, pickA, pickB);
    }
    if (!sol)
        return err("There is no fillet of this radius between the two objects.");

    PairEdit out;
    if (opt.trim) {
        auto ta = cutAt(a, *pa, sol->tA, pickA);
        if (!ta)
            return Error{ta.error()};
        auto tb = cutAt(b, *pb, sol->tB, pickB);
        if (!tb)
            return Error{tb.error()};
        out.modifiedA = std::move(ta.value());
        out.modifiedB = std::move(tb.value());
    }
    if (sol->arc)
        out.added.push_back(withData(a, *sol->arc));
    return out;
}

// ---- Polylines
// ------------------------------------------------------------------------------------

std::size_t nearestSegmentIndex(const PolylineData& polyline, Vec2 p) {
    auto segs = polylineSegs(cleanPolyline(polyline));
    if (segs.empty())
        return 0;
    return std::min(static_cast<std::size_t>(closestGlobalParam(segs, p)), segs.size() - 1);
}

std::optional<std::size_t> sharedVertex(const PolylineData& polyline, std::size_t sa,
                                        std::size_t sb) {
    PolylineData pl = cleanPolyline(polyline);
    std::size_t n = polylineSegs(pl).size();
    if (sa == sb || sa >= n || sb >= n)
        return std::nullopt;
    if (sb == sa + 1)
        return sb % n;
    if (sa == sb + 1)
        return sa % n;
    if (pl.closed && ((sa == 0 && sb == n - 1) || (sb == 0 && sa == n - 1)))
        return std::size_t{0};
    return std::nullopt;
}

namespace {

struct Corner {
    double uA = 1, uB = 0;  // where segment A ends / segment B starts (local parameters)
    Seg connector{};
};
using CornerSolver = std::function<std::optional<Corner>(const Seg& a, const Seg& b)>;

// Applies `solve` to the given vertices of a polyline and rebuilds it.
Result<Entity> rebuildCorners(const Entity& src, std::vector<std::size_t> vertices,
                              const CornerSolver& solve, bool single) {
    const auto* raw = entityAs<PolylineData>(src);
    if (!raw)
        return err("This is not a polyline.");
    PolylineData pl = cleanPolyline(*raw);
    std::vector<Seg> segs = polylineSegs(pl);
    const std::size_t n = segs.size();
    if (n < 2)
        return err("The polyline has no corner.");
    struct Range {
        double u0 = 0, u1 = 1;
    };
    std::vector<Range> ranges(n);
    std::vector<std::optional<Seg>> connectors(n + 1);
    std::size_t done = 0;
    for (std::size_t k : vertices) {
        if (!(pl.closed ? k < n : (k >= 1 && k < n)))
            return err("The polyline has no such corner.");
        std::size_t ia = (k + n - 1) % n, ib = k % n;
        auto c = solve(segs[ia], segs[ib]);
        bool fits = c && c->uA > ranges[ia].u0 + 1e-9 && c->uA <= 1.0 + 1e-9 &&
                    c->uB < ranges[ib].u1 - 1e-9 && c->uB >= -1e-9;
        if (!fits) {
            if (single)
                return err(c ? "The distance is too large for this corner."
                             : "These segments have no corner that can be rounded or cut.");
            continue;
        }
        ranges[ia].u1 = std::min(c->uA, 1.0);
        ranges[ib].u0 = std::max(c->uB, 0.0);
        connectors[k] = c->connector;
        ++done;
    }
    if (done == 0)
        return err("Nothing could be changed: the segments are too short or have no corners.");
    std::vector<Seg> chain;
    for (std::size_t i = 0; i < n; ++i) {
        chain.push_back(segSub(segs[i], ranges[i].u0, ranges[i].u1));
        std::size_t endVertex = pl.closed ? (i + 1) % n : i + 1;
        if (connectors[endVertex])
            chain.push_back(*connectors[endVertex]);
    }
    return withData(src, polylineFromSegs(chain, pl.closed, &pl));
}

std::vector<std::size_t> allCorners(const PolylineData& raw) {
    PolylineData pl = cleanPolyline(raw);
    std::size_t n = polylineSegs(pl).size();
    std::vector<std::size_t> v;
    for (std::size_t k = pl.closed ? 0 : 1; k < n; ++k)
        v.push_back(k);
    return v;
}

CornerSolver filletSolver(double R) {
    return [R](const Seg& a, const Seg& b) -> std::optional<Corner> {
        Vec2 t0 = segTangent(a, 1.0), t1 = segTangent(b, 0.0);
        double c = cross(t0, t1), dt = dot(t0, t1);
        if (std::abs(c) <= 1e-9)
            return std::nullopt;  // collinear / reversing: no corner
        Prim A = primOfSeg(a), B = primOfSeg(b);
        auto onSeg = [](const Seg& s, Vec2 p) {
            double u = segParamUnbounded(s, p);
            return u >= -1e-9 && u <= 1.0 + 1e-9;
        };
        auto sol = solveFillet(A, B, R, segPoint(a, 0.5), segPoint(b, 0.5),
                               [&](Vec2 ta, Vec2 tb) { return onSeg(a, ta) && onSeg(b, tb); });
        if (!sol)
            return std::nullopt;
        Corner out;
        out.uA = segParamUnbounded(a, sol->tA);
        out.uB = segParamUnbounded(b, sol->tB);
        double turn = std::atan2(std::abs(c), dt);
        out.connector = Seg{sol->tA, sol->tB, std::tan((c > 0 ? turn : -turn) / 4.0)};
        return out;
    };
}

CornerSolver chamferSolver(const ChamferOptions& o) {
    return [o](const Seg& a, const Seg& b) -> std::optional<Corner> {
        if (a.isArc() || b.isArc())
            return std::nullopt;
        Vec2 x = a.p1;
        Vec2 va = normalized(a.p0 - x), vb = normalized(b.p1 - x);
        double la = segLength(a), lb = segLength(b);
        double gamma = std::acos(std::clamp(dot(va, vb), -1.0, 1.0));
        if (gamma <= 1e-9 || gamma >= kPi - 1e-9)
            return std::nullopt;
        double d1 = o.distance1, d2 = o.distance2;
        if (o.byAngle) {
            if (o.angle <= 0 || o.angle + gamma >= kPi - 1e-9)
                return std::nullopt;
            d2 = d1 * std::sin(o.angle) / std::sin(gamma + o.angle);
        }
        if (d1 <= 0 || d2 <= 0 || d1 >= la - 1e-9 || d2 >= lb - 1e-9)
            return std::nullopt;
        Corner out;
        out.uA = 1.0 - d1 / la;
        out.uB = d2 / lb;
        out.connector = Seg{x + va * d1, x + vb * d2, 0};
        return out;
    };
}

}  // namespace

Result<Entity> filletPolylineVertex(const Entity& polyline, std::size_t vertex, double radius) {
    if (radius <= tol::kLinear)
        return polyline;  // a sharp corner stays as it is
    return rebuildCorners(polyline, {vertex}, filletSolver(radius), true);
}

Result<Entity> filletPolyline(const Entity& polyline, double radius) {
    const auto* pl = entityAs<PolylineData>(polyline);
    if (!pl)
        return err("This is not a polyline.");
    if (radius <= tol::kLinear)
        return polyline;
    return rebuildCorners(polyline, allCorners(*pl), filletSolver(radius), false);
}

Result<Entity> chamferPolylineVertex(const Entity& polyline, std::size_t vertex,
                                     const ChamferOptions& options) {
    return rebuildCorners(polyline, {vertex}, chamferSolver(options), true);
}

Result<Entity> chamferPolyline(const Entity& polyline, const ChamferOptions& options) {
    const auto* pl = entityAs<PolylineData>(polyline);
    if (!pl)
        return err("This is not a polyline.");
    return rebuildCorners(polyline, allCorners(*pl), chamferSolver(options), false);
}

// ---- Chamfer of two lines
// ---------------------------------------------------------------------------

Result<PairEdit> chamferEntities(const Entity& a, Vec2 pickA, const Entity& b, Vec2 pickB,
                                 const ChamferOptions& opt) {
    const auto* la = entityAs<LineData>(a);
    const auto* lb = entityAs<LineData>(b);
    if (!la || !lb) {
        std::vector<CurveRef> ca, cb;
        if (!geom::curvesOf(a, ca) || ca.empty() || !geom::curvesOf(b, cb) || cb.empty())
            return err("Chamfer works on lines, arcs, ellipses, splines and polylines.");
        return chamferCurves(a, pickA, b, pickB, opt);
    }
    auto pA = primOf(a), pB = primOf(b);
    if (!pA || !pB)
        return err("A line has no length.");
    Prim A = *pA, B = *pB;
    if (parallelLines(A, B))
        return err("The lines are parallel; there is no corner to chamfer.");
    std::vector<geom::Intersection> xs;
    geom::intersect(curveOfPrim(A), curveOfPrim(B), xs, true, true);
    if (xs.empty())
        return err("The lines do not meet.");
    Vec2 x = xs[0].p;
    auto dirToward = [&](const Prim& p, Vec2 pick) {
        Vec2 d = normalized(p.b - p.a);
        Vec2 v = d * dot(pick - x, d);
        if (tol::isZeroLength(v))
            v = distance(p.a, x) > distance(p.b, x) ? p.a - x : p.b - x;
        return normalized(v);
    };
    Vec2 va = dirToward(A, pickA), vb = dirToward(B, pickB);
    double gamma = std::acos(std::clamp(dot(va, vb), -1.0, 1.0));
    double d1 = opt.distance1, d2 = opt.distance2;
    if (opt.byAngle) {
        if (opt.angle <= 0 || opt.angle + gamma >= kPi - 1e-9)
            return err("That angle does not fit between the two lines.");
        d2 = d1 * std::sin(opt.angle) / std::sin(gamma + opt.angle);
    }
    if (d1 < 0 || d2 < 0)
        return err("Chamfer distances cannot be negative.");
    Vec2 p1 = x + va * d1, p2 = x + vb * d2;
    PairEdit out;
    if (opt.trim) {
        // The line keeps the part holding the pick; a distance beyond the end extends it.
        auto ta = cutAt(a, A, p1, pickA);
        if (!ta)
            return Error{ta.error()};
        auto tb = cutAt(b, B, p2, pickB);
        if (!tb)
            return Error{tb.error()};
        out.modifiedA = std::move(ta.value());
        out.modifiedB = std::move(tb.value());
    }
    if (!tol::isZeroLength(p2 - p1))
        out.added.push_back(withData(a, LineData{p1, p2}));
    return out;
}

}  // namespace tbcad::modify

#include <algorithm>
#include <cmath>

#include "tbcad/geom/curve.h"

namespace tbcad::geom {

namespace {

using Out = std::vector<Intersection>;

constexpr double kEps = 2.220446049250313e-16;

bool sameSpot(const Vec2& a, const Vec2& b) {
    double scale = std::max({std::abs(a.x), std::abs(a.y), std::abs(b.x), std::abs(b.y), 1.0});
    return distance(a, b) <= std::max(tol::kLinear, 1e-13 * scale);
}

void addUnique(Out& out, std::size_t firstNew, const Intersection& i) {
    for (std::size_t k = firstNew; k < out.size(); ++k)
        if (sameSpot(out[k].p, i.p))
            return;
    out.push_back(i);
}

// ---- line / line ------------------------------------------------------------------------------

void lineLine(const CurveRef& A, const CurveRef& B, bool extA, bool extB, Out& out) {
    Vec2 p = A.lineA(), r = A.lineB() - p;
    Vec2 q = B.lineA(), s = B.lineB() - q;
    double lr = length(r), ls = length(s);
    if (!(lr > 0) || !(ls > 0))
        return;
    double denom = cross(r, s);
    Vec2 qp = q - p;
    if (std::abs(denom) <= tol::kAngular * lr * ls) {
        if (std::abs(cross(qp, r)) / lr > tol::kLinear)
            return;
        if (extA && extB)
            return;
        double rr = dot(r, r), ss = dot(s, s);
        double b0 = dot(q - p, r) / rr, b1 = dot(q + s - p, r) / rr;  // B's end points on A
        double lo = std::min(b0, b1), hi = std::max(b0, b1);
        if (!extB) {
            // B limited to its own range.
        } else {
            lo = -std::numeric_limits<double>::infinity();
            hi = std::numeric_limits<double>::infinity();
        }
        if (!extA) {
            lo = std::max(lo, 0.0);
            hi = std::min(hi, 1.0);
        }
        double tolA = tol::kLinear / lr;
        if (hi < lo - tolA)
            return;
        auto emit = [&](double ta) {
            Vec2 pt = p + r * ta;
            out.push_back({pt, ta, dot(pt - q, s) / ss});
        };
        if (hi - lo <= tolA) {
            emit(0.5 * (lo + hi));
        } else {
            emit(lo);
            emit(hi);
        }
        return;
    }
    double t = cross(qp, s) / denom;
    double u = cross(qp, r) / denom;
    double eA = tol::kLinear / lr, eB = tol::kLinear / ls;
    if (!extA && (t < -eA || t > 1 + eA))
        return;
    if (!extB && (u < -eB || u > 1 + eB))
        return;
    if (!extA)
        t = std::clamp(t, 0.0, 1.0);
    if (!extB)
        u = std::clamp(u, 0.0, 1.0);
    out.push_back({p + r * t, t, u});
}

// ---- line / circle ------------------------------------------------------------------------------

void lineArc(const CurveRef& L, const CurveRef& A, bool extL, bool extA, Out& out) {
    Vec2 p = L.lineA(), r = L.lineB() - p;
    double lr2 = dot(r, r);
    if (!(lr2 > 0))
        return;
    double lr = std::sqrt(lr2);
    Vec2 c = A.center();
    double R = A.radius();
    double t0 = dot(c - p, r) / lr2;
    double dist = std::abs(cross(r, c - p)) / lr;
    if (dist > R + tol::kLinear)
        return;
    double ts[2];
    int n = 0;
    if (std::abs(dist - R) <= tol::kLinear) {
        ts[n++] = t0;
    } else {
        double h = std::sqrt(std::max(0.0, (R - dist) * (R + dist)));
        ts[n++] = t0 - h / lr;
        ts[n++] = t0 + h / lr;
    }
    for (int i = 0; i < n; ++i) {
        double t = ts[i];
        if (!extL && !L.containsParam(t))
            continue;
        if (!extL)
            t = std::clamp(t, 0.0, 1.0);
        Vec2 pt = p + r * t;
        Vec2 v = pt - c;
        double ang = std::atan2(v.y, v.x);
        if (!extA && !A.containsParam(ang))
            continue;
        out.push_back({pt, t, A.wrapParam(ang)});
    }
}

// ---- circle / circle
// ------------------------------------------------------------------------------

void arcArc(const CurveRef& A, const CurveRef& B, bool extA, bool extB, Out& out) {
    Vec2 c1 = A.center(), c2 = B.center();
    double r1 = A.radius(), r2 = B.radius();
    Vec2 d = c2 - c1;
    double dd = length(d);
    bool fullA = extA || A.sweep() >= kTwoPi - 1e-12;
    bool fullB = extB || B.sweep() >= kTwoPi - 1e-12;
    auto inA = [&](double ang) { return fullA || A.containsParam(ang); };
    auto inB = [&](double ang) { return fullB || B.containsParam(ang); };
    auto emit = [&](Vec2 pt) {
        Vec2 v1 = pt - c1, v2 = pt - c2;
        double a1 = std::atan2(v1.y, v1.x), a2 = std::atan2(v2.y, v2.x);
        if (!inA(a1) || !inB(a2))
            return;
        out.push_back({pt, A.wrapParam(a1), B.wrapParam(a2)});
    };
    if (dd <= tol::kLinear) {
        if (std::abs(r1 - r2) > tol::kLinear)
            return;
        if (fullA && fullB)
            return;  // identical circles: infinitely many points, report none
        // Coincident arcs: report the end points of the overlap.
        std::size_t first = out.size();
        auto tryEnd = [&](const CurveRef& src, bool srcFull, bool fromA) {
            if (srcFull)
                return;
            for (Vec2 pt : {src.startPoint(), src.endPoint()}) {
                Vec2 v1 = pt - c1, v2 = pt - c2;
                double a1 = std::atan2(v1.y, v1.x), a2 = std::atan2(v2.y, v2.x);
                if (!inA(a1) || !inB(a2))
                    continue;
                (void)fromA;
                addUnique(out, first, {pt, A.wrapParam(a1), B.wrapParam(a2)});
            }
        };
        tryEnd(A, fullA, true);
        tryEnd(B, fullB, false);
        return;
    }
    if (dd > r1 + r2 + tol::kLinear || dd < std::abs(r1 - r2) - tol::kLinear)
        return;
    Vec2 u = d / dd;
    if (std::abs(dd - (r1 + r2)) <= tol::kLinear) {
        emit(c1 + u * r1);
        return;
    }
    if (std::abs(dd - std::abs(r1 - r2)) <= tol::kLinear) {
        emit(c1 + u * (r1 >= r2 ? r1 : -r1));
        return;
    }
    double a = (dd * dd + r1 * r1 - r2 * r2) / (2 * dd);
    double h = std::sqrt(std::max(0.0, (r1 - a) * (r1 + a)));
    Vec2 mid = c1 + u * a;
    emit(mid + perp(u) * h);
    emit(mid - perp(u) * h);
}

// ---- line / ellipse
// ------------------------------------------------------------------------------

void lineEllipse(const CurveRef& L, const CurveRef& E, bool extL, bool extE, Out& out) {
    Vec2 c = E.center(), m = E.majorAxis();
    double ratio = E.ratio();
    double mm = dot(m, m);
    if (!(mm > 0) || !(ratio > 0))
        return;
    Vec2 mn = perp(m);
    auto toUnit = [&](Vec2 v) { return Vec2{dot(v, m) / mm, dot(v, mn) / (mm * ratio)}; };
    Vec2 p = L.lineA(), r = L.lineB() - p;
    Vec2 pu = toUnit(p - c);
    Vec2 ru = {dot(r, m) / mm, dot(r, mn) / (mm * ratio)};
    double a = dot(ru, ru);
    if (!(a > 0))
        return;
    double b = dot(pu, ru);
    double cc = dot(pu, pu) - 1.0;
    double disc = b * b - a * cc;
    double lm = std::sqrt(mm);
    // Distance of the line from the centre in unit space vs 1, tolerance scaled to mm.
    double distU = std::sqrt(std::max(0.0, dot(pu, pu) - b * b / a));
    double tolU = tol::kLinear / lm;
    if (distU > 1 + tolU)
        return;
    double ts[2];
    int n = 0;
    if (std::abs(distU - 1) <= tolU) {
        ts[n++] = -b / a;
    } else {
        double sq = std::sqrt(std::max(0.0, disc));
        ts[n++] = (-b - sq) / a;
        ts[n++] = (-b + sq) / a;
    }
    for (int i = 0; i < n; ++i) {
        double t = ts[i];
        if (!extL && !L.containsParam(t))
            continue;
        if (!extL)
            t = std::clamp(t, 0.0, 1.0);
        Vec2 pt = p + r * t;
        Vec2 q = toUnit(pt - c);
        double ang = std::atan2(q.y, q.x);
        if (!extE && !E.containsParam(ang))
            continue;
        out.push_back({pt, t, E.wrapParam(ang)});
    }
}

// ---- numeric ------------------------------------------------------------------------------------

struct Piece {
    double t0, t1;
    Vec2 p0, p1;
    BBox2 box;
};

std::vector<Piece> pieces(const CurveRef& c, double tol) {
    std::vector<Vec2> pts;
    std::vector<double> ts;
    flatten(c, tol, pts, &ts);
    std::vector<Piece> out;
    out.reserve(pts.size());
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        Piece pc;
        pc.t0 = ts[i];
        pc.t1 = ts[i + 1];
        pc.p0 = pts[i];
        pc.p1 = pts[i + 1];
        Vec2 chord = pc.p1 - pc.p0;
        double dev = 0;
        if (c.kind() != CurveRef::Kind::Line) {
            // Sample the middle for the real deviation from the chord (flatten() bounds the rest).
            double cl = length(chord);
            for (double f : {0.5}) {
                Vec2 m = c.pointAt(pc.t0 + (pc.t1 - pc.t0) * f);
                double d = cl > 0 ? std::abs(cross(chord, m - pc.p0)) / cl : distance(m, pc.p0);
                if (cl > 0) {
                    double along = dot(chord, m - pc.p0) / cl;
                    if (along < 0)
                        d = std::max(d, -along);
                    else if (along > cl)
                        d = std::max(d, along - cl);
                }
                dev = std::max(dev, d);
            }
            dev = dev * 1.6 + tol * 0.5;
        }
        pc.box.expand(pc.p0);
        pc.box.expand(pc.p1);
        pc.box = pc.box.inflated(dev + 1e-9);
        out.push_back(pc);
    }
    return out;
}

CurveRef extendedLine(const CurveRef& L, const CurveRef& other, double& k) {
    BBox2 ob = other.bounds();
    double len = std::max(distance(L.lineA(), L.lineB()), 1e-300);
    double reach = distance(L.lineA(), ob.center()) + 0.5 * length(ob.max - ob.min) + len;
    k = 2.0 * reach / len + 1.0;
    Vec2 d = L.lineB() - L.lineA();
    return CurveRef::line(L.lineA() - d * k, L.lineB() + d * k);
}

CurveRef fullCurve(const CurveRef& c) {
    if (c.kind() == CurveRef::Kind::Arc)
        return CurveRef::circle(c.center(), c.radius());
    if (c.kind() == CurveRef::Kind::Ellipse)
        return CurveRef::ellipse(c.center(), c.majorAxis(), c.ratio(), 0, kTwoPi);
    return c;
}

bool refine(const CurveRef& A, const CurveRef& B, double& ta, double& tb, double acc) {
    auto clampA = [&](double t) { return std::clamp(t, A.t0(), A.t1()); };
    auto clampB = [&](double t) { return std::clamp(t, B.t0(), B.t1()); };
    Vec2 F = A.pointAt(ta) - B.pointAt(tb);
    double fn = length(F);
    for (int it = 0; it < 80 && fn > acc; ++it) {
        Vec2 a = A.derivativeAt(ta), b = B.derivativeAt(tb) * -1.0;
        double det = cross(a, b);
        double na = length(a), nb = length(b);
        double dA, dB;
        if (std::abs(det) > 1e-6 * na * nb) {
            dA = (-F.x * b.y + F.y * b.x) / det;
            dB = (-a.x * F.y + a.y * F.x) / det;
        } else {
            // Nearly tangent: damped least squares.
            double jj00 = dot(a, a), jj01 = dot(a, b), jj11 = dot(b, b);
            double lam = 1e-3 * (jj00 + jj11) + 1e-300;
            double g0 = -dot(a, F), g1 = -dot(b, F);
            double m00 = jj00 + lam, m01 = jj01, m11 = jj11 + lam;
            double dd = m00 * m11 - m01 * m01;
            if (dd == 0)
                return false;
            dA = (g0 * m11 - g1 * m01) / dd;
            dB = (m00 * g1 - m01 * g0) / dd;
        }
        double step = 1.0;
        bool improved = false;
        for (int bt = 0; bt < 8; ++bt) {
            double na2 = clampA(ta + step * dA), nb2 = clampB(tb + step * dB);
            Vec2 F2 = A.pointAt(na2) - B.pointAt(nb2);
            double fn2 = length(F2);
            if (fn2 < fn) {
                ta = na2;
                tb = nb2;
                F = F2;
                fn = fn2;
                improved = true;
                break;
            }
            step *= 0.5;
        }
        if (!improved)
            break;
    }
    return fn <= acc;
}

void numeric(const CurveRef& A0, const CurveRef& B0, bool extA, bool extB, Out& out) {
    CurveRef A = A0, B = B0;
    double kA = 0, kB = 0;
    bool lineExtA = false, lineExtB = false;
    if (extA) {
        if (A.kind() == CurveRef::Kind::Line) {
            A = extendedLine(A0, B0, kA);
            lineExtA = true;
        } else {
            A = fullCurve(A0);
        }
    }
    if (extB) {
        if (B.kind() == CurveRef::Kind::Line) {
            B = extendedLine(B0, A0, kB);
            lineExtB = true;
        } else {
            B = fullCurve(B0);
        }
    }
    BBox2 bb = A.bounds();
    bb.merge(B.bounds());
    if (bb.empty())
        return;
    double diag = length(bb.max - bb.min);
    double maxAbs = std::max(
        {std::abs(bb.min.x), std::abs(bb.min.y), std::abs(bb.max.x), std::abs(bb.max.y), 1.0});
    double ftol = std::max(1e-3, 1e-3 * diag);  // refine() polishes each crossing
    std::vector<Piece> pa = pieces(A, ftol), pb = pieces(B, ftol);
    // Pieces outside the other curve's box cannot meet it, and the rest are paired by a sweep
    // along x instead of every piece against every piece (a long spline has about a thousand).
    {
        const BBox2 ba = A.bounds().inflated(ftol), bbx = B.bounds().inflated(ftol);
        std::erase_if(pa, [&](const Piece& x) { return !x.box.intersects(bbx); });
        std::erase_if(pb, [&](const Piece& y) { return !y.box.intersects(ba); });
    }
    std::sort(pb.begin(), pb.end(),
              [](const Piece& l, const Piece& r) { return l.box.min.x < r.box.min.x; });
    double widest = 0;
    for (const Piece& y : pb)
        widest = std::max(widest, y.box.max.x - y.box.min.x);
    double acc = 1e-9 + 32 * kEps * maxAbs;
    std::size_t first = out.size();
    std::size_t guard = 0;
    for (const Piece& x : pa) {
        // Only pieces starting between x.min - widest and x.max can overlap it in x.
        auto from = std::lower_bound(pb.begin(), pb.end(), x.box.min.x - widest,
                                     [](const Piece& y, double v) { return y.box.min.x < v; });
        for (auto yi = from; yi != pb.end() && yi->box.min.x <= x.box.max.x; ++yi) {
            const Piece& y = *yi;
            if (!x.box.intersects(y.box))
                continue;
            if (++guard > 400000)
                return;
            // Starting guess: crossing of the two chords.
            Vec2 r = x.p1 - x.p0, s = y.p1 - y.p0;
            double den = cross(r, s);
            double u = 0.5, v = 0.5;
            if (std::abs(den) > 1e-12 * length(r) * length(s)) {
                u = std::clamp(cross(y.p0 - x.p0, s) / den, 0.0, 1.0);
                v = std::clamp(cross(y.p0 - x.p0, r) / den, 0.0, 1.0);
            }
            double ta = x.t0 + (x.t1 - x.t0) * u, tb = y.t0 + (y.t1 - y.t0) * v;
            if (!refine(A, B, ta, tb, acc))
                continue;
            Intersection hit{A.pointAt(ta), ta, tb};
            // Map extended-line parameters back to the original line parameterisation.
            if (lineExtA)
                hit.tA = -kA + ta * (1 + 2 * kA);
            else if (extA)
                hit.tA = A0.wrapParam(ta);
            if (lineExtB)
                hit.tB = -kB + tb * (1 + 2 * kB);
            else if (extB)
                hit.tB = B0.wrapParam(tb);
            addUnique(out, first, hit);
        }
    }
}

}  // namespace

void intersect(const CurveRef& a, const CurveRef& b, std::vector<Intersection>& out, bool extendA,
               bool extendB) {
    using K = CurveRef::Kind;
    auto swapped = [&](auto fn) {
        Out tmp;
        fn(tmp);
        for (Intersection& i : tmp) {
            std::swap(i.tA, i.tB);
            out.push_back(i);
        }
    };
    if (a.kind() == K::Line && b.kind() == K::Line) {
        lineLine(a, b, extendA, extendB, out);
    } else if (a.kind() == K::Line && b.kind() == K::Arc) {
        lineArc(a, b, extendA, extendB, out);
    } else if (a.kind() == K::Arc && b.kind() == K::Line) {
        swapped([&](Out& t) { lineArc(b, a, extendB, extendA, t); });
    } else if (a.kind() == K::Arc && b.kind() == K::Arc) {
        arcArc(a, b, extendA, extendB, out);
    } else if (a.kind() == K::Line && b.kind() == K::Ellipse) {
        lineEllipse(a, b, extendA, extendB, out);
    } else if (a.kind() == K::Ellipse && b.kind() == K::Line) {
        swapped([&](Out& t) { lineEllipse(b, a, extendB, extendA, t); });
    } else {
        numeric(a, b, extendA, extendB, out);
    }
}

}  // namespace tbcad::geom

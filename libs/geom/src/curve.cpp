#include "tbcad/geom/curve.h"

#include <algorithm>
#include <cmath>

namespace tbcad::geom {

namespace {

constexpr int kMaxArcSegments = 4096;

Vec2 ellipseMinor(const Vec2& major, double ratio) {
    return perp(major) * ratio;
}

// 8-point Gauss-Legendre on [a, b].
template <class F>
double gauss8(F&& f, double a, double b) {
    static constexpr double x[4] = {0.1834346424956498, 0.5255324099163290, 0.7966664774136267,
                                    0.9602898564975363};
    static constexpr double w[4] = {0.3626837833783620, 0.3137066458778873, 0.2223810344533745,
                                    0.1012285362903763};
    double h = 0.5 * (b - a), c = 0.5 * (a + b), s = 0;
    for (int i = 0; i < 4; ++i)
        s += w[i] * (f(c - h * x[i]) + f(c + h * x[i]));
    return s * h;
}

}  // namespace

CurveRef CurveRef::line(Vec2 a, Vec2 b) {
    CurveRef c;
    c.kind_ = Kind::Line;
    c.p_ = a;
    c.q_ = b;
    c.t0_ = 0;
    c.sweep_ = 1;
    return c;
}

CurveRef CurveRef::arc(Vec2 center, double radius, double startAngle, double sweep) {
    CurveRef c;
    c.kind_ = Kind::Arc;
    c.p_ = center;
    c.r_ = radius;
    c.t0_ = startAngle;
    c.sweep_ = std::clamp(sweep, 0.0, kTwoPi);
    return c;
}

CurveRef CurveRef::ellipse(Vec2 center, Vec2 major, double ratio, double startParam, double sweep) {
    CurveRef c;
    c.kind_ = Kind::Ellipse;
    c.p_ = center;
    c.q_ = major;
    c.r_ = ratio;
    c.t0_ = startParam;
    c.sweep_ = std::clamp(sweep, 0.0, kTwoPi);
    return c;
}

Result<CurveRef> CurveRef::spline(const SplineData& data) {
    auto r = SplineCurve::make(data);
    if (!r)
        return r.error();
    auto sp = std::move(r).value();
    return spline(sp, sp->tMin(), sp->tMax());
}

CurveRef CurveRef::spline(std::shared_ptr<const SplineCurve> curve, double t0, double t1) {
    CurveRef c;
    c.kind_ = Kind::Spline;
    c.t0_ = t0;
    c.sweep_ = t1 - t0;
    c.sp_ = std::move(curve);
    return c;
}

bool CurveRef::isClosed() const {
    switch (kind_) {
        case Kind::Line:
            return false;
        case Kind::Arc:
        case Kind::Ellipse:
            return sweep_ >= kTwoPi - 1e-12;
        case Kind::Spline:
            return distance(startPoint(), endPoint()) <= tol::kLinear;
    }
    return false;
}

Vec2 CurveRef::pointAt(double t) const {
    switch (kind_) {
        case Kind::Line:
            return lerp(p_, q_, t);
        case Kind::Arc:
            return {p_.x + r_ * std::cos(t), p_.y + r_ * std::sin(t)};
        case Kind::Ellipse: {
            Vec2 m = ellipseMinor(q_, r_);
            double c = std::cos(t), s = std::sin(t);
            return {p_.x + q_.x * c + m.x * s, p_.y + q_.y * c + m.y * s};
        }
        case Kind::Spline:
            return sp_->pointAt(t);
    }
    return {};
}

Vec2 CurveRef::derivativeAt(double t) const {
    switch (kind_) {
        case Kind::Line:
            return q_ - p_;
        case Kind::Arc:
            return {-r_ * std::sin(t), r_ * std::cos(t)};
        case Kind::Ellipse: {
            Vec2 m = ellipseMinor(q_, r_);
            double c = std::cos(t), s = std::sin(t);
            return {-q_.x * s + m.x * c, -q_.y * s + m.y * c};
        }
        case Kind::Spline:
            return sp_->derivativeAt(t);
    }
    return {};
}

Vec2 CurveRef::tangentAt(double t) const {
    return normalized(derivativeAt(t));
}

double CurveRef::wrapParam(double t) const {
    if (kind_ == Kind::Arc || kind_ == Kind::Ellipse)
        return t0_ + normalizeAngle(t - t0_);
    return t;
}

bool CurveRef::containsParam(double t, double slack) const {
    switch (kind_) {
        case Kind::Line: {
            double len = length();
            double e = len > 0 ? slack / len : 0;
            return t >= -e && t <= 1 + e;
        }
        case Kind::Arc: {
            if (sweep_ >= kTwoPi - 1e-12)
                return true;
            double off = normalizeAngle(t - t0_);
            double e = r_ > 0 ? slack / r_ : 0;
            return off <= sweep_ + e || off >= kTwoPi - e;
        }
        case Kind::Ellipse: {
            if (sweep_ >= kTwoPi - 1e-12)
                return true;
            double off = normalizeAngle(t - t0_);
            double e = slack / std::max(tbcad::length(q_) * std::min(1.0, r_), 1e-300);
            return off <= sweep_ + e || off >= kTwoPi - e;
        }
        case Kind::Spline: {
            double e = 1e-12 * std::max(1.0, std::abs(t1() - t0_));
            return t >= t0_ - e && t <= t1() + e;
        }
    }
    return false;
}

double CurveRef::paramAt(Vec2 p) const {
    switch (kind_) {
        case Kind::Line: {
            Vec2 d = q_ - p_;
            double dd = dot(d, d);
            if (dd <= 0)
                return 0;
            return std::clamp(dot(p - p_, d) / dd, 0.0, 1.0);
        }
        case Kind::Arc: {
            Vec2 v = p - p_;
            double ang = (v.x == 0 && v.y == 0) ? t0_ : std::atan2(v.y, v.x);
            double off = normalizeAngle(ang - t0_);
            if (sweep_ >= kTwoPi - 1e-12)
                return t0_ + off;
            if (off <= sweep_)
                return t0_ + off;
            // Outside the arc: nearest end point.
            double dEnd = off - sweep_, dStart = kTwoPi - off;
            return dEnd < dStart ? t1() : t0_;
        }
        case Kind::Ellipse:
        case Kind::Spline:
            break;
    }
    // Numeric: coarse samples, then golden-section refinement around every local minimum of the
    // sampled distance (a curve can pass near the query point on several branches).
    double ta = t0_, tb = t1();
    std::vector<double> st;
    std::vector<Vec2> sp;
    if (kind_ == Kind::Ellipse) {
        int n = 64;
        st.reserve(n + 1);
        sp.reserve(n + 1);
        for (int i = 0; i <= n; ++i) {
            double t = ta + (tb - ta) * i / n;
            st.push_back(t);
            sp.push_back(pointAt(t));
        }
    } else {
        const auto& all = sp_->sampleParams();
        const auto& allp = sp_->samplePoints();
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i] < ta - 1e-15 || all[i] > tb + 1e-15)
                continue;
            st.push_back(all[i]);
            sp.push_back(allp[i]);
        }
        if (st.empty() || st.front() > ta + 1e-15) {
            st.insert(st.begin(), ta);
            sp.insert(sp.begin(), pointAt(ta));
        }
        if (st.back() < tb - 1e-15) {
            st.push_back(tb);
            sp.push_back(pointAt(tb));
        }
    }
    std::size_t n = st.size();
    std::vector<double> d2(n);
    for (std::size_t i = 0; i < n; ++i)
        d2[i] = lengthSquared(sp[i] - p);
    std::vector<std::size_t> seeds;
    for (std::size_t i = 0; i < n; ++i) {
        bool lo = i == 0 || d2[i] <= d2[i - 1];
        bool hi = i + 1 == n || d2[i] <= d2[i + 1];
        if (lo && hi)
            seeds.push_back(i);
    }
    std::sort(seeds.begin(), seeds.end(),
              [&](std::size_t a, std::size_t b) { return d2[a] < d2[b]; });
    if (seeds.size() > 6)
        seeds.resize(6);
    double bestT = st[seeds.empty() ? 0 : seeds[0]];
    double bestD = std::numeric_limits<double>::infinity();
    constexpr double gr = 0.6180339887498949;
    for (std::size_t si : seeds) {
        double lo = st[si > 0 ? si - 1 : si], hi = st[si + 1 < n ? si + 1 : si];
        double c = hi - gr * (hi - lo), d = lo + gr * (hi - lo);
        double fc = lengthSquared(pointAt(c) - p), fd = lengthSquared(pointAt(d) - p);
        for (int it = 0; it < 80 && hi - lo > 1e-15 * std::max(1.0, std::abs(hi)); ++it) {
            if (fc < fd) {
                hi = d;
                d = c;
                fd = fc;
                c = hi - gr * (hi - lo);
                fc = lengthSquared(pointAt(c) - p);
            } else {
                lo = c;
                c = d;
                fc = fd;
                d = lo + gr * (hi - lo);
                fd = lengthSquared(pointAt(d) - p);
            }
        }
        double t = 0.5 * (lo + hi);
        double ft = lengthSquared(pointAt(t) - p);
        if (ft < bestD) {
            bestD = ft;
            bestT = t;
        }
        if (d2[si] < bestD) {
            bestD = d2[si];
            bestT = st[si];
        }
    }
    // End points may beat an interior minimum on open curves.
    for (double te : {ta, tb}) {
        double fe = lengthSquared(pointAt(te) - p);
        if (fe < bestD) {
            bestD = fe;
            bestT = te;
        }
    }
    return bestT;
}

double CurveRef::lengthBetween(double ta, double tb) const {
    if (tb < ta)
        std::swap(ta, tb);
    switch (kind_) {
        case Kind::Line:
            return (tb - ta) * distance(p_, q_);
        case Kind::Arc:
            return r_ * (tb - ta);
        case Kind::Ellipse:
        case Kind::Spline:
            break;
    }
    std::vector<double> cuts;
    cuts.push_back(ta);
    if (kind_ == Kind::Spline) {
        for (double b : sp_->breaks())
            if (b > ta && b < tb)
                cuts.push_back(b);
    }
    cuts.push_back(tb);
    int sub = kind_ == Kind::Ellipse ? 16 : 6;
    auto speed = [&](double t) { return tbcad::length(derivativeAt(t)); };
    double total = 0;
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        for (int k = 0; k < sub; ++k) {
            double a = cuts[i] + (cuts[i + 1] - cuts[i]) * k / sub;
            double b = cuts[i] + (cuts[i + 1] - cuts[i]) * (k + 1) / sub;
            total += gauss8(speed, a, b);
        }
    }
    return total;
}

double CurveRef::length() const {
    return lengthBetween(t0(), t1());
}

double CurveRef::paramAtLength(double s) const {
    double total = length();
    if (s <= 0)
        return t0();
    if (s >= total)
        return t1();
    if (kind_ == Kind::Line)
        return s / total;
    if (kind_ == Kind::Arc)
        return t0_ + s / r_;
    // One pass over the curve gives the length of every integration piece; the piece that holds s
    // is then bisected on its own, so a call costs about one length() instead of eighty.
    std::vector<double> cuts;
    cuts.push_back(t0());
    if (kind_ == Kind::Spline) {
        for (double b : sp_->breaks())
            if (b > t0() && b < t1())
                cuts.push_back(b);
    }
    cuts.push_back(t1());
    const int sub = kind_ == Kind::Ellipse ? 16 : 6;
    auto speed = [&](double t) { return tbcad::length(derivativeAt(t)); };
    double run = 0;
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        for (int k = 0; k < sub; ++k) {
            const double a = cuts[i] + (cuts[i + 1] - cuts[i]) * k / sub;
            const double b = cuts[i] + (cuts[i + 1] - cuts[i]) * (k + 1) / sub;
            const double piece = gauss8(speed, a, b);
            if (run + piece >= s) {
                double lo = a, hi = b;
                for (int it = 0; it < 60; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    if (run + gauss8(speed, a, mid) < s)
                        lo = mid;
                    else
                        hi = mid;
                }
                return 0.5 * (lo + hi);
            }
            run += piece;
        }
    }
    return t1();
}

std::vector<Vec2> CurveRef::pointsAtEqualLength(int n) const {
    std::vector<Vec2> out;
    if (n < 1)
        return out;
    out.reserve(static_cast<std::size_t>(n) + 1);
    if (kind_ != Kind::Ellipse && kind_ != Kind::Spline) {
        const double total = length();
        for (int i = 0; i <= n; ++i)
            out.push_back(pointAt(paramAtLength(total * i / n)));
        return out;
    }
    std::vector<double> cuts;
    cuts.push_back(t0());
    if (kind_ == Kind::Spline) {
        for (double b : sp_->breaks())
            if (b > t0() && b < t1())
                cuts.push_back(b);
    }
    cuts.push_back(t1());
    const int sub = kind_ == Kind::Ellipse ? 16 : 6;
    auto speed = [&](double t) { return tbcad::length(derivativeAt(t)); };
    struct Piece {
        double a, b, start, len;
    };
    std::vector<Piece> pieces;
    double total = 0;
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        for (int k = 0; k < sub; ++k) {
            const double a = cuts[i] + (cuts[i + 1] - cuts[i]) * k / sub;
            const double b = cuts[i] + (cuts[i + 1] - cuts[i]) * (k + 1) / sub;
            const double l = gauss8(speed, a, b);
            pieces.push_back({a, b, total, l});
            total += l;
        }
    }
    std::size_t pi = 0;
    for (int i = 0; i <= n; ++i) {
        const double s = total * i / n;
        if (s <= 0) {
            out.push_back(pointAt(t0()));
            continue;
        }
        if (s >= total) {
            out.push_back(pointAt(t1()));
            continue;
        }
        while (pi + 1 < pieces.size() && pieces[pi].start + pieces[pi].len < s)
            ++pi;
        const Piece& pc = pieces[pi];
        const double want = s - pc.start;
        double lo = pc.a, hi = pc.b;
        double t = pc.len > 0 ? pc.a + (pc.b - pc.a) * std::clamp(want / pc.len, 0.0, 1.0)
                              : 0.5 * (pc.a + pc.b);
        for (int it = 0; it < 60; ++it) {
            const double f = gauss8(speed, pc.a, t) - want;
            if (f < 0)
                lo = t;
            else
                hi = t;
            if (std::abs(f) <= 1e-13 * std::max(total, 1.0) || hi - lo <= 1e-15 * std::abs(hi))
                break;
            const double v = speed(t);
            double next = v > 0 ? t - f / v : 0.5 * (lo + hi);
            if (!(next > lo && next < hi))
                next = 0.5 * (lo + hi);
            t = next;
        }
        out.push_back(pointAt(t));
    }
    return out;
}

BBox2 CurveRef::bounds() const {
    BBox2 b;
    switch (kind_) {
        case Kind::Line:
            b.expand(p_);
            b.expand(q_);
            return b;
        case Kind::Arc: {
            b.expand(startPoint());
            b.expand(endPoint());
            for (int k = 0; k < 4; ++k) {
                double a = k * kPi / 2;
                if (normalizeAngle(a - t0_) <= sweep_)
                    b.expand({p_.x + r_ * std::cos(a), p_.y + r_ * std::sin(a)});
            }
            return b;
        }
        case Kind::Ellipse: {
            b.expand(startPoint());
            b.expand(endPoint());
            Vec2 m = ellipseMinor(q_, r_);
            double tx = std::atan2(m.x, q_.x), ty = std::atan2(m.y, q_.y);
            for (double base : {tx, ty})
                for (double a : {base, base + kPi})
                    if (normalizeAngle(a - t0_) <= sweep_)
                        b.expand(pointAt(a));
            return b;
        }
        case Kind::Spline: {
            if (t0_ <= sp_->tMin() && t1() >= sp_->tMax())
                return sp_->bounds();
            b.expand(startPoint());
            b.expand(endPoint());
            const auto& st = sp_->sampleParams();
            const auto& sp = sp_->samplePoints();
            for (std::size_t i = 0; i < st.size(); ++i)
                if (st[i] >= t0_ && st[i] <= t1())
                    b.expand(sp[i]);
            return b.inflated(sp_->sampleDeviation());
        }
    }
    return b;
}

Result<CurveRef> CurveRef::subcurve(double ta, double tb) const {
    if (!(tb > ta))
        return makeError(ErrorCode::InvalidArgument, "subcurve range is empty");
    switch (kind_) {
        case Kind::Line: {
            if (ta < -1e-12 || tb > 1 + 1e-12)
                return makeError(ErrorCode::OutOfRange, "line parameter out of range");
            return line(pointAt(ta), pointAt(tb));
        }
        case Kind::Arc:
            if (tb - ta > kTwoPi + 1e-12)
                return makeError(ErrorCode::OutOfRange, "arc sweep exceeds a full circle");
            return arc(p_, r_, ta, tb - ta);
        case Kind::Ellipse:
            if (tb - ta > kTwoPi + 1e-12)
                return makeError(ErrorCode::OutOfRange, "ellipse sweep exceeds a full ellipse");
            return ellipse(p_, q_, r_, ta, tb - ta);
        case Kind::Spline:
            if (ta < t0_ - 1e-12 || tb > t1() + 1e-12)
                return makeError(ErrorCode::OutOfRange, "spline parameter out of range");
            return spline(sp_, std::max(ta, t0_), std::min(tb, t1()));
    }
    return makeError(ErrorCode::Generic, "unreachable");
}

Result<std::pair<CurveRef, CurveRef>> CurveRef::split(double t) const {
    double len = length();
    double a = lengthBetween(t0(), t), b = lengthBetween(t, t1());
    if (t < t0() || t > t1() || a <= tol::kLinear || b <= tol::kLinear || len <= 0)
        return makeError(ErrorCode::OutOfRange, "split parameter is not interior to the curve");
    auto l = subcurve(t0(), t);
    auto r = subcurve(t, t1());
    if (!l)
        return l.error();
    if (!r)
        return r.error();
    return std::make_pair(std::move(l).value(), std::move(r).value());
}

// ---------------------------------------------------------------------------------------------
// Flattening

namespace {

void flattenAdaptive(const CurveRef& c, double ta, double tb, const Vec2& pa, const Vec2& pb,
                     double tol, int depth, std::vector<Vec2>& out, std::vector<double>* params) {
    double tm = 0.5 * (ta + tb);
    Vec2 pm = c.pointAt(tm);
    Vec2 chord = pb - pa;
    double cl2 = lengthSquared(chord);
    double dev;
    if (cl2 <= 0) {
        dev = distance(pm, pa);
    } else {
        double u = std::clamp(dot(pm - pa, chord) / cl2, 0.0, 1.0);
        dev = distance(pm, pa + chord * u);
        // A midpoint that lies off the chord's span means a loop or cusp: keep refining.
        double u0 = dot(pm - pa, chord) / cl2;
        if (u0 < 0 || u0 > 1)
            dev = std::max(dev, tol * 2);
    }
    if (dev > tol && depth < 18) {
        flattenAdaptive(c, ta, tm, pa, pm, tol, depth + 1, out, params);
        flattenAdaptive(c, tm, tb, pm, pb, tol, depth + 1, out, params);
    } else {
        out.push_back(pb);
        if (params)
            params->push_back(tb);
    }
}

}  // namespace

void flatten(const CurveRef& c, double tol, std::vector<Vec2>& out, std::vector<double>* params) {
    if (!(tol > 0))
        tol = 1e-3;
    double ta = c.t0(), tb = c.t1();
    switch (c.kind()) {
        case CurveRef::Kind::Line:
            out.push_back(c.lineA());
            out.push_back(c.lineB());
            if (params) {
                params->push_back(0);
                params->push_back(1);
            }
            return;
        case CurveRef::Kind::Arc: {
            double r = c.radius();
            int n = 4;
            if (r > 0) {
                double arg = std::clamp(1.0 - tol / r, -1.0, 1.0);
                double step = 2.0 * std::acos(arg);
                if (step > 0)
                    n = static_cast<int>(std::ceil(c.sweep() / step));
                else
                    n = kMaxArcSegments;
            }
            n = std::clamp(n, 4, kMaxArcSegments);
            for (int i = 0; i <= n; ++i) {
                double t = i == n ? tb : ta + c.sweep() * i / n;
                out.push_back(c.pointAt(t));
                if (params)
                    params->push_back(t);
            }
            return;
        }
        case CurveRef::Kind::Ellipse:
        case CurveRef::Kind::Spline: {
            std::vector<double> seeds;
            if (c.kind() == CurveRef::Kind::Ellipse) {
                int n = std::max(8, static_cast<int>(std::ceil(32 * c.sweep() / kTwoPi)));
                for (int i = 0; i <= n; ++i)
                    seeds.push_back(i == n ? tb : ta + c.sweep() * i / n);
            } else {
                seeds.push_back(ta);
                for (double b : c.splineCurve()->breaks()) {
                    if (b <= ta || b >= tb)
                        continue;
                    seeds.push_back(b);
                }
                seeds.push_back(tb);
                // Four initial pieces per span for robustness against S-shaped spans.
                std::vector<double> dense;
                for (std::size_t i = 0; i + 1 < seeds.size(); ++i)
                    for (int k = 0; k < 4; ++k)
                        dense.push_back(seeds[i] + (seeds[i + 1] - seeds[i]) * k / 4);
                dense.push_back(tb);
                seeds = std::move(dense);
            }
            Vec2 prev = c.pointAt(seeds.front());
            out.push_back(prev);
            if (params)
                params->push_back(seeds.front());
            for (std::size_t i = 0; i + 1 < seeds.size(); ++i) {
                Vec2 next = c.pointAt(seeds[i + 1]);
                flattenAdaptive(c, seeds[i], seeds[i + 1], prev, next, tol, 0, out, params);
                prev = next;
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Polyline helpers

std::optional<BulgeArc> bulgeArc(Vec2 p0, Vec2 p1, double bulge) {
    Vec2 chord = p1 - p0;
    double len = length(chord);
    if (!(len > 0) || std::abs(bulge) < 1e-12)
        return std::nullopt;
    double ab = std::abs(bulge);
    BulgeArc a;
    a.sweep = 4.0 * std::atan(ab);
    a.radius = len * 0.5 * (1.0 + bulge * bulge) / (2.0 * ab);
    Vec2 mid = (p0 + p1) * 0.5;
    double d = len * 0.5 * (1.0 - bulge * bulge) / (2.0 * bulge);  // signed, left of the chord
    a.center = mid + perp(chord / len) * d;
    a.reversed = bulge < 0;
    Vec2 s = a.reversed ? p1 : p0;
    a.startAngle = normalizeAngle(std::atan2(s.y - a.center.y, s.x - a.center.x));
    return a;
}

std::optional<double> bulgeThrough(Vec2 a, Vec2 b, Vec2 c) {
    Vec2 ba = a - b, bc = c - b;
    double cr = cross(bc, ba);  // sign: which side of the chord b lies on
    double la = length(ba), lc = length(bc);
    if (la <= 0 || lc <= 0 || std::abs(cr) <= 1e-12 * la * lc)
        return std::nullopt;
    double theta = std::atan2(std::abs(cr), dot(ba, bc));  // inscribed angle at b
    double mag = std::tan((kPi - theta) * 0.5);
    // b to the right of a->c means a counter-clockwise arc (positive bulge).
    double side = cross(c - a, b - a);
    return side < 0 ? mag : -mag;
}

Result<ArcData> arcFromThreePoints(Vec2 a, Vec2 b, Vec2 c) {
    auto bu = bulgeThrough(a, b, c);
    if (!bu)
        return makeError(ErrorCode::InvalidArgument, "three points are collinear");
    auto arc = bulgeArc(a, c, *bu);
    ArcData d;
    d.center = arc->center;
    d.radius = arc->radius;
    d.startAngle = arc->startAngle;
    d.endAngle = normalizeAngle(arc->startAngle + arc->sweep);
    return d;
}

double Segment::toCurveParam(double u) const {
    switch (curve.kind()) {
        case CurveRef::Kind::Line:
            return reversed ? 1.0 - u : u;
        default:
            return reversed ? curve.t1() - u * curve.sweep() : curve.t0() + u * curve.sweep();
    }
}

double Segment::toLocalUnbounded(double t) const {
    double u;
    if (curve.kind() == CurveRef::Kind::Line) {
        u = t;
    } else {
        double sweep = curve.sweep();
        double off = normalizeAngle(t - curve.t0());
        if (off <= sweep + 1e-9)
            u = std::min(off, sweep) / sweep;
        else if (kTwoPi - off < off - sweep)
            u = -(kTwoPi - off) / sweep;
        else
            u = 1.0 + (off - sweep) / sweep;
    }
    return reversed ? 1.0 - u : u;
}

double Segment::toLocal(double t) const {
    return std::clamp(toLocalUnbounded(t), 0.0, 1.0);
}

std::vector<Segment> segments(const PolylineData& pl) {
    std::vector<Segment> out;
    int n = static_cast<int>(pl.v.size());
    int count = segmentCount(pl);
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const PolyVertex& a = pl.v[static_cast<std::size_t>(i)];
        const PolyVertex& b = pl.v[static_cast<std::size_t>((i + 1) % n)];
        if (distance(a.p, b.p) <= 0)
            continue;
        Segment s;
        s.index = i;
        s.p0 = a.p;
        s.p1 = b.p;
        s.bulge = a.bulge;
        if (auto arc = bulgeArc(a.p, b.p, a.bulge)) {
            s.curve = CurveRef::arc(arc->center, arc->radius, arc->startAngle, arc->sweep);
            s.reversed = arc->reversed;
        } else {
            s.curve = CurveRef::line(a.p, b.p);
            s.bulge = 0;
        }
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace tbcad::geom

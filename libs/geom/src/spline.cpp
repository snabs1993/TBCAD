#include "tbcad/geom/spline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace tbcad::geom {

namespace {

constexpr int kMaxDegree = 15;
constexpr int kSamplesPerSpan = 16;

struct Hom {
    double x = 0, y = 0, w = 1;
};

std::vector<double> clampedUniformKnots(int ncp, int p) {
    std::vector<double> k(static_cast<std::size_t>(ncp + p + 1));
    int interior = ncp - p - 1;
    for (int i = 0; i <= p; ++i) {
        k[static_cast<std::size_t>(i)] = 0;
        k[static_cast<std::size_t>(ncp + i)] = 1;
    }
    for (int j = 1; j <= interior; ++j)
        k[static_cast<std::size_t>(p + j)] = static_cast<double>(j) / (interior + 1);
    return k;
}

}  // namespace

Result<std::shared_ptr<const SplineCurve>> SplineCurve::make(const SplineData& data) {
    SplineData src = data;
    if (src.control.empty()) {
        if (src.fit.size() < 2)
            return makeError(ErrorCode::InvalidArgument, "spline has no control or fit points");
        InterpolateOptions o;
        o.degree = src.degree;
        o.closed = src.closed;
        auto r = interpolateSpline(src.fit, o);
        if (!r)
            return r.error();
        src = std::move(r).value();
    }
    int p = src.degree;
    int ncp = static_cast<int>(src.control.size());
    if (p < 1 || p > kMaxDegree)
        return makeError(ErrorCode::InvalidArgument, "spline degree out of range (1..15)");
    if (ncp < p + 1)
        return makeError(ErrorCode::InvalidArgument,
                         "spline needs at least degree+1 control points");
    if (!src.weights.empty() && src.weights.size() != src.control.size())
        return makeError(ErrorCode::InvalidArgument, "spline weights do not match control points");
    for (double w : src.weights)
        if (!(w > 0) || !std::isfinite(w))
            return makeError(ErrorCode::InvalidArgument, "spline weights must be positive");
    for (const Vec2& c : src.control)
        if (!isFinite(c))
            return makeError(ErrorCode::InvalidArgument, "spline control point is not finite");

    std::shared_ptr<SplineCurve> s(new SplineCurve());
    s->degree_ = p;
    s->ctrl_ = src.control;
    s->weights_ = src.weights;
    s->rational_ = !src.weights.empty() &&
                   std::any_of(src.weights.begin(), src.weights.end(),
                               [&](double w) { return std::abs(w - src.weights.front()) > 1e-14; });
    if (!s->rational_)
        s->weights_.clear();
    if (src.knots.empty()) {
        s->knots_ = clampedUniformKnots(ncp, p);
    } else {
        if (static_cast<int>(src.knots.size()) != ncp + p + 1)
            return makeError(ErrorCode::InvalidArgument, "spline knot count does not match");
        if (!std::is_sorted(src.knots.begin(), src.knots.end()))
            return makeError(ErrorCode::InvalidArgument, "spline knots are not ascending");
        s->knots_ = src.knots;
    }
    s->tMin_ = s->knots_[static_cast<std::size_t>(p)];
    s->tMax_ = s->knots_[static_cast<std::size_t>(ncp)];
    if (!(s->tMax_ > s->tMin_))
        return makeError(ErrorCode::InvalidArgument, "spline has an empty parameter domain");

    // Derivative control points.
    std::size_t nd = static_cast<std::size_t>(ncp - 1);
    s->dq_x_.resize(nd);
    s->dq_y_.resize(nd);
    s->dq_w_.resize(nd);
    auto homAt = [&](int i) {
        double w = s->rational_ ? s->weights_[static_cast<std::size_t>(i)] : 1.0;
        const Vec2& c = s->ctrl_[static_cast<std::size_t>(i)];
        return Hom{c.x * w, c.y * w, w};
    };
    for (int i = 0; i + 1 < ncp; ++i) {
        double den = s->knots_[static_cast<std::size_t>(i + p + 1)] -
                     s->knots_[static_cast<std::size_t>(i + 1)];
        double f = den > 0 ? p / den : 0.0;
        Hom a = homAt(i), b = homAt(i + 1);
        s->dq_x_[static_cast<std::size_t>(i)] = f * (b.x - a.x);
        s->dq_y_[static_cast<std::size_t>(i)] = f * (b.y - a.y);
        s->dq_w_[static_cast<std::size_t>(i)] = f * (b.w - a.w);
    }
    s->dknots_.assign(s->knots_.begin() + 1, s->knots_.end() - 1);

    s->breaks_.push_back(s->tMin_);
    for (double k : s->knots_)
        if (k > s->breaks_.back() && k <= s->tMax_)
            s->breaks_.push_back(k);
    s->buildSamples();
    return std::shared_ptr<const SplineCurve>(std::move(s));
}

int SplineCurve::spanOf(double t) const {
    int n = static_cast<int>(ctrl_.size()) - 1;
    if (t >= knots_[static_cast<std::size_t>(n + 1)])
        return n;
    if (t <= knots_[static_cast<std::size_t>(degree_)])
        return degree_;
    int lo = degree_, hi = n + 1;
    int mid = (lo + hi) / 2;
    while (t < knots_[static_cast<std::size_t>(mid)] ||
           t >= knots_[static_cast<std::size_t>(mid + 1)]) {
        if (t < knots_[static_cast<std::size_t>(mid)])
            hi = mid;
        else
            lo = mid;
        mid = (lo + hi) / 2;
    }
    return mid;
}

void SplineCurve::basis(int span, double t, int p, const std::vector<double>& u,
                        double* out) const {
    std::array<double, kMaxDegree + 2> left{}, right{};
    out[0] = 1.0;
    for (int j = 1; j <= p; ++j) {
        left[static_cast<std::size_t>(j)] = t - u[static_cast<std::size_t>(span + 1 - j)];
        right[static_cast<std::size_t>(j)] = u[static_cast<std::size_t>(span + j)] - t;
        double saved = 0;
        for (int r = 0; r < j; ++r) {
            double den =
                right[static_cast<std::size_t>(r + 1)] + left[static_cast<std::size_t>(j - r)];
            double temp = den != 0 ? out[r] / den : 0.0;
            out[r] = saved + right[static_cast<std::size_t>(r + 1)] * temp;
            saved = left[static_cast<std::size_t>(j - r)] * temp;
        }
        out[j] = saved;
    }
}

Vec2 SplineCurve::pointAt(double t) const {
    t = std::clamp(t, tMin_, tMax_);
    int s = spanOf(t);
    std::array<double, kMaxDegree + 1> n{};
    basis(s, t, degree_, knots_, n.data());
    double x = 0, y = 0, w = 0;
    for (int r = 0; r <= degree_; ++r) {
        std::size_t i = static_cast<std::size_t>(s - degree_ + r);
        double wi = rational_ ? weights_[i] : 1.0;
        double b = n[static_cast<std::size_t>(r)] * wi;
        x += b * ctrl_[i].x;
        y += b * ctrl_[i].y;
        w += b;
    }
    return {x / w, y / w};
}

Vec2 SplineCurve::derivativeAt(double t) const {
    t = std::clamp(t, tMin_, tMax_);
    int s = spanOf(t);
    int p = degree_;
    std::array<double, kMaxDegree + 1> n{};
    // Value (needed for the rational quotient rule).
    double w = 0, wx = 0, wy = 0;
    {
        std::array<double, kMaxDegree + 1> b{};
        basis(s, t, p, knots_, b.data());
        for (int r = 0; r <= p; ++r) {
            std::size_t i = static_cast<std::size_t>(s - p + r);
            double wi = rational_ ? weights_[i] : 1.0;
            double c = b[static_cast<std::size_t>(r)] * wi;
            wx += c * ctrl_[i].x;
            wy += c * ctrl_[i].y;
            w += c;
        }
    }
    double dx = 0, dy = 0, dw = 0;
    basis(s - 1, t, p - 1, dknots_, n.data());
    for (int r = 0; r <= p - 1; ++r) {
        std::size_t i = static_cast<std::size_t>(s - p + r);
        if (i >= dq_x_.size())
            continue;
        double b = n[static_cast<std::size_t>(r)];
        dx += b * dq_x_[i];
        dy += b * dq_y_[i];
        dw += b * dq_w_[i];
    }
    Vec2 c{wx / w, wy / w};
    return {(dx - dw * c.x) / w, (dy - dw * c.y) / w};
}

void SplineCurve::buildSamples() {
    sampleT_.clear();
    sampleP_.clear();
    double dev = 0;
    for (std::size_t b = 0; b + 1 < breaks_.size(); ++b) {
        double a = breaks_[b], e = breaks_[b + 1];
        for (int k = 0; k < kSamplesPerSpan; ++k) {
            double t0 = a + (e - a) * k / kSamplesPerSpan;
            double t1 = a + (e - a) * (k + 1) / kSamplesPerSpan;
            Vec2 p0 = k == 0 && b > 0 ? sampleP_.back() : pointAt(t0);
            Vec2 p1 = pointAt(t1);
            if (sampleT_.empty()) {
                sampleT_.push_back(t0);
                sampleP_.push_back(p0);
            }
            Vec2 m = pointAt(0.5 * (t0 + t1));
            Vec2 chord = p1 - p0;
            double cl = length(chord);
            double d = cl > 0 ? std::abs(cross(chord, m - p0)) / cl : distance(m, p0);
            // Distance along the chord beyond the end points also counts.
            if (cl > 0) {
                double along = dot(chord, m - p0) / cl;
                if (along < 0)
                    d = std::max(d, -along);
                else if (along > cl)
                    d = std::max(d, along - cl);
            }
            dev = std::max(dev, d);
            sampleT_.push_back(t1);
            sampleP_.push_back(p1);
        }
    }
    sampleDev_ = 2.0 * dev + 1e-9;
    bounds_ = BBox2{};
    for (const Vec2& p : sampleP_)
        bounds_.expand(p);
    bounds_ = bounds_.inflated(sampleDev_);
}

// ---------------------------------------------------------------------------------------------
// Interpolation

namespace {

// Solves A x = b (b has two columns) for a square matrix stored row-major. Banded elimination
// without pivoting when `banded` > 0, dense with partial pivoting otherwise.
bool solveSystem(std::vector<double>& a, std::vector<Vec2>& b, int n, int band) {
    auto at = [&](int r, int c) -> double& {
        return a[static_cast<std::size_t>(r) * static_cast<std::size_t>(n) +
                 static_cast<std::size_t>(c)];
    };
    bool pivoting = band <= 0;
    for (int k = 0; k < n; ++k) {
        int rowEnd = pivoting ? n : std::min(n, k + band + 1);
        if (pivoting) {
            int best = k;
            for (int r = k + 1; r < n; ++r)
                if (std::abs(at(r, k)) > std::abs(at(best, k)))
                    best = r;
            if (best != k) {
                for (int c = 0; c < n; ++c)
                    std::swap(at(k, c), at(best, c));
                std::swap(b[static_cast<std::size_t>(k)], b[static_cast<std::size_t>(best)]);
            }
        }
        double piv = at(k, k);
        if (std::abs(piv) < 1e-13)
            return false;
        int colEnd = pivoting ? n : std::min(n, k + band + 1);
        for (int r = k + 1; r < rowEnd; ++r) {
            double f = at(r, k) / piv;
            if (f == 0)
                continue;
            for (int c = k; c < colEnd; ++c)
                at(r, c) -= f * at(k, c);
            b[static_cast<std::size_t>(r)] -= b[static_cast<std::size_t>(k)] * f;
        }
    }
    for (int k = n - 1; k >= 0; --k) {
        Vec2 s = b[static_cast<std::size_t>(k)];
        int colEnd = pivoting ? n : std::min(n, k + band + 1);
        for (int c = k + 1; c < colEnd; ++c)
            s -= b[static_cast<std::size_t>(c)] * at(k, c);
        b[static_cast<std::size_t>(k)] = s / at(k, k);
    }
    return true;
}

}  // namespace

Result<SplineData> interpolateSpline(std::span<const Vec2> fitPoints,
                                     const InterpolateOptions& opt) {
    // De-duplicate consecutive coincident points.
    std::vector<Vec2> q;
    for (const Vec2& p : fitPoints) {
        if (!isFinite(p))
            return makeError(ErrorCode::InvalidArgument, "fit point is not finite");
        if (q.empty() || distance(q.back(), p) > 1e-9)
            q.push_back(p);
    }
    bool closed = opt.closed;
    if (closed && q.size() >= 3) {
        if (distance(q.front(), q.back()) <= 1e-9)
            q.pop_back();
    } else {
        closed = false;
    }
    if (q.size() < 2)
        return makeError(ErrorCode::InvalidArgument, "need at least two distinct fit points");
    std::vector<Vec2> fitOut = q;

    std::optional<Vec2> t0 = opt.startTangent, t1 = opt.endTangent;
    if (closed) {
        Vec2 d = normalized(q[1] - q.back());
        t0 = d;
        t1 = d;
        q.push_back(q.front());
    }
    int n = static_cast<int>(q.size());
    int p = std::clamp(opt.degree, 1, kMaxDegree);
    p = std::min(p, n - 1);

    bool useTangents = t0.has_value() || t1.has_value() || !opt.natural;
    if (!useTangents && p < 2)
        useTangents = false;  // degree 1: no end conditions possible
    if (p < 2)
        useTangents = false;
    if (n == 2)
        useTangents = false;  // a straight segment needs no end conditions
    bool endCond = useTangents || (opt.natural && p >= 2 && n > 2 && !closed);
    if (closed)
        endCond = true;
    if (p < 2)
        endCond = false;
    int e = endCond ? 2 : 0;
    int m = n + e;

    std::vector<double> ub(static_cast<std::size_t>(n), 0.0);
    double total = 0;
    for (int i = 1; i < n; ++i) {
        total += distance(q[static_cast<std::size_t>(i)], q[static_cast<std::size_t>(i - 1)]);
        ub[static_cast<std::size_t>(i)] = total;
    }
    if (!(total > 0))
        return makeError(ErrorCode::InvalidArgument, "fit points are coincident");
    for (double& v : ub)
        v /= total;
    ub.back() = 1.0;

    std::vector<double> knots(static_cast<std::size_t>(m + p + 1));
    for (int i = 0; i <= p; ++i) {
        knots[static_cast<std::size_t>(i)] = 0;
        knots[static_cast<std::size_t>(m + i)] = 1;
    }
    if (e == 0) {
        for (int j = 1; j <= n - p - 1; ++j) {
            double s = 0;
            for (int i = j; i < j + p; ++i)
                s += ub[static_cast<std::size_t>(i)];
            knots[static_cast<std::size_t>(p + j)] = s / p;
        }
    } else {
        for (int j = 0; j <= n - p; ++j) {
            double s = 0;
            for (int i = j; i < j + p; ++i)
                s += ub[static_cast<std::size_t>(i)];
            knots[static_cast<std::size_t>(p + 1 + j)] = s / p;
        }
    }

    // Assemble the collocation system.
    std::vector<double> a(static_cast<std::size_t>(m) * static_cast<std::size_t>(m), 0.0);
    std::vector<Vec2> rhs(static_cast<std::size_t>(m));
    auto at = [&](int r, int c) -> double& {
        return a[static_cast<std::size_t>(r) * static_cast<std::size_t>(m) +
                 static_cast<std::size_t>(c)];
    };

    SplineData tmp;
    tmp.degree = p;
    tmp.knots = knots;
    tmp.control.assign(static_cast<std::size_t>(m), Vec2{});
    // Basis values via a throw-away curve per row would be slow; compute directly.
    auto spanOf = [&](double t) {
        int nn = m - 1;
        if (t >= knots[static_cast<std::size_t>(nn + 1)])
            return nn;
        int lo = p, hi = nn + 1, mid = (lo + hi) / 2;
        while (t < knots[static_cast<std::size_t>(mid)] ||
               t >= knots[static_cast<std::size_t>(mid + 1)]) {
            if (t < knots[static_cast<std::size_t>(mid)])
                hi = mid;
            else
                lo = mid;
            mid = (lo + hi) / 2;
        }
        return mid;
    };
    int row = 0;
    int dataRow0 = e ? 1 : 0;
    if (e) {
        Vec2 tan0{}, tan1{};
        if (useTangents) {
            auto est = [&](int i0, int i1) {
                Vec2 d = q[static_cast<std::size_t>(i1)] - q[static_cast<std::size_t>(i0)];
                return normalized(d);
            };
            tan0 = t0 ? normalized(*t0) : est(0, 1);
            tan1 = t1 ? normalized(*t1) : est(n - 2, n - 1);
            tan0 *= total;
            tan1 *= total;
            // Start: P1 - P0 = (u_{p+1}/p) * T0
            at(0, 0) = -1;
            at(0, 1) = 1;
            rhs[0] = tan0 * (knots[static_cast<std::size_t>(p + 1)] / p);
            at(m - 1, m - 2) = -1;
            at(m - 1, m - 1) = 1;
            rhs[static_cast<std::size_t>(m - 1)] =
                tan1 * ((1.0 - knots[static_cast<std::size_t>(m - 1)]) / p);
        } else {
            // Natural: zero second derivative at both ends.
            double d1 = knots[static_cast<std::size_t>(p + 2)] - knots[static_cast<std::size_t>(2)];
            double d0 = knots[static_cast<std::size_t>(p + 1)] - knots[static_cast<std::size_t>(1)];
            at(0, 0) = 1.0 / d0;
            at(0, 1) = -1.0 / d0 - 1.0 / d1;
            at(0, 2) = 1.0 / d1;
            double e1 =
                knots[static_cast<std::size_t>(m + p - 1)] - knots[static_cast<std::size_t>(m - 1)];
            double e0 =
                knots[static_cast<std::size_t>(m + p - 2)] - knots[static_cast<std::size_t>(m - 2)];
            // (P[m-1]-P[m-2])/e1 - (P[m-2]-P[m-3])/e0 = 0
            at(m - 1, m - 1) = 1.0 / e1;
            at(m - 1, m - 2) = -1.0 / e1 - 1.0 / e0;
            at(m - 1, m - 3) = 1.0 / e0;
        }
        row = 1;
    }
    for (int k = 0; k < n; ++k) {
        double t = ub[static_cast<std::size_t>(k)];
        int s = spanOf(t);
        std::array<double, kMaxDegree + 1> nb{};
        std::array<double, kMaxDegree + 2> left{}, right{};
        nb[0] = 1.0;
        for (int j = 1; j <= p; ++j) {
            left[static_cast<std::size_t>(j)] = t - knots[static_cast<std::size_t>(s + 1 - j)];
            right[static_cast<std::size_t>(j)] = knots[static_cast<std::size_t>(s + j)] - t;
            double saved = 0;
            for (int r = 0; r < j; ++r) {
                double den =
                    right[static_cast<std::size_t>(r + 1)] + left[static_cast<std::size_t>(j - r)];
                double temp = den != 0 ? nb[static_cast<std::size_t>(r)] / den : 0.0;
                nb[static_cast<std::size_t>(r)] =
                    saved + right[static_cast<std::size_t>(r + 1)] * temp;
                saved = left[static_cast<std::size_t>(j - r)] * temp;
            }
            nb[static_cast<std::size_t>(j)] = saved;
        }
        for (int r = 0; r <= p; ++r)
            at(row, s - p + r) = nb[static_cast<std::size_t>(r)];
        rhs[static_cast<std::size_t>(row)] = q[static_cast<std::size_t>(k)];
        ++row;
    }
    (void)dataRow0;

    std::vector<double> a2 = a;
    std::vector<Vec2> rhs2 = rhs;
    int band = p + 2;
    bool ok = m > 1500 ? solveSystem(a, rhs, m, band) : false;
    if (!ok && m <= 1500) {
        a = a2;
        rhs = rhs2;
        ok = solveSystem(a, rhs, m, 0);
    } else if (!ok) {
        return makeError(ErrorCode::Generic, "spline interpolation failed (singular system)");
    }
    if (!ok)
        return makeError(ErrorCode::Generic, "spline interpolation failed (singular system)");

    SplineData out;
    out.degree = p;
    out.knots = std::move(knots);
    out.control = std::move(rhs);
    out.fit = std::move(fitOut);
    out.closed = closed;
    out.periodic = false;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Knot insertion / sub-curve

namespace {

struct HomSpline {
    int p = 3;
    std::vector<double> u{};
    std::vector<Hom> q{};
};

int findSpan(const HomSpline& s, double t) {
    int n = static_cast<int>(s.q.size()) - 1;
    if (t >= s.u[static_cast<std::size_t>(n + 1)])
        return n;
    int lo = s.p, hi = n + 1, mid = (lo + hi) / 2;
    while (t < s.u[static_cast<std::size_t>(mid)] || t >= s.u[static_cast<std::size_t>(mid + 1)]) {
        if (t < s.u[static_cast<std::size_t>(mid)])
            hi = mid;
        else
            lo = mid;
        mid = (lo + hi) / 2;
    }
    return mid;
}

void insertKnot(HomSpline& s, double t) {
    int p = s.p;
    int k = findSpan(s, t);
    int mult = 0;
    for (double v : s.u)
        if (v == t)
            ++mult;
    std::vector<Hom> nq(s.q.size() + 1);
    for (int i = 0; i <= k - p; ++i)
        nq[static_cast<std::size_t>(i)] = s.q[static_cast<std::size_t>(i)];
    for (int i = k - mult; i < static_cast<int>(s.q.size()); ++i)
        nq[static_cast<std::size_t>(i + 1)] = s.q[static_cast<std::size_t>(i)];
    for (int i = k - p + 1; i <= k - mult; ++i) {
        double den = s.u[static_cast<std::size_t>(i + p)] - s.u[static_cast<std::size_t>(i)];
        double alpha = den > 0 ? (t - s.u[static_cast<std::size_t>(i)]) / den : 0.0;
        const Hom& a = s.q[static_cast<std::size_t>(i)];
        const Hom& b = s.q[static_cast<std::size_t>(i - 1)];
        nq[static_cast<std::size_t>(i)] = {alpha * a.x + (1 - alpha) * b.x,
                                           alpha * a.y + (1 - alpha) * b.y,
                                           alpha * a.w + (1 - alpha) * b.w};
    }
    s.u.insert(s.u.begin() + k + 1, t);
    s.q = std::move(nq);
}

// Brings the multiplicity of t up to p, then returns the index of the last knot equal to t.
int splitPoint(HomSpline& s, double t) {
    int mult = static_cast<int>(std::count(s.u.begin(), s.u.end(), t));
    for (; mult < s.p; ++mult)
        insertKnot(s, t);
    int k = -1;
    for (std::size_t i = 0; i < s.u.size(); ++i)
        if (s.u[i] == t)
            k = static_cast<int>(i);
    return k;
}

}  // namespace

Result<SplineData> splineSubcurve(const SplineData& data, double ta, double tb) {
    auto curveR = SplineCurve::make(data);
    if (!curveR)
        return curveR.error();
    const SplineCurve& c = *curveR.value();
    if (!(tb > ta))
        return makeError(ErrorCode::InvalidArgument, "empty spline sub-range");
    ta = std::max(ta, c.tMin());
    tb = std::min(tb, c.tMax());
    if (!(tb > ta))
        return makeError(ErrorCode::InvalidArgument, "empty spline sub-range");

    HomSpline h;
    h.p = c.degree();
    h.u = c.knots();
    SplineData base = data;
    if (base.control.empty()) {
        auto r =
            interpolateSpline(base.fit, InterpolateOptions{base.degree, base.closed, {}, {}, true});
        if (!r)
            return r.error();
        base = std::move(r).value();
    }
    for (std::size_t i = 0; i < base.control.size(); ++i) {
        double w = base.weights.empty() ? 1.0 : base.weights[i];
        h.q.push_back({base.control[i].x * w, base.control[i].y * w, w});
    }
    int p = h.p;
    if (ta > c.tMin()) {
        int k = splitPoint(h, ta);
        // Right part: knots [ta] + U[k-p+1 ..], controls Q[k-p ..].
        HomSpline r;
        r.p = p;
        r.u.push_back(ta);
        r.u.insert(r.u.end(), h.u.begin() + (k - p + 1), h.u.end());
        r.q.assign(h.q.begin() + (k - p), h.q.end());
        h = std::move(r);
    }
    if (tb < c.tMax()) {
        int k = splitPoint(h, tb);
        HomSpline l;
        l.p = p;
        l.u.assign(h.u.begin(), h.u.begin() + (k + 1));
        l.u.push_back(tb);
        l.q.assign(h.q.begin(), h.q.begin() + (k - p + 1));
        h = std::move(l);
    }
    SplineData out;
    out.degree = p;
    out.knots = std::move(h.u);
    bool rational = !base.weights.empty();
    for (const Hom& q : h.q) {
        out.control.push_back({q.x / q.w, q.y / q.w});
        if (rational)
            out.weights.push_back(q.w);
    }
    out.closed = false;
    out.periodic = false;
    return out;
}

}  // namespace tbcad::geom

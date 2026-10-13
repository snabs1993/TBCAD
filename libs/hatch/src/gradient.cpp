#include <algorithm>
#include <cmath>

#include "tbcad/hatch/hatch.h"

namespace tbcad::hatch {

namespace {

using Ring = std::vector<Vec2>;

// Sutherland-Hodgman: keeps the part of the closed polygon with dot(p, u) >= bound (keepAbove) or
// <= bound. Concave rings may leave zero-width bridges along the clip line; under the even-odd
// rule they do not change the filled area.
Ring clip(const Ring& in, Vec2 u, double bound, bool keepAbove) {
    Ring out;
    const std::size_t n = in.size();
    if (n < 3)
        return out;
    auto side = [&](const Vec2& p) {
        double d = dot(p, u) - bound;
        return keepAbove ? d : -d;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2& a = in[i];
        const Vec2& b = in[(i + 1) % n];
        double sa = side(a), sb = side(b);
        if (sa >= 0)
            out.push_back(a);
        if ((sa >= 0) != (sb >= 0)) {
            double t = sa / (sa - sb);
            out.push_back(a + (b - a) * t);
        }
    }
    return out.size() >= 3 ? out : Ring{};
}

}  // namespace

std::vector<GradientBand> gradientBands(const HatchData& hatch, double tolerance,
                                        std::size_t bands) {
    std::vector<GradientBand> out;
    if (!hatch.gradient || bands == 0)
        return out;
    const GradientData& g = *hatch.gradient;
    std::vector<Ring> rings = loopRings(hatch, tolerance);
    Vec2 u{std::cos(g.angle), std::sin(g.angle)};
    double lo = 1e300, hi = -1e300;
    for (const Ring& r : rings)
        for (const Vec2& p : r) {
            double d = dot(p, u);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
    if (!(hi > lo))
        return out;
    const double w = (hi - lo) / static_cast<double>(bands);
    for (std::size_t i = 0; i < bands; ++i) {
        double a = lo + w * static_cast<double>(i);
        // Each band reaches a quarter band into the next one (which is painted over it), so that
        // no hairline gaps show between bands.
        double b = lo + w * (static_cast<double>(i) + (i + 1 == bands ? 1.0 : 1.25));
        GradientBand band;
        double s = (static_cast<double>(i) + 0.5) / static_cast<double>(bands);
        band.t = g.kind == GradientKind::Cylinder ? 1.0 - std::abs(2.0 * s - 1.0) : s;
        for (const Ring& r : rings) {
            Ring c = clip(r, u, i == 0 ? lo - 1 : a, true);
            c = clip(c, u, i + 1 == bands ? hi + 1 : b, false);
            if (c.size() >= 3)
                band.rings.push_back(std::move(c));
        }
        if (!band.rings.empty())
            out.push_back(std::move(band));
    }
    return out;
}

}  // namespace tbcad::hatch

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>

#include "tbcad/core/color.h"
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

// The part of the ring inside the disc (centre c, radius r): clipped by the sides of a regular
// polygon that holds the circle.
Ring clipDisc(Ring ring, Vec2 c, double r) {
    constexpr int kSides = 48;
    const double apothem = r;  // the polygon's sides touch the circle
    for (int k = 0; k < kSides && ring.size() >= 3; ++k) {
        const double a = 2 * kPi * k / kSides;
        const Vec2 n{std::cos(a), std::sin(a)};
        ring = clip(ring, n, dot(c, n) + apothem, false);
    }
    return ring;
}

bool inverted(GradientKind k) {
    return k == GradientKind::InvCylinder || k == GradientKind::InvSpherical ||
           k == GradientKind::InvHemispherical || k == GradientKind::InvCurved;
}

bool radial(GradientKind k) {
    return k == GradientKind::Spherical || k == GradientKind::InvSpherical ||
           k == GradientKind::Hemispherical || k == GradientKind::InvHemispherical;
}

// Colour parameter at fraction s (0..1) along the gradient direction, for the strip kinds.
// `c` is the middle of the gradient (0.5 centred, less when shifted).
double stripT(GradientKind k, double s, double c) {
    switch (k) {
        case GradientKind::Cylinder:
        case GradientKind::InvCylinder:
            return 1.0 - std::abs(s - c) / std::max(c, 1.0 - c);
        case GradientKind::Curved:
        case GradientKind::InvCurved: {
            // A cylinder whose light side is bent towards the start.
            const double d = std::abs(s - c) / std::max(c, 1.0 - c);
            return s < c ? 1.0 - d * d : 1.0 - d;
        }
        default:
            return s < c ? 0.5 * s / c : 0.5 + 0.5 * (s - c) / (1.0 - c);
    }
}

}  // namespace

const char* gradientName(GradientKind kind) {
    switch (kind) {
        case GradientKind::Cylinder:
            return "CYLINDER";
        case GradientKind::InvCylinder:
            return "INVCYLINDER";
        case GradientKind::Spherical:
            return "SPHERICAL";
        case GradientKind::InvSpherical:
            return "INVSPHERICAL";
        case GradientKind::Hemispherical:
            return "HEMISPHERICAL";
        case GradientKind::InvHemispherical:
            return "INVHEMISPHERICAL";
        case GradientKind::Curved:
            return "CURVED";
        case GradientKind::InvCurved:
            return "INVCURVED";
        default:
            return "LINEAR";
    }
}

std::optional<GradientKind> gradientKindFromName(std::string_view name) {
    std::string up;
    for (char ch : name)
        if (ch != ' ' && ch != '_')
            up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    for (std::uint8_t k = 1; k <= kGradientKindLast; ++k)
        if (up == gradientName(static_cast<GradientKind>(k)))
            return static_cast<GradientKind>(k);
    return std::nullopt;
}

Color tintedColor(Color c, double tint) {
    Rgba base{c.r, c.g, c.b, 255};
    if (c.kind == ColorKind::Index)
        base = paletteColor(c.indexValue());
    tint = std::clamp(std::isfinite(tint) ? tint : 1.0, 0.0, 1.0);
    // 0.5 keeps the colour; below goes towards black, above towards white.
    auto mix = [&](std::uint8_t v) {
        const double c = v;
        const double r = tint < 0.5 ? c * (tint * 2) : c + (255.0 - c) * (tint - 0.5) * 2;
        return static_cast<std::uint8_t>(std::lround(std::clamp(r, 0.0, 255.0)));
    };
    return Color::rgb(mix(base.r), mix(base.g), mix(base.b));
}

std::vector<GradientBand> gradientBands(const HatchData& hatch, double tolerance,
                                        std::size_t bands) {
    std::vector<GradientBand> out;
    if (!hatch.gradient || bands == 0)
        return out;
    const GradientData& g = *hatch.gradient;
    std::vector<Ring> rings = loopRings(hatch, tolerance);
    const Vec2 u{std::cos(g.angle), std::sin(g.angle)};
    const Vec2 v{-u.y, u.x};
    double lo = 1e300, hi = -1e300, vlo = 1e300, vhi = -1e300;
    for (const Ring& r : rings)
        for (const Vec2& p : r) {
            lo = std::min(lo, dot(p, u));
            hi = std::max(hi, dot(p, u));
            vlo = std::min(vlo, dot(p, v));
            vhi = std::max(vhi, dot(p, v));
        }
    if (!(hi > lo))
        return out;
    const double shift = std::clamp(std::isfinite(g.shift) ? g.shift : 0.0, 0.0, 1.0);
    const double centre = 0.5 - 0.45 * shift;  // never at the very edge
    const bool inv = inverted(g.kind);

    if (radial(g.kind)) {
        // Discs around the centre, outermost (colour 1) first; colour 2 at the centre.
        const bool hemi =
            g.kind == GradientKind::Hemispherical || g.kind == GradientKind::InvHemispherical;
        const double cu = lo + (hi - lo) * centre;
        const double cv = hemi ? vlo : (vlo + vhi) / 2;
        const Vec2 c = u * cu + v * cv;
        double reach = 0;
        for (const Ring& r : rings)
            for (const Vec2& p : r)
                reach = std::max(reach, length(p - c));
        if (!(reach > 0))
            return out;
        const std::size_t n = std::max<std::size_t>(2, bands / 2);
        for (std::size_t i = 0; i < n; ++i) {
            const double rad = reach * (1.0 - static_cast<double>(i) / static_cast<double>(n));
            GradientBand band;
            const double mid = (static_cast<double>(i) + 0.5) / static_cast<double>(n);
            band.t = inv ? 1.0 - mid : mid;
            for (const Ring& r : rings) {
                // The first disc holds everything: no clipping needed.
                Ring cr = i == 0 ? r : clipDisc(r, c, rad);
                if (cr.size() >= 3)
                    band.rings.push_back(std::move(cr));
            }
            if (!band.rings.empty())
                out.push_back(std::move(band));
        }
        return out;
    }

    const double w = (hi - lo) / static_cast<double>(bands);
    for (std::size_t i = 0; i < bands; ++i) {
        double a = lo + w * static_cast<double>(i);
        // Each band reaches a quarter band into the next one (which is painted over it), so that
        // no hairline gaps show between bands.
        double b = lo + w * (static_cast<double>(i) + (i + 1 == bands ? 1.0 : 1.25));
        GradientBand band;
        const double s = (static_cast<double>(i) + 0.5) / static_cast<double>(bands);
        const double t = std::clamp(stripT(g.kind, s, centre), 0.0, 1.0);
        band.t = inv ? 1.0 - t : t;
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

#include "tbcad/drafting/grid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/ucs.h"

namespace tbcad::drafting {

double effectiveGridSpacing(double spacingMm, double pixelsPerMm, double minPixels) {
    if (!(spacingMm > 0) || !(pixelsPerMm > 0))
        return 0;
    double s = spacingMm;
    int guard = 0;
    while (s * pixelsPerMm < minPixels && ++guard < 64)
        s *= (s * pixelsPerMm * 5 >= minPixels) ? 5.0 : 10.0;
    return s;
}

GridLevels adaptiveGridLevels(double spacingMm, double pixelsPerMm, double minPixels,
                              double fullPixels) {
    GridLevels out;
    if (!(spacingMm > 0) || !(pixelsPerMm > 0))
        return out;
    double s = spacingMm;
    int guard = 0;
    while (s * pixelsPerMm < minPixels && ++guard < 64)
        s *= 10.0;
    out.fineMm = s;
    out.coarseMm = s * 10.0;
    const double span = std::max(fullPixels - minPixels, 1e-9);
    const double t = std::clamp((s * pixelsPerMm - minPixels) / span, 0.0, 1.0);
    out.fineAlpha = t * t * (3.0 - 2.0 * t);  // smoothstep
    return out;
}

void buildGrid(render::Overlay& overlay, const render::Camera2D& camera, const GridStyle& style) {
    const GridLevels lv = adaptiveGridLevels(style.spacingMm, camera.pixelsPerMm);
    if (lv.fineMm <= 0)
        return;
    const BBox2 vis = camera.visibleWorld();
    if (vis.empty())
        return;

    // The visible area in grid coordinates (a rotated grid sees a rotated, larger rectangle).
    Ucs frame;
    frame.origin = style.origin;
    frame.angle = style.angle;
    BBox2 area;
    for (Vec2 c : {vis.min, Vec2{vis.max.x, vis.min.y}, vis.max, Vec2{vis.min.x, vis.max.y}})
        area.expand(frame.fromWorld(c));

    const Rgba minor = style.dark ? Rgba{70, 76, 86, 255} : Rgba{190, 196, 206, 255};
    const Rgba major = style.dark ? Rgba{96, 104, 116, 255} : Rgba{160, 168, 180, 255};

    if (style.isoPlane != 0) {
        // Isometric lattice: lines along both axes of the plane, spaced `s` along the other axis.
        const auto [a1, a2] = isoAxesDeg(static_cast<IsoPlane>(style.isoPlane));
        const Vec2 b1{std::cos(a1 * kPi / 180.0), std::sin(a1 * kPi / 180.0)};
        const Vec2 b2{std::cos(a2 * kPi / 180.0), std::sin(a2 * kPi / 180.0)};
        const double det = b1.x * b2.y - b1.y * b2.x;
        auto coords = [&](Vec2 p) {  // p = i b1 + j b2
            return Vec2{(p.x * b2.y - p.y * b2.x) / det, (b1.x * p.y - b1.y * p.x) / det};
        };
        BBox2 ij;
        for (Vec2 c :
             {area.min, Vec2{area.max.x, area.min.y}, area.max, Vec2{area.min.x, area.max.y}})
            ij.expand(coords(c));
        auto level = [&](double s, Rgba color, double alpha) {
            if (alpha <= 0.01)
                return;
            const double ni = ij.width() / s, nj = ij.height() / s;
            if (ni > 200 || nj > 200)
                return;
            render::OverlayStyle st;
            color.a = static_cast<std::uint8_t>(std::lround(color.a * alpha));
            st.color = color;
            st.widthPx = 1.0;
            const double i0 = std::floor(ij.min.x / s) * s, i1 = ij.max.x + s;
            const double j0 = std::floor(ij.min.y / s) * s, j1 = ij.max.y + s;
            for (double j = j0; j <= j1; j += s)
                overlay.line(render::Space::World, frame.toWorld(b1 * i0 + b2 * j),
                             frame.toWorld(b1 * i1 + b2 * j), st);
            for (double i = i0; i <= i1; i += s)
                overlay.line(render::Space::World, frame.toWorld(b1 * i + b2 * j0),
                             frame.toWorld(b1 * i + b2 * j1), st);
        };
        level(lv.fineMm, minor, lv.fineAlpha);
        level(lv.coarseMm, major, 1.0);
        return;
    }
    auto drawLevel = [&](double s, Rgba color, double alpha, bool skipCoarse) {
        if (alpha <= 0.01)
            return;
        const double nx = area.width() / s, ny = area.height() / s;
        if (nx > 200 || ny > 200)
            return;  // pathological aspect ratio; the spacing rule already keeps normal views small
        render::OverlayStyle st;
        color.a = static_cast<std::uint8_t>(std::lround(color.a * alpha));
        st.color = color;
        st.widthPx = 1.0;
        if (!style.lines) {
            // Dots: a dashed line whose dashes are one pixel wide and one grid step apart.
            st.dashPx = 1.5;
            st.gapPx = s * camera.pixelsPerMm - 1.5;
        }
        auto onCoarse = [&](double v) {
            const double k = v / (s * 10.0);
            return std::abs(k - std::round(k)) < 1e-6;
        };
        const double x0 = std::floor(area.min.x / s) * s, y0 = std::floor(area.min.y / s) * s;
        const double x1 = area.max.x + s, y1 = area.max.y + s;
        for (double y = y0; y <= y1; y += s) {
            if (skipCoarse && onCoarse(y))
                continue;
            overlay.line(render::Space::World, frame.toWorld({x0, y}), frame.toWorld({x1, y}), st);
        }
        if (style.lines) {
            for (double x = x0; x <= x1; x += s) {
                if (skipCoarse && onCoarse(x))
                    continue;
                overlay.line(render::Space::World, frame.toWorld({x, y0}), frame.toWorld({x, y1}),
                             st);
            }
        }
    };
    // Dots sit on the crossings of horizontal lines only; the coarse level skips none of them.
    drawLevel(lv.fineMm, minor, lv.fineAlpha, style.lines);
    drawLevel(lv.coarseMm, major, 1.0, false);
}

}  // namespace tbcad::drafting

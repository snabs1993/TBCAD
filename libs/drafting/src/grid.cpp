#include "tbcad/drafting/grid.h"

#include <cmath>

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

void buildGrid(render::Overlay& overlay, const render::Camera2D& camera, const GridStyle& style) {
    const double s = effectiveGridSpacing(style.spacingMm, camera.pixelsPerMm);
    if (s <= 0)
        return;
    const BBox2 vis = camera.visibleWorld();
    if (vis.empty())
        return;
    const double spacingPx = s * camera.pixelsPerMm;
    const double nx = vis.width() / s, ny = vis.height() / s;
    if (nx > 200 || ny > 200)
        return;  // pathological aspect ratio; the spacing rule already keeps normal views small

    const Rgba minor = style.dark ? Rgba{70, 76, 86, 255} : Rgba{190, 196, 206, 255};
    const Rgba major = style.dark ? Rgba{96, 104, 116, 255} : Rgba{160, 168, 180, 255};
    auto styleFor = [&](bool isMajor) {
        render::OverlayStyle st;
        st.color = isMajor ? major : minor;
        st.widthPx = 1.0;
        if (!style.lines) {
            // Dots: a dashed line whose dashes are one pixel wide and one grid step apart.
            st.dashPx = 1.5;
            st.gapPx = spacingPx - 1.5;
        }
        return st;
    };

    const double x0 = std::floor(vis.min.x / s) * s, y0 = std::floor(vis.min.y / s) * s;
    // Horizontal lines (and, for line style, vertical ones).
    for (double y = y0; y <= vis.max.y + s; y += s) {
        const auto idx = std::llround(y / s);
        const bool isMajor = idx % 5 == 0;
        overlay.line(render::Space::World, {x0, y}, {vis.max.x + s, y}, styleFor(isMajor));
    }
    if (style.lines) {
        for (double x = x0; x <= vis.max.x + s; x += s) {
            const auto idx = std::llround(x / s);
            overlay.line(render::Space::World, {x, y0}, {x, vis.max.y + s}, styleFor(idx % 5 == 0));
        }
    }
}

}  // namespace tbcad::drafting

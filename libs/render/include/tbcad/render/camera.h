#pragma once

#include "tbcad/core/math.h"

namespace tbcad::render {

// Screen space: pixel (0,0) is the top-left corner of the viewport, x right, y down, continuous
// coordinates (a pixel centre is at +0.5). World space: x right, y up, millimetres.
struct Camera2D {
    static constexpr double kMinPixelsPerMm = 1e-7;
    static constexpr double kMaxPixelsPerMm = 1e7;

    Vec2 center{};  // world point shown at the middle of the viewport
    double pixelsPerMm = 1.0;
    int viewportW = 1;
    int viewportH = 1;

    Vec2 screenToWorld(Vec2 px) const;
    Vec2 worldToScreen(Vec2 world) const;
    // Zooms by `factor` (>1 = in) keeping the world point under `screenPt` fixed.
    void zoomAt(Vec2 screenPt, double factor);
    // Moves the view so that content under the cursor follows a drag of (dx, dy) pixels.
    void panPixels(double dx, double dy);
    // Centres `box` and picks the zoom so it fills the viewport with `margin` (fraction of the
    // viewport) around it. An empty or degenerate box keeps the current zoom and centres on it.
    void fit(const BBox2& box, double margin = 0.05);
    // World rectangle covered by the viewport.
    BBox2 visibleWorld() const;
    // Size of one screen pixel in millimetres.
    double pixelSizeMm() const { return 1.0 / pixelsPerMm; }

    friend bool operator==(const Camera2D&, const Camera2D&) = default;
};

}  // namespace tbcad::render

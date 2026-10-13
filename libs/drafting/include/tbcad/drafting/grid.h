#pragma once

#include "tbcad/render/camera.h"
#include "tbcad/render/overlay.h"

namespace tbcad::drafting {

// Smallest spacing >= `spacingMm` (times 5 or 10 as often as needed) at which neighbouring grid
// lines are at least `minPixels` apart on screen. 0 when spacingMm is not positive.
double effectiveGridSpacing(double spacingMm, double pixelsPerMm, double minPixels = 8.0);

struct GridStyle {
    double spacingMm = 10;
    bool lines = false;  // false = dots
    bool dark = true;    // picks the colours for the dark or the light theme
};

// Adds the grid of the visible area to the overlay: dotted or continuous lines, every fifth
// line of the effective spacing a little stronger. Draws nothing when the area would need more
// than ~200 lines in one direction, or the spacing is not positive.
void buildGrid(render::Overlay& overlay, const render::Camera2D& camera, const GridStyle& style);

}  // namespace tbcad::drafting

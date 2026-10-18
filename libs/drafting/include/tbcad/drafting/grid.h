#pragma once

#include "tbcad/core/core.h"
#include "tbcad/render/camera.h"
#include "tbcad/render/overlay.h"

namespace tbcad::drafting {

// Smallest spacing >= `spacingMm` (times 5 or 10 as often as needed) at which neighbouring grid
// lines are at least `minPixels` apart on screen. 0 when spacingMm is not positive.
double effectiveGridSpacing(double spacingMm, double pixelsPerMm, double minPixels = 8.0);

// The two grid levels drawn at a zoom: `fine` lines (spacing `fineMm`, drawn with opacity
// `fineAlpha` 0..1) and `coarse` lines ten times as far apart, always fully visible. The fine
// level fades in as the view zooms in and fades out as it zooms out, so the grid never jumps.
// fineMm is spacingMm times a power of ten; 0 when the inputs are not positive.
struct GridLevels {
    double fineMm = 0;
    double fineAlpha = 0;
    double coarseMm = 0;
};
GridLevels adaptiveGridLevels(double spacingMm, double pixelsPerMm, double minPixels = 8.0,
                              double fullPixels = 24.0);

struct GridStyle {
    Vec2 origin{};     // grid origin (the UCS origin)
    double angle = 0;  // grid rotation in radians (the UCS rotation)
    double spacingMm = 10;
    bool lines = false;  // false = dots
    int isoPlane = 0;    // IsoPlane value: 0 off; else the lattice of that isometric plane
    bool dark = true;    // picks the colours for the dark or the light theme
};

// Adds the grid of the visible area to the overlay: dotted or continuous lines, every fifth
// line of the effective spacing a little stronger. Draws nothing when the area would need more
// than ~200 lines in one direction, or the spacing is not positive.
void buildGrid(render::Overlay& overlay, const render::Camera2D& camera, const GridStyle& style);

}  // namespace tbcad::drafting

#pragma once

#include <optional>
#include <string_view>

#include "tbcad/drafting/settings.h"
#include "tbcad/render/camera.h"
#include "tbcad/render/overlay.h"

namespace tbcad::drafting {

// Draws the snap marker for `mode` centred on the world point `p`: square endpoint, triangle
// midpoint, circle center, X intersection, right angle perpendicular, circle with a line
// tangent, hourglass nearest, diamond quadrant, circle with a cross node. `sizePx` is the full
// width (12 px by default, see ux-ui.md). The colour is the theme's `snap` token unless `color`.
void drawSnapMarker(render::Overlay& overlay, const render::Camera2D& camera, Vec2 p, SnapMode mode,
                    bool darkTheme = true, double sizePx = 12,
                    std::optional<Rgba> color = std::nullopt);

// Same, picking the mode from a FilterFeedback::marker name. False when the name is unknown.
bool drawSnapMarker(render::Overlay& overlay, const render::Camera2D& camera, Vec2 p,
                    std::string_view markerName, bool darkTheme = true, double sizePx = 12,
                    std::optional<Rgba> color = std::nullopt);

}  // namespace tbcad::drafting

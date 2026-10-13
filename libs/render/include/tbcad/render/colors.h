#pragma once

#include "tbcad/core/color.h"

namespace tbcad::render {

// True when the background is closer to white than to black.
bool isLightBackground(Rgba bg);

// Colours that would vanish against the background are swapped, as CAD programs do: white
// (palette index 7) is drawn black on a light background, black is drawn white on a dark one.
Rgba adaptToBackground(Rgba c, Rgba background);

}  // namespace tbcad::render

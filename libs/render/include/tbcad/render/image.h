#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "tbcad/core/image.h"
#include "tbcad/core/math.h"

namespace tbcad::render {

using tbcad::ImageData;

// One image to draw behind the drawing (an underlay). The Renderer keeps a texture per `key`, so
// the same key must always mean the same pixels; give a new key when the pixels change.
struct ImageDraw {
    std::uint64_t key = 0;
    std::shared_ptr<const ImageData> data{};
    // World positions (mm) of the visible part's bottom-left, bottom-right, top-right and
    // top-left corners.
    std::array<Vec2, 4> corners{};
    // The visible part of the image in texture coordinates: u from left to right, v from the top
    // row (0) down to the bottom row (1). So bottom-left is (uMin, vMax).
    double uMin = 0, uMax = 1, vMin = 0, vMax = 1;
    // 0 = invisible, 1 = opaque. The image is blended with what is already drawn (the
    // background), which is how a faded underlay stays out of the way.
    float opacity = 1;
};

}  // namespace tbcad::render

#pragma once

#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::blocks {

// Corner grips of a raster picture (an Insert of the block IMAGEATTACH makes: a frame in pixel
// units) or a PDF underlay (the box of what its page drew). Dragging a corner scales the picture about the opposite corner.
struct PictureHandle {
    int corner = 0;  // 0..3: frame vertices in order, from the bottom-left one
    Vec2 world{};
};
std::vector<PictureHandle> pictureHandles(const Document& doc, const Entity& insert);
// The Insert scaled (uniformly, keeping the opposite corner fixed) so the dragged corner comes
// as close as it can to `world`, measured along the frame diagonal.
Result<Entity> pictureDrag(const Document& doc, const Entity& insert, int corner, Vec2 world);

}  // namespace tbcad::blocks

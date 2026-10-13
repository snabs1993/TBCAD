#pragma once

namespace tbcad::render::gl {

// Positions arrive as hi/lo float pairs relative to a tile origin; the tile offset uniform
// (tile origin - camera centre, computed in double on the CPU) is split the same way, so the sum
// below is exact where it matters: near the camera.
extern const char* const kStrokeThinVs;
extern const char* const kStrokeThickVs;
extern const char* const kStrokeFs;
extern const char* const kFillVs;
extern const char* const kFillFs;
extern const char* const kOverlayVs;
extern const char* const kOverlayFs;
extern const char* const kImageVs;
extern const char* const kImageFs;

}  // namespace tbcad::render::gl

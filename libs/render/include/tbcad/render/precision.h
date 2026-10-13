#pragma once

#include "tbcad/core/math.h"

namespace tbcad::render {

// A double split into two floats: hi + lo reproduces the value to about 2^-48 relative.
struct SplitFloat {
    float hi = 0;
    float lo = 0;
};

inline SplitFloat splitDouble(double v) {
    SplitFloat s;
    s.hi = static_cast<float>(v);
    s.lo = static_cast<float>(v - static_cast<double>(s.hi));
    return s;
}

// One vertex as it is stored in GPU buffers. Position is relative to the origin of its tile,
// kept as a hi/lo float pair per axis; `d` is the distance along the polyline (mm) modulo the
// dash period (strokes only).
struct GpuVertex {
    float hx = 0, hy = 0;  // hi parts
    float lx = 0, ly = 0;  // lo parts
    float d = 0;
};
static_assert(sizeof(GpuVertex) == 20);

inline GpuVertex packVertex(const Vec2& world, const Vec2& tileOrigin, double dashDistance = 0) {
    SplitFloat x = splitDouble(world.x - tileOrigin.x);
    SplitFloat y = splitDouble(world.y - tileOrigin.y);
    return {x.hi, y.hi, x.lo, y.lo, static_cast<float>(dashDistance)};
}

// (tile origin - camera centre), computed in double and split like a vertex. This is what the
// shaders receive as per-tile uniforms.
struct TileOffset {
    float hiX = 0, hiY = 0, loX = 0, loY = 0;
};

inline TileOffset tileOffset(const Vec2& tileOrigin, const Vec2& cameraCenter) {
    SplitFloat x = splitDouble(tileOrigin.x - cameraCenter.x);
    SplitFloat y = splitDouble(tileOrigin.y - cameraCenter.y);
    return {x.hi, y.hi, x.lo, y.lo};
}

struct Vec2f {
    float x = 0, y = 0;
};

// CPU mirror of the vertex shader: position relative to the camera centre in millimetres, using
// only float arithmetic.
inline Vec2f cameraRelative(const GpuVertex& v, const TileOffset& o) {
    return {(v.hx + o.hiX) + (v.lx + o.loX), (v.hy + o.hiY) + (v.ly + o.loY)};
}

}  // namespace tbcad::render

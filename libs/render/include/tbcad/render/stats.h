#pragma once

#include <cstdint>

namespace tbcad::render {

struct RenderStats {
    std::uint32_t drawCalls = 0;
    std::uint64_t vertices = 0;  // vertices submitted (instanced segments count 2 each)
    std::uint32_t tilesDrawn = 0;
    std::uint64_t uploadedBytes = 0;  // buffer data uploaded this frame
    double cpuMs = 0;                 // CPU time between beginFrame and endFrame
};

}  // namespace tbcad::render

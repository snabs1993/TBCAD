#pragma once

#include <cstdint>
#include <vector>

namespace tbcad {

// Pixels of a raster image, 8 bits per channel, straight (not premultiplied) alpha, first row on
// top. Used by image underlays (libs/underlay) and drawn by the renderer.
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba{};
};

}  // namespace tbcad

// A small TIFF reader for scanned drawings (stb_image does not read TIFF). Baseline plus the
// usual compressions: none, PackBits, LZW, Deflate and CCITT Group 4; 1/2/4/8/16-bit grey,
// palette, RGB(A) and CMYK, in strips, with the horizontal predictor. The first page only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "tbcad/core/core.h"

namespace tbcad::underlay {

bool looksLikeTiff(const std::uint8_t* data, std::size_t size);
Result<std::shared_ptr<const ImageData>> decodeTiff(const std::uint8_t* data, std::size_t size);

}  // namespace tbcad::underlay

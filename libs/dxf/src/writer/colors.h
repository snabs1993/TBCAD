// Internal helpers of the DXF writer: colours, line weights and text escaping.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "tbcad/core/color.h"

namespace tbcad::dxf::detail {

// A colour as DXF stores it: an ACI number for group 62 and, when the ACI is only an
// approximation, the exact colour for group 420.
struct AciChoice {
    int aci = 7;
    std::optional<std::uint32_t> rgb{};
};

// Palette index -> the ACI the reader maps back to the same index (exact round trip).
AciChoice aciForIndex(int index);
// Any RGB colour -> nearest ACI plus the exact colour.
AciChoice aciForRgba(Rgba c);
// Index or Rgb colour (ByLayer/ByBlock are handled by the caller).
AciChoice aciFor(Color c);

// Nearest line weight DXF accepts, in hundredths of a millimetre.
int validLineweight(int hundredths);

// Text for a group value. UTF-8 mode passes valid UTF-8 through; otherwise every non-ASCII
// character becomes \U+XXXX (surrogate pairs above U+FFFF). Control characters become spaces.
std::string encodeText(std::string_view utf8, bool utf8Mode);

// "%.17g"-style shortest form that always looks like a real number.
std::string formatNumber(double v);

}  // namespace tbcad::dxf::detail

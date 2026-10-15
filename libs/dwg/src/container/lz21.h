// Decompression of the LZ77 variant used by the sections of R2007 (AC1021) files
// (ODA spec section 5.10).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tbcad::dwg::detail {

// Expands `in` into exactly `expected` bytes. Returns false when the stream is damaged or ends
// early; `why` (optional) then says where. `out` is replaced.
bool lz21Decompress(std::span<const std::uint8_t> in, std::size_t expected,
                    std::vector<std::uint8_t>& out, std::string* why = nullptr);

}  // namespace tbcad::dwg::detail

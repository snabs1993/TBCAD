// Decompression of the LZ77 variant used by R2004 and later sections (ODA spec section 4.7).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tbcad::dwg::detail {

// Expands `in` into exactly `expected` bytes. Returns false when the stream is damaged, ends
// early or would produce more than `expected` bytes. `out` is replaced.
bool lz77Decompress(std::span<const std::uint8_t> in, std::size_t expected,
                    std::vector<std::uint8_t>& out);

}  // namespace tbcad::dwg::detail

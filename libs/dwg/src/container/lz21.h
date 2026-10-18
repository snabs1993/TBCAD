// Decompression of the LZ77 variant used by the sections of R2007 (AC1021) files
// (ODA spec section 5.10).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tbcad::dwg::detail {

// Block sizes of the remainder of a literal run, in stream order, by the run's length modulo 32
// (see lz21.cpp). Mutable only so the diagnostic cut search can try other arrangements.
std::array<std::vector<std::uint8_t>, 32>& lz21Cuts();

// Expands `in` into exactly `expected` bytes. Returns false when the stream is damaged or ends
// early; `why` (optional) then says where. `consumed` (optional) receives the number of input
// bytes used on success. `out` is replaced.
bool lz21Decompress(std::span<const std::uint8_t> in, std::size_t expected,
                    std::vector<std::uint8_t>& out, std::string* why = nullptr,
                    std::size_t* consumed = nullptr);

}  // namespace tbcad::dwg::detail

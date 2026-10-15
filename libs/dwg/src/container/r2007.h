// The R2007 (AC1021) file layout (ODA spec section 5): a Reed-Solomon coded file header, a page
// map and a section map stored as "system pages", and data pages that are Reed-Solomon coded
// and compressed with their own LZ77 variant. Error correction is not applied: the data bytes
// are taken out of the code words as they are.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "container/r2004.h"
#include "tbcad/core/core.h"

namespace tbcad::dwg::detail {

// Takes the data bytes of `blocks` code words of `n` bytes (`k` of them data) from `coded`.
// `interleaved` code words store byte j of block b at position j * blocks + b; otherwise all data
// blocks come first and the parity blocks follow. Returns blocks * k bytes.
std::vector<std::uint8_t> rsDataBytes(std::span<const std::uint8_t> coded, std::size_t blocks,
                                      std::size_t k, std::size_t n, bool interleaved);

Result<SectionSet> readSections2007(std::span<const std::uint8_t> file,
                                    std::vector<std::string>& notes,
                                    std::vector<std::string>* trace = nullptr);

}  // namespace tbcad::dwg::detail

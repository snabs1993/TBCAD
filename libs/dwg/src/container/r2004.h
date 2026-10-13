// The R2004 and later file layout (ODA spec section 4, used by R2004, R2010, R2013 and R2018):
// an encrypted file header, a page map and a section map, and sections made of compressed
// pages. `readSections` returns every section as one contiguous byte vector.
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::dwg::detail {

struct SectionSet {
    int codepage = 0;
    int maintenance = 0;  // byte 0x0B of the file
    std::map<std::string, std::vector<std::uint8_t>> sections{};
};

// The checksum used by the page headers (ODA spec section 4.2).
std::uint32_t pageChecksum(std::uint32_t seed, std::span<const std::uint8_t> data);

// The 0x6C-byte pad that the file header data is XORed with (and that fills gaps).
std::vector<std::uint8_t> headerMask(std::size_t length);

Result<SectionSet> readSections(std::span<const std::uint8_t> file,
                                std::vector<std::string>& notes);

}  // namespace tbcad::dwg::detail

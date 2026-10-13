#include <array>

#include "bytes.h"

namespace tbcad::io::detail {

namespace {
constexpr std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kTable = makeTable();
}  // namespace

// CRC-32 (IEEE 802.3, the same polynomial as zlib and PNG).
std::uint32_t tbcadCrc32(std::span<const std::uint8_t> data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t b : data)
        c = kTable[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string fourccName(std::uint32_t v) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        auto c = static_cast<char>((v >> (8 * i)) & 0xFF);
        s += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return s;
}

}  // namespace tbcad::io::detail

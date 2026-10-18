#include <array>

#include "bytes.h"

namespace tbcad::io::detail {

namespace {
// Slice-by-8: eight table lookups handle eight bytes at once (about four times faster than one
// byte at a time). kTables[0] is the ordinary byte table.
constexpr std::array<std::array<std::uint32_t, 256>, 8> makeTables() {
    std::array<std::array<std::uint32_t, 256>, 8> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i)
        for (int s = 1; s < 8; ++s)
            t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    return t;
}
constexpr auto kTables = makeTables();
}  // namespace

// CRC-32 (IEEE 802.3, the same polynomial as zlib and PNG).
std::uint32_t tbcadCrc32(std::span<const std::uint8_t> data) {
    std::uint32_t c = 0xFFFFFFFFu;
    const std::uint8_t* p = data.data();
    std::size_t n = data.size();
    while (n >= 8) {
        // Bytes are combined little-endian, whatever the machine order is.
        const std::uint32_t lo =
            c ^ (static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
                 static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24);
        const std::uint32_t hi =
            static_cast<std::uint32_t>(p[4]) | static_cast<std::uint32_t>(p[5]) << 8 |
            static_cast<std::uint32_t>(p[6]) << 16 | static_cast<std::uint32_t>(p[7]) << 24;
        c = kTables[7][lo & 0xFF] ^ kTables[6][(lo >> 8) & 0xFF] ^ kTables[5][(lo >> 16) & 0xFF] ^
            kTables[4][lo >> 24] ^ kTables[3][hi & 0xFF] ^ kTables[2][(hi >> 8) & 0xFF] ^
            kTables[1][(hi >> 16) & 0xFF] ^ kTables[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n-- > 0)
        c = kTables[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
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

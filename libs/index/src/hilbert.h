#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace tbcad::index::detail {

// Position of a cell of the 2^32 x 2^32 grid along the Hilbert curve. Cells that are consecutive
// on the curve are neighbours, which is what makes a sorted run of boxes spatially compact.
// Table driven: 4 bits of each coordinate per step.
class HilbertTable {
public:
    HilbertTable() {
        for (unsigned s = 0; s < 4; ++s) {
            for (unsigned xn = 0; xn < 16; ++xn) {
                for (unsigned yn = 0; yn < 16; ++yn) {
                    unsigned state = s;
                    unsigned digits = 0;
                    for (int bit = 3; bit >= 0; --bit) {
                        unsigned x = (xn >> bit) & 1u;
                        unsigned y = (yn >> bit) & 1u;
                        digits = (digits << 2) | step(state, x, y);
                    }
                    table_[index(s, xn, yn)] = static_cast<std::uint16_t>((digits << 2) | state);
                }
            }
        }
    }

    std::uint64_t key(std::uint32_t x, std::uint32_t y) const {
        std::uint64_t d = 0;
        unsigned state = 0;
        for (int shift = 28; shift >= 0; shift -= 4) {
            std::uint16_t t = table_[index(state, (x >> shift) & 15u, (y >> shift) & 15u)];
            d = (d << 8) | (t >> 2);
            state = t & 3u;
        }
        return d;
    }

    // The same curve on a 2^16 x 2^16 grid: the top 32 bits of key() for coordinates shifted left
    // by 16, in half the steps. Enough resolution to order a drawing; sorting 32-bit keys is
    // much cheaper than 64-bit ones.
    std::uint32_t key16(std::uint32_t x, std::uint32_t y) const {
        std::uint32_t d = 0;
        unsigned state = 0;
        for (int shift = 12; shift >= 0; shift -= 4) {
            std::uint16_t t = table_[index(state, (x >> shift) & 15u, (y >> shift) & 15u)];
            d = (d << 8) | (t >> 2);
            state = t & 3u;
        }
        return d;
    }

    static const HilbertTable& instance() {
        static const HilbertTable t;
        return t;
    }

private:
    static unsigned index(unsigned s, unsigned xn, unsigned yn) {
        return (s << 8) | (xn << 4) | yn;
    }

    // State bit 0: x and y swapped, bit 1: both inverted. Same recurrence as the textbook
    // xy2d with its rotate step.
    static unsigned step(unsigned& state, unsigned x, unsigned y) {
        if (state & 1u)
            std::swap(x, y);
        if (state & 2u) {
            x ^= 1u;
            y ^= 1u;
        }
        unsigned digit = (3u * x) ^ y;
        if (y == 0) {
            if (x == 1)
                state ^= 2u;
            state ^= 1u;
        }
        return digit;
    }

    std::array<std::uint16_t, 4 * 256> table_{};
};

inline std::uint64_t hilbertKey(std::uint32_t x, std::uint32_t y) {
    return HilbertTable::instance().key(x, y);
}

}  // namespace tbcad::index::detail

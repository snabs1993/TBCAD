#include "container/lz21.h"

namespace tbcad::dwg::detail {

bool lz21Decompress(std::span<const std::uint8_t> in, std::size_t expected,
                    std::vector<std::uint8_t>& out, std::string* why) {
    out.assign(expected, 0);
    std::size_t src = 0, dst = 0;
    bool bad = false;
    auto fail = [&](const char* what) {
        if (why)
            *why = std::string(what) + " (source byte " + std::to_string(src) + " of " +
                   std::to_string(in.size()) + ", output byte " + std::to_string(dst) + " of " +
                   std::to_string(expected) + ")";
        return false;
    };
    auto next = [&]() -> std::uint32_t {
        if (src >= in.size()) {
            bad = true;
            return 0;
        }
        return in[src++];
    };
    if (in.empty())
        return expected == 0 ? true : fail("empty input");
    std::uint32_t op = in[src++];
    std::uint32_t literal = 0;
    if ((op >> 4) == 2) {
        // First opcode of a stream that starts with a short header: two bytes are skipped and the
        // length of the first literal run is the low bits of the third.
        src += 2;
        if (src > in.size())
            return fail("the header is cut off");
        literal = next() & 7;
    }
    // The back-reference instruction in `op` (its first byte), decoded into offset and length;
    // `op` then holds the byte after the instruction, whose low bits count the literals to follow.
    auto instruction = [&](std::uint32_t& offset, std::uint32_t& length) {
        switch (op >> 4) {
            case 0:
                length = (op & 0xF) + 0x13;
                offset = next();
                op = next();
                length += (op >> 3) & 0x10;
                offset += ((op & 0x78) << 5) + 1;
                break;
            case 1:
                length = (op & 0xF) + 3;
                offset = next();
                op = next();
                offset += ((op & 0xF8) << 5) + 1;
                break;
            case 2:
                offset = next();
                offset |= (next() << 8) & 0xFF00;
                length = op & 7;
                if ((op & 8) == 0) {
                    op = next();
                    length += op & 0xF8;
                } else {
                    ++offset;
                    length += next() << 3;
                    op = next();
                    length += ((op & 0xF8) << 8) + 0x100;
                }
                break;
            default:
                length = op >> 4;
                offset = op & 15;
                op = next();
                offset += ((op & 0xF8) << 1) + 1;
                break;
        }
    };
    for (;;) {
        if (bad)
            return fail("ends inside an instruction");
        if (literal == 0) {
            literal = op + 8;
            if (literal == 0x17) {
                std::uint32_t n = next();
                literal += n;
                if (n == 0xFF) {
                    do {
                        n = next();
                        n |= next() << 8;
                        literal += n;
                    } while (n == 0xFFFF && !bad);
                }
            }
        }
        if (bad || literal > in.size() - src || literal > expected - dst)
            return fail("a literal run does not fit");
        // Literal runs are stored back to front (checked against real files: the sizes in the
        // file header only come out right that way).
        for (std::uint32_t k = 0; k < literal; ++k)
            out[dst + k] = in[src + literal - 1 - k];
        src += literal;
        dst += literal;
        literal = 0;
        if (dst == expected)
            return true;
        if (src >= in.size())
            return fail("ends early");
        op = in[src++];
        std::uint32_t offset = 0, length = 0;
        instruction(offset, length);
        for (;;) {
            if (bad)
                return fail("ends inside an instruction");
            if (offset > dst || length > expected - dst)
                return fail("a back reference does not fit");
            for (std::uint32_t k = 0; k < length; ++k, ++dst)
                out[dst] = out[dst - offset];
            literal = op & 7;
            if (literal != 0 || src >= in.size())
                break;
            op = in[src++];
            if ((op >> 4) == 0)
                break;
            if ((op >> 4) == 15)
                op &= 15;
            instruction(offset, length);
        }
        if (dst == expected && literal == 0)
            return true;
    }
}

}  // namespace tbcad::dwg::detail

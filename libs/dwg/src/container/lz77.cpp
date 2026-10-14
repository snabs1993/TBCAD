#include "container/lz77.h"

#include <string>

namespace tbcad::dwg::detail {

namespace {

class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> in) : in_(in) {}
    bool atEnd() const { return pos_ >= in_.size(); }
    std::size_t pos() const { return pos_; }
    std::uint8_t peek() const { return pos_ < in_.size() ? in_[pos_] : 0; }
    std::uint8_t next() {
        if (pos_ >= in_.size()) {
            failed = true;
            return 0;
        }
        return in_[pos_++];
    }
    bool failed = false;
    std::span<const std::uint8_t> take(std::size_t n) {
        if (n > in_.size() - pos_) {
            failed = true;
            return {};
        }
        auto s = in_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

private:
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
};

// "Literal Length": 1..15 means value + 3; 0 starts a run of 0xFF steps; a byte with a high
// nibble means no literals (the byte is the next opcode and is left in the stream).
std::size_t literalLength(Cursor& c) {
    const std::uint8_t b = c.peek();
    if (b & 0xF0)
        return 0;
    c.next();
    if (b != 0)
        return static_cast<std::size_t>(b) + 3;
    std::size_t total = 0x0F;
    for (int guard = 0; guard < (1 << 20); ++guard) {
        const std::uint8_t x = c.next();
        if (c.failed)
            return 0;
        if (x == 0) {
            total += 0xFF;
            continue;
        }
        total += x;
        break;
    }
    return total + 3;
}

// "Long Compression Offset": a byte, or 0xFF plus further bytes while they are zero.
std::size_t longCount(Cursor& c) {
    std::uint8_t b = c.next();
    if (b != 0)
        return b;
    std::size_t total = 0xFF;
    for (int guard = 0; guard < (1 << 20); ++guard) {
        b = c.next();
        if (c.failed)
            return 0;
        if (b == 0) {
            total += 0xFF;
            continue;
        }
        return total + b;
    }
    return total;
}

// `extra` is the distance bias: the stored offset counts back from the byte before the output
// position, so the source is `size - (offset + extra)`. The spec does not say whether the
// stored value is zero based; the caller tries 1 first (offset 0 would copy from nothing).
// `hiBit`: read bit 3 of an 0x12..0x1F opcode as bit 14 of the distance and the length from the
// low three bits (a second reading of that opcode, tried when the first does not fit).
// `leftover` receives the input bytes not consumed on success.
bool run(std::span<const std::uint8_t> in, std::size_t expected, std::size_t extra, bool hiBit,
         std::vector<std::uint8_t>& out, std::string* why, std::size_t& leftover) {
    auto fail = [&](const char* what, const Cursor& cur) {
        if (why)
            *why = std::string(what) + " (input byte " + std::to_string(cur.pos()) + " of " +
                   std::to_string(in.size()) + ", output " + std::to_string(out.size()) + " of " +
                   std::to_string(expected) + ")";
        return false;
    };
    out.clear();
    out.reserve(expected);
    Cursor c(in);
    leftover = 0;
    auto copyLiterals = [&](std::size_t n) {
        if (n > expected - out.size()) {
            // Trailing bytes after a complete output are not data.
            if (out.size() == expected) {
                leftover = in.size() - c.pos() + 1;  // ended without the terminator
                return true;
            }
            return fail("literal run too long", c);
        }
        const auto s = c.take(n);
        if (c.failed)
            return fail("literal run cut off", c);
        out.insert(out.end(), s.begin(), s.end());
        return true;
    };
    if (!copyLiterals(literalLength(c)))
        return false;  // reason already recorded
    for (;;) {
        if (c.atEnd())
            return out.size() == expected || fail("input ended early", c);
        // Complete output with input left over (padding or a checksum): done.
        if (out.size() == expected) {
            leftover = in.size() - c.pos() + 1;  // ended without the terminator
            return true;
        }
        const std::uint8_t op = c.next();
        std::size_t count = 0, offset = 0, lit = 0;
        auto twoByte = [&]() {
            const std::uint8_t first = c.next();
            const std::uint8_t second = c.next();
            offset =
                (static_cast<std::size_t>(first) >> 2) | (static_cast<std::size_t>(second) << 6);
            lit = first & 3;
        };
        if (op == 0x11) {
            if (out.size() != expected)
                return fail("terminator before the expected size", c);
            leftover = 0;  // the stream ended with its terminator at the expected size
            return true;
        }
        if (op < 0x10) {
            return fail("unexpected opcode", c);  // 0x00..0x0F cannot start an opcode set
        } else if (op == 0x10) {
            count = longCount(c) + 9;
            twoByte();
            offset += 0x3FFF;
        } else if (op < 0x20) {
            count = (op & (hiBit ? 0x07 : 0x0F)) + 2;
            twoByte();
            offset += 0x3FFF;
            if (hiBit && (op & 0x08))
                offset += 0x4000;
        } else if (op == 0x20) {
            count = longCount(c) + 0x21;
            twoByte();
        } else if (op < 0x40) {
            count = static_cast<std::size_t>(op) - 0x1E;
            twoByte();
        } else {
            count = (static_cast<std::size_t>(op & 0xF0) >> 4) - 1;
            const std::uint8_t second = c.next();
            offset = (static_cast<std::size_t>(second) << 2) | ((op & 0x0C) >> 2);
            lit = op & 3;
        }
        if (c.failed)
            return fail("opcode cut off", c);
        if (lit == 0 && op != 0x11)
            lit = literalLength(c);
        if (c.failed)
            return fail("literal length cut off", c);
        const std::size_t back = offset + extra;
        if (back == 0 || back > out.size() || count > expected - out.size())
            return fail(back == 0 || back > out.size() ? "copy distance outside the output"
                                                       : "copy longer than the expected size",
                        c);
        // The ranges may overlap (a distance shorter than the length repeats a pattern).
        std::size_t from = out.size() - back;
        for (std::size_t i = 0; i < count; ++i)
            out.push_back(out[from + i]);
        if (!copyLiterals(lit))
            return false;
    }
}

}  // namespace

bool lz77Decompress(std::span<const std::uint8_t> in, std::size_t expected,
                    std::vector<std::uint8_t>& out, int* distanceBias, std::string* why) {
    // Prefer a reading that consumes the whole input; fall back to one that only fills the output.
    std::vector<std::uint8_t> best;
    int bestBias = -1;
    bool first = true;
    for (int bias : {1, 0}) {
        for (bool hi : {false, true}) {
            std::size_t left = 0;
            std::string reason;
            if (!run(in, expected, static_cast<std::size_t>(bias), hi, out, first ? why : &reason,
                     left)) {
                first = false;
                continue;
            }
            first = false;
            if (left == 0) {
                if (distanceBias)
                    *distanceBias = bias;
                return true;
            }
            if (bestBias < 0) {
                best = out;
                bestBias = bias;
            }
        }
    }
    if (bestBias < 0)
        return false;
    out = std::move(best);
    if (distanceBias)
        *distanceBias = bestBias;
    return true;
}

}  // namespace tbcad::dwg::detail

#pragma once

// Small byte buffer helpers for the collaboration wire formats (internal).

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace tbcad::collab::detail {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Writer {
public:
    std::vector<std::uint8_t>& bytes() { return b_; }
    void u8(std::uint8_t v) { b_.push_back(v); }
    void varint(std::uint64_t v) {
        while (v >= 0x80) {
            b_.push_back(static_cast<std::uint8_t>(v | 0x80));
            v >>= 7;
        }
        b_.push_back(static_cast<std::uint8_t>(v));
    }
    void raw(std::span<const std::uint8_t> s) { b_.insert(b_.end(), s.begin(), s.end()); }

private:
    std::vector<std::uint8_t> b_{};
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> s) : s_(s) {}
    std::size_t remaining() const { return s_.size() - pos_; }
    bool atEnd() const { return pos_ == s_.size(); }
    std::uint8_t u8() {
        if (pos_ >= s_.size())
            throw FormatError("unexpected end of data");
        return s_[pos_++];
    }
    std::uint64_t varint() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            std::uint8_t b = u8();
            if (shift == 63 && b > 1)
                throw FormatError("varint overflows 64 bits");
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80))
                return v;
        }
        throw FormatError("varint too long");
    }
    // An element count that can still fit in what is left.
    std::size_t count(std::size_t minElementBytes) {
        std::uint64_t n = varint();
        if (minElementBytes != 0 && n > remaining() / minElementBytes)
            throw FormatError("element count exceeds the remaining data");
        return static_cast<std::size_t>(n);
    }
    std::span<const std::uint8_t> rest() {
        auto r = s_.subspan(pos_);
        pos_ = s_.size();
        return r;
    }

private:
    std::span<const std::uint8_t> s_;
    std::size_t pos_ = 0;
};

}  // namespace tbcad::collab::detail

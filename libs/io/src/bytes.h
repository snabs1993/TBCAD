#pragma once

// Little-endian byte buffers used by the .tbcad reader and writer (internal to libs/io).

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/math.h"

namespace tbcad::io::detail {

// Thrown by ByteReader and the decoders; always caught at the public API boundary.
struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class ByteWriter {
public:
    std::vector<std::uint8_t>& bytes() { return b_; }
    const std::vector<std::uint8_t>& bytes() const { return b_; }
    std::size_t size() const { return b_.size(); }
    void reserve(std::size_t n) { b_.reserve(n); }

    void u8(std::uint8_t v) { b_.push_back(v); }
    void u16(std::uint16_t v) { le(v, 2); }
    void u32(std::uint32_t v) { le(v, 4); }
    void u64(std::uint64_t v) { le(v, 8); }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void vec2(const Vec2& v) {
        f64(v.x);
        f64(v.y);
    }
    // LEB128, 1..10 bytes.
    void varint(std::uint64_t v) {
        while (v >= 0x80) {
            b_.push_back(static_cast<std::uint8_t>(v | 0x80));
            v >>= 7;
        }
        b_.push_back(static_cast<std::uint8_t>(v));
    }
    void raw(const void* p, std::size_t n) {
        const auto* c = static_cast<const std::uint8_t*>(p);
        b_.insert(b_.end(), c, c + n);
    }
    void raw(std::span<const std::uint8_t> s) { raw(s.data(), s.size()); }
    void patchU32(std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b_[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }

private:
    void le(std::uint64_t v, int n) {
        const std::size_t at = b_.size();
        b_.resize(at + static_cast<std::size_t>(n));
        if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(b_.data() + at, &v, static_cast<std::size_t>(n));
        } else {
            for (int i = 0; i < n; ++i)
                b_[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }
    std::vector<std::uint8_t> b_{};
};

// Bounds-checked reader over a byte span. Every overrun throws FormatError.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> s) : s_(s) {}

    std::size_t remaining() const { return s_.size() - pos_; }
    std::size_t pos() const { return pos_; }
    bool atEnd() const { return pos_ == s_.size(); }

    std::uint8_t u8() { return static_cast<std::uint8_t>(le(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(le(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(le(4)); }
    std::uint64_t u64() { return le(8); }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    double f64() { return std::bit_cast<double>(u64()); }
    bool boolean() {
        std::uint8_t v = u8();
        if (v > 1)
            throw FormatError("invalid boolean");
        return v == 1;
    }
    Vec2 vec2() {
        Vec2 v;
        v.x = f64();
        v.y = f64();
        return v;
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
    // Reads an element count and checks that `count * minElementBytes` can still fit in the
    // remaining bytes, so a corrupt count can never trigger a huge allocation.
    std::size_t count(std::size_t minElementBytes) {
        std::uint64_t n = varint();
        if (minElementBytes != 0 && n > remaining() / minElementBytes)
            throw FormatError("element count exceeds the remaining data");
        return static_cast<std::size_t>(n);
    }
    std::span<const std::uint8_t> bytes(std::size_t n) {
        if (n > remaining())
            throw FormatError("unexpected end of data");
        auto r = s_.subspan(pos_, n);
        pos_ += n;
        return r;
    }
    std::span<const std::uint8_t> rest() { return bytes(remaining()); }
    void skip(std::size_t n) { (void)bytes(n); }

private:
    std::uint64_t le(int n) {
        if (static_cast<std::size_t>(n) > remaining())
            throw FormatError("unexpected end of data");
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i)
            v |= static_cast<std::uint64_t>(s_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
        pos_ += static_cast<std::size_t>(n);
        return v;
    }
    std::span<const std::uint8_t> s_;
    std::size_t pos_ = 0;
};

std::uint32_t tbcadCrc32(std::span<const std::uint8_t> data);

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24;
}
std::string fourccName(std::uint32_t v);

inline bool validUtf8(std::string_view s) {
    std::size_t i = 0, n = s.size();
    while (i < n) {
        auto c = static_cast<std::uint8_t>(s[i]);
        std::size_t len;
        std::uint32_t cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n)
            return false;
        for (std::size_t k = 1; k < len; ++k) {
            auto cc = static_cast<std::uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

}  // namespace tbcad::io::detail

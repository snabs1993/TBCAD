#include "bits/bit_reader.h"

#include <array>
#include <cstring>

namespace tbcad::dwg {

void BitReader::seekBit(std::size_t bit) {
    if (bit > bitSize()) {
        failed_ = true;
        pos_ = bitSize();
        return;
    }
    pos_ = bit;
}

std::uint64_t BitReader::bits(unsigned n) {
    if (n == 0)
        return 0;
    if (failed_ || n > 64 || bitsLeft() < n) {
        failed_ = true;
        pos_ = bitSize();
        return 0;
    }
    std::uint64_t v = 0;
    unsigned left = n;
    while (left > 0) {
        const std::size_t byte = pos_ >> 3;
        const unsigned used = static_cast<unsigned>(pos_ & 7);
        const unsigned avail = 8 - used;
        const unsigned take = left < avail ? left : avail;
        const unsigned shift = avail - take;
        const unsigned chunk = (data_[byte] >> shift) & ((1u << take) - 1u);
        v = (v << take) | chunk;
        pos_ += take;
        left -= take;
    }
    return v;
}

unsigned BitReader::b3() {
    unsigned v = 0;
    for (int i = 0; i < 3; ++i) {
        const unsigned bit = static_cast<unsigned>(bits(1));
        v = (v << 1) | bit;
        if (!bit)
            break;
    }
    return v;
}

std::uint16_t BitReader::rs() {
    const std::uint32_t lo = rc();
    const std::uint32_t hi = rc();
    return failed_ ? 0 : static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint32_t BitReader::rl() {
    const std::uint32_t a = rs();
    const std::uint32_t b = rs();
    return failed_ ? 0 : a | (b << 16);
}

std::uint64_t BitReader::rll() {
    const std::uint64_t a = rl();
    const std::uint64_t b = rl();
    return failed_ ? 0 : a | (b << 32);
}

double BitReader::rd() {
    const std::uint64_t u = rll();
    double d;
    std::memcpy(&d, &u, sizeof d);
    return d;
}

std::int32_t BitReader::bs() {
    switch (bb()) {
        case 0:
            return static_cast<std::int16_t>(rs());
        case 1:
            return rc();
        case 2:
            return 0;
        default:
            return 256;
    }
}

std::int32_t BitReader::bl() {
    switch (bb()) {
        case 0:
            return static_cast<std::int32_t>(rl());
        case 1:
            return rc();
        case 2:
            return 0;
        default:
            failed_ = true;  // code 3 is not used
            return 0;
    }
}

std::int64_t BitReader::bll() {
    const unsigned len = b3();
    std::uint64_t v = 0;
    for (unsigned i = 0; i < len; ++i)
        v |= static_cast<std::uint64_t>(rc()) << (8 * i);
    return static_cast<std::int64_t>(v);
}

double BitReader::bd() {
    switch (bb()) {
        case 0:
            return rd();
        case 1:
            return 1.0;
        case 2:
            return 0.0;
        default:
            failed_ = true;
            return 0.0;
    }
}

double BitReader::bdWithDefault(double def) {
    std::uint8_t raw[8];
    std::memcpy(raw, &def, 8);
    switch (bb()) {
        case 0:
            return def;
        case 1:
            for (int i = 0; i < 4; ++i)
                raw[i] = rc();
            break;
        case 2:
            raw[4] = rc();
            raw[5] = rc();
            for (int i = 0; i < 4; ++i)
                raw[i] = rc();
            break;
        default:
            return rd();
    }
    double d;
    std::memcpy(&d, raw, 8);
    return d;
}

double BitReader::bitThickness() {
    return b() ? 0.0 : bd();
}

std::int64_t BitReader::mc() {
    std::uint64_t v = 0;
    unsigned shift = 0;
    for (int i = 0; i < 10; ++i) {
        const std::uint8_t byte = rc();
        if (failed_)
            return 0;
        if (!(byte & 0x80)) {
            const bool neg = (byte & 0x40) != 0;
            v |= static_cast<std::uint64_t>(byte & 0x3f) << shift;
            return neg ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
        }
        v |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        shift += 7;
    }
    failed_ = true;
    return 0;
}

std::uint64_t BitReader::mcUnsigned() {
    std::uint64_t v = 0;
    unsigned shift = 0;
    for (int i = 0; i < 10; ++i) {
        const std::uint8_t byte = rc();
        if (failed_)
            return 0;
        v |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return v;
        shift += 7;
    }
    failed_ = true;
    return 0;
}

std::uint32_t BitReader::ms() {
    std::uint64_t v = 0;
    unsigned shift = 0;
    for (int i = 0; i < 3; ++i) {
        const std::uint16_t word = rs();
        if (failed_)
            return 0;
        v |= static_cast<std::uint64_t>(word & 0x7fff) << shift;
        if (!(word & 0x8000))
            return static_cast<std::uint32_t>(v);
        shift += 15;
    }
    failed_ = true;
    return 0;
}

HandleRef BitReader::handle() {
    HandleRef h;
    h.code = static_cast<std::uint8_t>(bits(4));
    const unsigned n = static_cast<unsigned>(bits(4));
    for (unsigned i = 0; i < n; ++i)
        h.value = (h.value << 8) | rc();
    return h;
}

std::uint32_t BitReader::objectTypeR2010() {
    switch (bb()) {
        case 0:
            return rc();
        case 1:
            return rc() + 0x1f0u;
        default:
            return rs();
    }
}

std::string BitReader::textBytes() {
    if (strings_) {
        std::string t = strings_->textUnicode();
        if (strings_->failed())
            failed_ = true;
        return t;
    }
    const std::int32_t n = bs();
    std::string s;
    if (n <= 0 || failed_)
        return s;
    if (static_cast<std::size_t>(n) * 8 > bitsLeft()) {
        failed_ = true;
        return s;
    }
    s.reserve(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i)
        s.push_back(static_cast<char>(rc()));
    // Stored strings carry a terminating zero.
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

std::string BitReader::textUnicode() {
    const std::int32_t n = bs();
    std::string out;
    if (n <= 0 || failed_)
        return out;
    if (static_cast<std::size_t>(n) * 16 > bitsLeft()) {
        failed_ = true;
        return out;
    }
    std::uint32_t pendingHigh = 0;
    for (std::int32_t i = 0; i < n; ++i) {
        std::uint32_t c = rs();
        if (c == 0)
            break;
        if (c >= 0xd800 && c < 0xdc00) {
            pendingHigh = c;
            continue;
        }
        if (c >= 0xdc00 && c < 0xe000 && pendingHigh) {
            c = 0x10000 + ((pendingHigh - 0xd800) << 10) + (c - 0xdc00);
            pendingHigh = 0;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

std::uint64_t resolveHandle(const HandleRef& ref, std::uint64_t reference) {
    switch (ref.code) {
        case 6:
            return reference + 1;
        case 8:
            return reference - 1;
        case 10:
            return reference + ref.value;
        case 12:
            return reference - ref.value;
        default:
            return ref.value;
    }
}

namespace {

const std::array<std::uint16_t, 256>& table16() {
    static const auto t = [] {
        std::array<std::uint16_t, 256> a{};
        for (unsigned i = 0; i < 256; ++i) {
            std::uint16_t c = static_cast<std::uint16_t>(i);
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? static_cast<std::uint16_t>((c >> 1) ^ 0xA001)
                            : static_cast<std::uint16_t>(c >> 1);
            a[i] = c;
        }
        return a;
    }();
    return t;
}

const std::array<std::uint32_t, 256>& table32() {
    static const auto t = [] {
        std::array<std::uint32_t, 256> a{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            a[i] = c;
        }
        return a;
    }();
    return t;
}

}  // namespace

std::uint16_t crc16(std::uint16_t start, std::span<const std::uint8_t> data) {
    const auto& t = table16();
    std::uint16_t dx = start;
    for (std::uint8_t byte : data) {
        const std::uint8_t al = static_cast<std::uint8_t>(byte ^ (dx & 0xff));
        dx = static_cast<std::uint16_t>(((dx >> 8) & 0xff) ^ t[al]);
    }
    return dx;
}

std::uint32_t crc32(std::uint32_t seed, std::span<const std::uint8_t> data) {
    const auto& t = table32();
    std::uint32_t c = ~seed;
    for (std::uint8_t byte : data)
        c = (c >> 8) ^ t[(c ^ byte) & 0xff];
    return ~c;
}

}  // namespace tbcad::dwg

namespace tbcad::dwg {

std::uint32_t BitReader::cmc(bool r2004, std::int32_t* index) {
    const std::int32_t idx = bs();
    if (index)
        *index = idx;
    if (!r2004)
        return 0;
    const std::uint32_t word = bl();
    const unsigned flags = rc();
    if (flags & 1)
        textBytes();
    if (flags & 2)
        textBytes();
    return word;
}

bool locateStringStream(std::span<const std::uint8_t> data, std::size_t endBit,
                        BitReader& strings) {
    strings = BitReader();
    if (endBit < 1 || endBit > data.size() * 8)
        return endBit == 0;
    BitReader flag(data, endBit - 1);
    if (!flag.b())
        return !flag.failed();
    if (endBit < 17)
        return false;
    std::size_t pos = endBit - 1 - 16;
    BitReader a(data, pos);
    std::uint32_t size = a.rs();
    if (size & 0x8000) {
        if (pos < 16)
            return false;
        pos -= 16;
        BitReader b(data, pos);
        const std::uint32_t hi = b.rs();
        size = (size & 0x7FFF) | (hi << 15);
    }
    if (a.failed() || size > pos)
        return false;
    strings = BitReader(data, pos - size);
    return true;
}

}  // namespace tbcad::dwg

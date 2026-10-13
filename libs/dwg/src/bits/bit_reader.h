// Reads the bit-packed value forms used inside DWG object data (ODA spec section 2).
// Every read is bounds-checked: past the end the reader sets `failed()` and returns zero, so a
// damaged file can never cause an out-of-range access.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace tbcad::dwg {

// A handle reference: how it was stored (code) and the number it holds.
struct HandleRef {
    std::uint8_t code = 0;
    std::uint64_t value = 0;
    bool operator==(const HandleRef&) const = default;
};

class BitReader {
public:
    BitReader() = default;
    explicit BitReader(std::span<const std::uint8_t> data, std::size_t bitPos = 0)
        : data_(data), pos_(bitPos) {}

    bool failed() const { return failed_; }
    void fail() { failed_ = true; }
    std::size_t bitPos() const { return pos_; }
    std::size_t bitSize() const { return data_.size() * 8; }
    std::size_t bitsLeft() const { return pos_ >= bitSize() ? 0 : bitSize() - pos_; }
    void seekBit(std::size_t bit);
    void alignByte() { pos_ = (pos_ + 7) & ~std::size_t{7}; }
    // Byte position after rounding up to a byte boundary.
    std::size_t bytePos() const { return (pos_ + 7) / 8; }

    // Raw bits, most significant first; n <= 64.
    std::uint64_t bits(unsigned n);
    bool b() { return bits(1) != 0; }                         // B
    unsigned bb() { return static_cast<unsigned>(bits(2)); }  // BB
    unsigned b3();                                            // 3B
    std::uint8_t rc() { return static_cast<std::uint8_t>(bits(8)); }
    std::uint16_t rs();
    std::uint32_t rl();
    std::uint64_t rll();
    double rd();
    std::int32_t bs();                 // BS
    std::int32_t bl();                 // BL
    std::int64_t bll();                // BLL (R2010+)
    double bd();                       // BD
    double bdWithDefault(double def);  // DD
    double bitThickness();             // BT (R2000+ form)
    std::int64_t mc();                 // MC (signed, bit 0x40 of the last byte negates)
    std::uint64_t mcUnsigned();        // MC without the sign rule (object map offsets)
    std::uint32_t ms();                // MS
    HandleRef handle();                // H
    // Object type of R2010+ (bit pair, then one or two bytes).
    std::uint32_t objectTypeR2010();

    // T for R2004 and older: BS length, then that many bytes in the file code page.
    std::string textBytes();
    // TU: BS length in characters, then UTF-16LE; returned as UTF-8.
    std::string textUnicode();

private:
    std::span<const std::uint8_t> data_{};
    std::size_t pos_ = 0;
    bool failed_ = false;
};

// Resolves a stored handle against the handle of the object that holds it (spec 2.13).
std::uint64_t resolveHandle(const HandleRef& ref, std::uint64_t referenceHandle);

// CRC-16 as used by the file header and objects (modified start value, spec 2.14.1) and the
// plain CRC-32 of R2018 (spec 2.14.2); tables are generated at start-up.
std::uint16_t crc16(std::uint16_t start, std::span<const std::uint8_t> data);
std::uint32_t crc32(std::uint32_t seed, std::span<const std::uint8_t> data);

}  // namespace tbcad::dwg

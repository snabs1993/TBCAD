// Reads the bit-packed value forms used inside DWG object data (ODA spec section 2).
// Every read is bounds-checked: past the end the reader sets `failed()` and returns zero, so a
// damaged file can never cause an out-of-range access.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#if defined(_MSC_VER)
#include <stdlib.h>
#endif

namespace tbcad::dwg {

// A handle reference: how it was stored (code) and the number it holds.
struct HandleRef {
    std::uint8_t code = 0;
    std::uint64_t value = 0;
    bool operator==(const HandleRef&) const = default;
};

// Code page of the drawing being read (the DWG header index: 30 = ANSI_1252, 37 = ANSI_874 Thai,
// 2 = ISO 8859-1). textBytes() turns the 8-bit strings of R2004 and older into UTF-8 with it.
// Returns true when strings will be converted, so the DXF twin can be written as UTF-8.
bool setTextCodepage(int dwgCodepage);

// Converts bytes in the given DWG code page to UTF-8; unknown pages return the input unchanged.
std::string codepageToUtf8(const std::string& bytes, int dwgCodepage);

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
    std::uint64_t bits(unsigned n) {
        // Fast path: one 8 byte load covers the bits (n + position in the byte <= 64).
        const std::size_t byte = pos_ >> 3;
        const unsigned used = static_cast<unsigned>(pos_ & 7);
        if (n != 0 && n + used <= 64 && !failed_ && byte + 8 <= data_.size()) {
            std::uint64_t w;
            std::memcpy(&w, data_.data() + byte, 8);
            w = swap64(w);  // first byte of the stream to the top
            pos_ += n;
            return (w << used) >> (64 - n);
        }
        return bitsSlow(n);
    }
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
    // R2007 and later keep every string in a separate stream of the same object; once set,
    // textBytes() reads (UTF-16) from there. The pointed-to reader must outlive this one.
    void setStringStream(BitReader* strings) { strings_ = strings; }
    // CMC: a plain index up to R2000; from R2004 on index, RGB word, flags and optional names.
    // Returns the RGB word (0 before R2004).
    std::uint32_t cmc(bool r2004, std::int32_t* index = nullptr);
    // TU: BS length in characters, then UTF-16LE; returned as UTF-8.
    std::string textUnicode();

private:
    static std::uint64_t swap64(std::uint64_t v) {
#if defined(_MSC_VER)
        return _byteswap_uint64(v);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return v;
#else
        return __builtin_bswap64(v);
#endif
    }
    std::uint64_t bitsSlow(unsigned n);
    std::span<const std::uint8_t> data_{};
    std::size_t pos_ = 0;
    bool failed_ = false;
    BitReader* strings_ = nullptr;
};

// Finds the string stream of an R2007+ object, header or class section whose bit stream ends
// at `endBit`: the last bit says whether one exists, the 16 bits before it hold its size in
// bits (plus a second 16 bit part when the first has 0x8000 set). The spec gives that distance
// as "16 bytes" and "128 bits"; the natural layout is assumed and has to be confirmed with real
// files. Returns false on damage; `strings` stays empty when there is no string stream.
bool locateStringStream(std::span<const std::uint8_t> data, std::size_t endBit, BitReader& strings);

// Resolves a stored handle against the handle of the object that holds it (spec 2.13).
std::uint64_t resolveHandle(const HandleRef& ref, std::uint64_t referenceHandle);

// CRC-16 as used by the file header and objects (modified start value, spec 2.14.1) and the
// plain CRC-32 of R2018 (spec 2.14.2); tables are generated at start-up.
std::uint16_t crc16(std::uint16_t start, std::span<const std::uint8_t> data);
std::uint32_t crc32(std::uint32_t seed, std::span<const std::uint8_t> data);

}  // namespace tbcad::dwg

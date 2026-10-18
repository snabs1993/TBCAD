#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bits/bit_reader.h"

using namespace tbcad::dwg;

namespace {

// "0101 0011" style text to bytes (spaces ignored; padded with zeros).
std::vector<std::uint8_t> fromBits(const std::string& s) {
    std::vector<std::uint8_t> out;
    unsigned n = 0, cur = 0;
    for (char c : s) {
        if (c != '0' && c != '1')
            continue;
        cur = (cur << 1) | static_cast<unsigned>(c - '0');
        if (++n == 8) {
            out.push_back(static_cast<std::uint8_t>(cur));
            n = cur = 0;
        }
    }
    if (n) {
        cur <<= 8 - n;
        out.push_back(static_cast<std::uint8_t>(cur));
    }
    return out;
}

}  // namespace

TEST_CASE("bitshort forms: short, byte, zero, 256") {
    auto d = fromBits("00 00000001 00000001  10  11  01 00001111  10");
    BitReader r(d);
    CHECK(r.bs() == 257);
    CHECK(r.bs() == 0);
    CHECK(r.bs() == 256);
    CHECK(r.bs() == 15);
    CHECK(r.bs() == 0);
    CHECK_FALSE(r.failed());
}

TEST_CASE("negative bitshort uses the 16 bit form") {
    auto d = fromBits("00 11111111 11111111");
    BitReader r(d);
    CHECK(r.bs() == -1);
}

TEST_CASE("bitlong forms") {
    auto d = fromBits("00 00000001 00000001 00000000 00000000  10  01 00001111  10");
    BitReader r(d);
    CHECK(r.bl() == 257);
    CHECK(r.bl() == 0);
    CHECK(r.bl() == 15);
    CHECK(r.bl() == 0);
    CHECK_FALSE(r.failed());
    auto bad = fromBits("11");
    BitReader rb(bad);
    rb.bl();
    CHECK(rb.failed());
}

TEST_CASE("bitdouble forms") {
    std::vector<std::uint8_t> d = fromBits("01 10 00");
    // 00 is followed by eight raw bytes; 2.5 = 0x4004000000000000 little-endian.
    d.push_back(0);
    d.push_back(0);
    d.push_back(0);
    d.push_back(0);
    d.push_back(0);
    d.push_back(0);
    d.push_back(0x04);
    d.push_back(0x40);
    // The two bits after "00" shift the raw bytes by 6 bits: build the stream by hand instead.
    auto s = fromBits(
        "01 10 00 " +
        std::string("00000000 00000000 00000000 00000000 00000000 00000000 00000100 01000000"));
    BitReader r(s);
    CHECK(r.bd() == 1.0);
    CHECK(r.bd() == 0.0);
    CHECK(r.bd() == 2.5);
    CHECK_FALSE(r.failed());
}

TEST_CASE("modular chars from the worked examples") {
    std::vector<std::uint8_t> a{0x82, 0x24};
    BitReader ra(a);
    CHECK(ra.mc() == 4610);
    std::vector<std::uint8_t> b{0xE9, 0x97, 0xE6, 0x35};
    BitReader rb(b);
    CHECK(rb.mc() == 112823273);
    std::vector<std::uint8_t> c{0x85, 0x4B};
    BitReader rc(c);
    CHECK(rc.mc() == -1413);
    std::vector<std::uint8_t> unterminated{0x80, 0x80};
    BitReader ru(unterminated);
    ru.mc();
    CHECK(ru.failed());
}

TEST_CASE("modular short from the worked example") {
    std::vector<std::uint8_t> d{0x31, 0xF4, 0x8D, 0x00};
    BitReader r(d);
    CHECK(r.ms() == 4650033);
}

TEST_CASE("handles: counter bytes and relative codes") {
    // 5.2.05.E7 -> hard pointer, value 0x05E7
    std::vector<std::uint8_t> d{0x52, 0x05, 0xE7, 0x60};
    BitReader r(d);
    HandleRef h = r.handle();
    CHECK(h.code == 5);
    CHECK(h.value == 0x5E7);
    HandleRef rel = r.handle();  // 6.0: reference + 1
    CHECK(rel.code == 6);
    CHECK(resolveHandle(rel, 0x100) == 0x101);
    CHECK(resolveHandle({8, 0}, 0x100) == 0xFF);
    CHECK(resolveHandle({10, 5}, 0x100) == 0x105);
    CHECK(resolveHandle({12, 5}, 0x100) == 0xFB);
    CHECK(resolveHandle({4, 0x22}, 0x100) == 0x22);
}

TEST_CASE("BLL counts its bytes with a plain 3-bit length") {
    auto d = fromBits("000  010 00000001 00000010  001 00000111");
    BitReader r(d);
    CHECK(r.bll() == 0);
    CHECK(r.bll() == 0x0201);
    CHECK(r.bll() == 7);
    CHECK_FALSE(r.failed());
}

TEST_CASE("3B, object type of R2010 and bit-level raw values") {
    auto d = fromBits("0  10  110  111");
    BitReader r(d);
    CHECK(r.b3() == 0);
    CHECK(r.b3() == 2);
    CHECK(r.b3() == 6);
    CHECK(r.b3() == 7);
    auto t = fromBits("00 00001010  01 00000001  10 00000001 00000010");
    BitReader rt(t);
    CHECK(rt.objectTypeR2010() == 10);
    CHECK(rt.objectTypeR2010() == 0x1f1);
    CHECK(rt.objectTypeR2010() == 0x0201);
}

TEST_CASE("bitdouble with default patches bytes of the default") {
    // opcode 01: four bytes replace the first four bytes of the default.
    double def = 1.0;  // 0x3FF0000000000000
    auto d = fromBits("01 00000000 00000000 00000000 00000000");
    BitReader r(d);
    CHECK(r.bdWithDefault(def) == 1.0);
    auto none = fromBits("00");
    BitReader rn(none);
    CHECK(rn.bdWithDefault(7.5) == 7.5);
}

TEST_CASE("text: bytes and utf-16") {
    auto t = fromBits("01 00000011 01000001 01000010 00000000");
    BitReader r(t);
    CHECK(r.textBytes() == "AB");
    auto u = fromBits("01 00000010 01000001 00000000 00001110 00001110");
    BitReader ru(u);
    // 'A' then U+0E0E (a Thai letter)
    CHECK(ru.textUnicode() == std::string("A\xE0\xB8\x8E"));
}

TEST_CASE("reading past the end fails safely") {
    std::vector<std::uint8_t> d{0xFF};
    BitReader r(d);
    CHECK(r.rl() == 0);
    CHECK(r.failed());
    CHECK(r.bs() == 0);
    CHECK(r.textBytes().empty());
    BitReader empty;
    empty.rd();
    CHECK(empty.failed());
}

TEST_CASE("crc tables") {
    CHECK(crc16(0, std::vector<std::uint8_t>{0x01}) == 0xC0C1);
    CHECK(crc32(0, std::vector<std::uint8_t>{'1', '2', '3', '4', '5', '6', '7', '8', '9'}) ==
          0xCBF43926u);
}

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "container/lz21.h"
#include "container/r2007.h"

using tbcad::dwg::detail::lz21Decompress;
using tbcad::dwg::detail::rsDataBytes;

namespace {

std::string expand(const std::vector<std::uint8_t>& in, std::size_t expected, bool* ok = nullptr) {
    std::vector<std::uint8_t> out;
    const bool good = lz21Decompress(in, expected, out);
    if (ok)
        *ok = good;
    return std::string(out.begin(), out.end());
}

}  // namespace

TEST_CASE("R2007 compression: a short header, a literal run and a back reference") {
    // 0x22 starts the short header (two bytes skipped), 0x03 is the literal count, then the
    // literals; 0x30 0x00 copies 3 bytes from one byte back and ends with no literals to follow.
    const std::vector<std::uint8_t> in = {0x22, 0x00, 0x00, 0x03, 'A', 'B', 'C', 0x30, 0x00};
    bool ok = false;
    CHECK(expand(in, 6, &ok) == "ABCCCC");
    CHECK(ok);
}

TEST_CASE("R2007 compression: a literal run longer than the short form") {
    // An opcode below 0x10 gives a literal run of opcode + 8 bytes.
    std::vector<std::uint8_t> in = {0x02};
    const std::string text = "0123456789";
    in.insert(in.end(), text.begin(), text.end());
    bool ok = false;
    CHECK(expand(in, 10, &ok) == text);
    CHECK(ok);
}

TEST_CASE("R2007 compression: damaged input is refused") {
    std::vector<std::uint8_t> out;
    CHECK_FALSE(lz21Decompress({}, 5, out));
    const std::vector<std::uint8_t> shortRun = {0x02, 'a', 'b'};
    CHECK_FALSE(lz21Decompress(shortRun, 10, out));
    const std::vector<std::uint8_t> farBack = {0x22, 0, 0, 0x01, 'A', 0x30, 0xF8};
    CHECK_FALSE(lz21Decompress(farBack, 8, out));
}

TEST_CASE("Reed-Solomon code words: interleaved and plain data bytes") {
    // Two blocks of (n = 6, k = 4): block 0 data a0..a3, block 1 data b0..b3.
    std::vector<std::uint8_t> interleaved(12, 0xEE);
    const std::uint8_t a[4] = {1, 2, 3, 4}, b[4] = {5, 6, 7, 8};
    for (int j = 0; j < 4; ++j) {
        interleaved[static_cast<std::size_t>(j) * 2] = a[j];
        interleaved[static_cast<std::size_t>(j) * 2 + 1] = b[j];
    }
    const auto got = rsDataBytes(interleaved, 2, 4, 6, true);
    REQUIRE(got.size() == 8);
    CHECK(got[0] == 1);
    CHECK(got[3] == 4);
    CHECK(got[4] == 5);
    CHECK(got[7] == 8);
    const std::vector<std::uint8_t> plain = {1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE};
    CHECK(rsDataBytes(plain, 2, 4, 6, false) == got);
}

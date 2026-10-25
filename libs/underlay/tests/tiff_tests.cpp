#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../src/tiff.h"
#include "tiff_fixtures.h"

using namespace tbcad;
using namespace tbcad::underlay;

namespace {

template <std::size_t N>
Result<std::shared_ptr<const ImageData>> decode(const std::uint8_t (&bytes)[N]) {
    return decodeTiff(bytes, N);
}

}  // namespace

TEST_CASE("TIFF: uncompressed 1-bit and Group 4 give the same picture") {
    auto raw = decode(fixtures::raw1);
    auto g4 = decode(fixtures::g4);
    REQUIRE(raw.ok());
    REQUIRE(g4.ok());
    CHECK(raw.value()->width == 2700);
    CHECK(raw.value()->height == 24);
    CHECK(g4.value()->rgba == raw.value()->rgba);
    // The picture is not blank: it has both black and white pixels.
    bool black = false, white = false;
    for (std::size_t i = 0; i < raw.value()->rgba.size(); i += 4)
        (raw.value()->rgba[i] == 0 ? black : white) = true;
    CHECK(black);
    CHECK(white);
}

TEST_CASE("TIFF: LZW, PackBits and Deflate RGB give the picture of the uncompressed one") {
    auto raw = decode(fixtures::rgbraw);
    REQUIRE(raw.ok());
    CHECK(raw.value()->width == 24);
    CHECK(raw.value()->height == 16);
    const auto& px = raw.value()->rgba;
    CHECK(px[(3 * 24 + 5) * 4 + 0] == 50);
    CHECK(px[(3 * 24 + 5) * 4 + 1] == 45);
    CHECK(px[(3 * 24 + 5) * 4 + 2] == 15);
    for (auto r :
         {decode(fixtures::rgblzw), decode(fixtures::rgbpack), decode(fixtures::rgbdefl)}) {
        REQUIRE(r.ok());
        CHECK(r.value()->rgba == px);
    }
}

TEST_CASE("TIFF: grey LZW matches grey uncompressed, damaged files are errors") {
    auto a = decode(fixtures::graylzw);
    auto b = decode(fixtures::grayraw);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.value()->rgba == b.value()->rgba);
    CHECK_FALSE(decodeTiff(fixtures::g4, 20).ok());
    CHECK_FALSE(decodeTiff(fixtures::g4, 8).ok());
    std::uint8_t junk[16] = {'I', 'I', 42, 0, 0xFF, 0xFF, 0xFF, 0x7F};
    CHECK_FALSE(decodeTiff(junk, sizeof junk).ok());
    // Cut in the middle of the Group 4 data: an error or a picture, never a crash.
    (void)decodeTiff(fixtures::g4, 400);
}

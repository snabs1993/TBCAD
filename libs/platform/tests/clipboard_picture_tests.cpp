#include <doctest/doctest.h>

#include <vector>

#include "tbcad/platform/clipboard_picture.h"

using namespace tbcad::platform;

namespace {

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

std::vector<std::uint8_t> header(int w, int h, int bits, std::uint32_t compression = 0,
                                 std::uint32_t colours = 0) {
    std::vector<std::uint8_t> v;
    put32(v, 40);
    put32(v, static_cast<std::uint32_t>(w));
    put32(v, static_cast<std::uint32_t>(h));
    v.push_back(1);
    v.push_back(0);
    v.push_back(static_cast<std::uint8_t>(bits));
    v.push_back(0);
    put32(v, compression);
    put32(v, 0);
    put32(v, 0);
    put32(v, 0);
    put32(v, colours);
    put32(v, 0);
    return v;
}

}  // namespace

TEST_CASE("a 24-bit bottom-up bitmap comes out top row first, as RGBA") {
    auto v = header(2, 2, 24);
    // bottom row first, each row padded to four bytes: blue, green | red, white
    for (std::uint8_t b : {255, 0, 0, 0, 255, 0, 0, 0})  // B G R order: blue px, green px, pad
        v.push_back(b);
    for (std::uint8_t b : {0, 0, 255, 255, 255, 255, 0, 0})  // red px, white px, pad
        v.push_back(b);
    auto pic = parseDib(v.data(), v.size());
    REQUIRE(pic);
    CHECK(pic->width == 2);
    CHECK(pic->height == 2);
    // top row is the last stored: red, white
    CHECK(pic->rgba[0] == 255);
    CHECK(pic->rgba[1] == 0);
    CHECK(pic->rgba[2] == 0);
    CHECK(pic->rgba[4] == 255);
    CHECK(pic->rgba[5] == 255);
    // bottom row: blue, green
    CHECK(pic->rgba[8 + 2] == 255);
    CHECK(pic->rgba[12 + 1] == 255);
}

TEST_CASE("a 32-bit bitmap with a zero fourth byte is opaque, a top-down height is honoured") {
    auto v = header(1, -2, 32);
    for (std::uint8_t b : {10, 20, 30, 0, 40, 50, 60, 0})
        v.push_back(b);
    auto pic = parseDib(v.data(), v.size());
    REQUIRE(pic);
    CHECK(pic->height == 2);
    CHECK(pic->rgba[0] == 30);  // B,G,R = 10,20,30 -> R is 30
    CHECK(pic->rgba[3] == 255);
    CHECK(pic->rgba[4] == 60);
}

TEST_CASE("an 8-bit bitmap uses its colour table") {
    auto v = header(1, 1, 8, 0, 2);
    put32(v, 0x00000000);
    put32(v, 0x00FF8000);  // R=255 G=128 B=0
    for (std::uint8_t b : {1, 0, 0, 0})
        v.push_back(b);
    auto pic = parseDib(v.data(), v.size());
    REQUIRE(pic);
    CHECK(pic->rgba[0] == 255);
    CHECK(pic->rgba[1] == 128);
    CHECK(pic->rgba[2] == 0);
}

TEST_CASE("cut-short or unsupported bitmap data is refused") {
    auto v = header(4, 4, 24);
    CHECK_FALSE(parseDib(v.data(), v.size()));
    CHECK_FALSE(parseDib(nullptr, 0));
    auto rle = header(1, 1, 8, 1);
    CHECK_FALSE(parseDib(rle.data(), rle.size()));
}

TEST_CASE("buildDib writes a bottom-up 24-bit bitmap that parseDib reads back") {
    ClipboardPicture pic;
    pic.width = 3;
    pic.height = 2;
    for (int i = 0; i < 6; ++i) {
        pic.rgba.push_back(static_cast<std::uint8_t>(10 * i));
        pic.rgba.push_back(static_cast<std::uint8_t>(20 + i));
        pic.rgba.push_back(static_cast<std::uint8_t>(200 - i));
        pic.rgba.push_back(255);
    }
    auto dib = buildDib(pic);
    REQUIRE(!dib.empty());
    auto back = parseDib(dib.data(), dib.size());
    REQUIRE(back.has_value());
    CHECK(back->width == 3);
    CHECK(back->height == 2);
    CHECK(back->rgba == pic.rgba);
}

TEST_CASE("buildDib blends transparent pixels over white and refuses empty pictures") {
    ClipboardPicture pic;
    pic.width = 1;
    pic.height = 1;
    pic.rgba = {0, 0, 0, 0};
    auto dib = buildDib(pic);
    auto back = parseDib(dib.data(), dib.size());
    REQUIRE(back.has_value());
    CHECK(back->rgba[0] == 255);
    CHECK(buildDib(ClipboardPicture{}).empty());
}

TEST_CASE("the stashed clipboard picture is handed out once") {
    CHECK(!takeStashedClipboardPicture().has_value());
    ClipboardPicture pic;
    pic.width = 5;
    stashClipboardPicture(pic);
    auto got = takeStashedClipboardPicture();
    REQUIRE(got.has_value());
    CHECK(got->width == 5);
    CHECK(!takeStashedClipboardPicture().has_value());
}

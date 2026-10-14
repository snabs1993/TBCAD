#include <doctest/doctest.h>

#include <string>

#include "container/lz77.h"

using tbcad::dwg::detail::lz77Decompress;

namespace {

std::string expand(const std::vector<std::uint8_t>& in, std::size_t expected, bool* ok = nullptr) {
    std::vector<std::uint8_t> out;
    const bool good = lz77Decompress(in, expected, out);
    if (ok)
        *ok = good;
    return std::string(out.begin(), out.end());
}

std::vector<std::uint8_t> bytes(std::initializer_list<int> v) {
    std::vector<std::uint8_t> r;
    for (int x : v)
        r.push_back(static_cast<std::uint8_t>(x));
    return r;
}

}  // namespace

TEST_CASE("a literal run followed by the terminator expands to itself") {
    auto in = bytes({0x05, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 0x11});
    CHECK(expand(in, 8) == "ABCDEFGH");
}

TEST_CASE("a long literal length is read from the zero-prefixed form") {
    std::vector<std::uint8_t> in = {0x00, 0x02};
    std::string text(20, 'x');
    in.insert(in.end(), text.begin(), text.end());
    in.push_back(0x11);
    CHECK(expand(in, 20) == text);
}

TEST_CASE("a back reference copies earlier output, then literals follow") {
    // ABCDEFGH, copy 4 from 8 back, one literal Z
    auto in = bytes({0x05, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 0x5D, 0x01, 'Z', 0x11});
    CHECK(expand(in, 13) == "ABCDEFGHABCDZ");
}

TEST_CASE("a distance shorter than the length repeats a pattern") {
    auto in = bytes({0x01, 'X', 'Y', 'Z', 'W', 0xB0, 0x00, 0x11});
    CHECK(expand(in, 14) == "XYZWWWWWWWWWWW");
}

TEST_CASE("the two byte offset form carries its own literal count") {
    auto in = bytes({0x01, 'A', 'B', 'C', 'D', 0x22, 0x0D, 0x00, 'Q', 0x11});
    CHECK(expand(in, 9) == "ABCDABCDQ");
}

TEST_CASE("damaged streams fail instead of reading or writing out of bounds") {
    bool ok = true;
    expand(bytes({0x05, 'A', 'B'}), 8, &ok);  // cut off in the literals
    CHECK_FALSE(ok);
    expand(bytes({0x01, 'A', 'B', 'C', 'D', 0x5D, 0x70, 'Z', 0x11}), 20,
           &ok);  // reaches too far back
    CHECK_FALSE(ok);
    expand(bytes({0x05, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 0x11}), 4, &ok);  // too long
    CHECK_FALSE(ok);
    expand(bytes({0x05, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 0x03}), 12, &ok);  // bad opcode
    CHECK_FALSE(ok);
    expand({}, 5, &ok);
    CHECK_FALSE(ok);
}

TEST_CASE("bytes left over after the expected size are ignored") {
    bool ok = false;
    CHECK(expand(bytes({0x01, 'A', 'B', 'C', 'D', 0x11, 0x00, 0x00}), 4, &ok) == "ABCD");
    CHECK(ok);
    ok = false;
    CHECK(expand(bytes({0x01, 'A', 'B', 'C', 'D', 0x07, 0x07}), 4, &ok) == "ABCD");
    CHECK(ok);
}

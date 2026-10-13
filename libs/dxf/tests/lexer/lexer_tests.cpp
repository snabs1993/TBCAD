
#include <string>

#include "doctest_include.h"
#include "tbcad/dxf/lexer.h"
#include "test_util.h"

namespace tbcad::dxf {
namespace {

std::vector<Group> lexAll(std::string_view text, Report& report) {
    static std::vector<std::byte> keep;  // groups point into the buffer
    keep = testing::bytesOf(text);
    auto lx = Lexer::open(keep, report);
    REQUIRE(lx.ok());
    std::vector<Group> out;
    while (auto g = lx.value().next()) {
        out.push_back(*g);
    }
    return out;
}

}  // namespace

TEST_CASE("valueTypeForCode follows the group code table") {
    CHECK(valueTypeForCode(0) == ValueType::String);
    CHECK(valueTypeForCode(2) == ValueType::String);
    CHECK(valueTypeForCode(5) == ValueType::Handle);
    CHECK(valueTypeForCode(10) == ValueType::Double);
    CHECK(valueTypeForCode(39) == ValueType::Double);
    CHECK(valueTypeForCode(40) == ValueType::Double);
    CHECK(valueTypeForCode(62) == ValueType::Int16);
    CHECK(valueTypeForCode(70) == ValueType::Int16);
    CHECK(valueTypeForCode(90) == ValueType::Int32);
    CHECK(valueTypeForCode(105) == ValueType::Handle);
    CHECK(valueTypeForCode(160) == ValueType::Int64);
    CHECK(valueTypeForCode(210) == ValueType::Double);
    CHECK(valueTypeForCode(290) == ValueType::Bool);
    CHECK(valueTypeForCode(310) == ValueType::Binary);
    CHECK(valueTypeForCode(330) == ValueType::Handle);
    CHECK(valueTypeForCode(370) == ValueType::Int16);
    CHECK(valueTypeForCode(420) == ValueType::Int32);
    CHECK(valueTypeForCode(999) == ValueType::String);
    CHECK(valueTypeForCode(1004) == ValueType::Binary);
    CHECK(valueTypeForCode(1040) == ValueType::Double);
    CHECK(valueTypeForCode(1070) == ValueType::Int16);
    CHECK(valueTypeForCode(1071) == ValueType::Int32);
}

TEST_CASE("open rejects input that cannot be DXF") {
    Report r;
    CHECK_FALSE(Lexer::open({}, r).ok());
    const auto text = testing::bytesOf("hello world\nthis is not dxf\n");
    auto bad = Lexer::open(text, r);
    REQUIRE_FALSE(bad.ok());
    CHECK_FALSE(bad.error().message.empty());
    const auto onlyBlank = testing::bytesOf("\n\n  \n");
    CHECK_FALSE(Lexer::open(onlyBlank, r).ok());
    const auto badBinary = testing::bytesOf(std::string("AutoCAD Binary DXF\r\n\x1a\0", 22) + "xx");
    CHECK_FALSE(Lexer::open(badBinary, r).ok());
}

TEST_CASE("string values are slices of the input; code 0 is trimmed, others are exact") {
    Report r;
    const auto g = lexAll("0\nTEXT \n1\n  two  words \n0\nEOF\n", r);
    REQUIRE(g.size() == 3);
    CHECK(g[0].str() == "TEXT");
    CHECK(g[1].str() == "  two  words ");
    CHECK(r.empty());
}

TEST_CASE("handles keep their text; non-hex handles are flagged") {
    Report r;
    const auto g = lexAll("5\n1F\n5\nARROW\n105\n  ff \n0\nEOF\n", r);
    REQUIRE(g.size() == 4);
    const auto& h0 = std::get<Handle>(g[0].value);
    CHECK(h0.valid);
    CHECK(h0.value == 0x1F);
    const auto& h1 = std::get<Handle>(g[1].value);
    CHECK_FALSE(h1.valid);
    CHECK(h1.text == "ARROW");
    CHECK(std::get<Handle>(g[2].value).value == 0xFF);
}

TEST_CASE("integers: unsigned 16/32-bit spellings keep their bits, others clamp with a warning") {
    Report r;
    const auto g = lexAll("70\n65535\n70\n40000000\n90\n4294967295\n70\n 7 \n290\n5\n0\nEOF\n", r);
    REQUIRE(g.size() == 6);
    CHECK(std::get<std::int16_t>(g[0].value) == -1);
    CHECK(std::get<std::int16_t>(g[1].value) == 32767);
    CHECK(std::get<std::int32_t>(g[2].value) == -1);
    CHECK(std::get<std::int16_t>(g[3].value) == 7);
    CHECK(std::get<bool>(g[4].value) == true);
    CHECK(r.count(Severity::Warning) == 1);
    CHECK(r.countCode(code::kBadNumber) == 1);
}

TEST_CASE("numbers: exponent, leading plus, trailing dot, garbage") {
    Report r;
    const auto g = lexAll("10\n1.5E+02\n20\n+3.\n30\n.5\n40\nabc\n41\n1.5x\n0\nEOF\n", r);
    REQUIRE(g.size() == 6);
    CHECK(g[0].number() == 150.0);
    CHECK(g[1].number() == 3.0);
    CHECK(g[2].number() == 0.5);
    CHECK(g[3].number() == 0.0);
    CHECK(g[4].number() == 1.5);
    CHECK(r.countCode(code::kBadNumber) == 2);
}

TEST_CASE("far coordinates stay exact doubles") {
    Report r;
    const auto g = lexAll("10\n123456789012.123456\n0\nEOF\n", r);
    CHECK(g[0].number() == 123456789012.123456);
}

TEST_CASE("lone CR, CRLF and LF all end lines; line numbers count each") {
    Report r;
    const auto g = lexAll("0\rSECTION\r\n2\nHEADER\r0\nEOF", r);
    REQUIRE(g.size() == 3);
    CHECK(g[0].line == 1);
    CHECK(g[1].line == 3);
    CHECK(g[2].line == 5);
    CHECK(r.empty());
}

TEST_CASE("nothing is returned after EOF") {
    Report r;
    const auto g = lexAll("0\nEOF\n0\nSECTION\n", r);
    CHECK(g.size() == 1);
}

TEST_CASE("desync with no resynchronisation point ends the stream with a warning") {
    Report r;
    const auto g = lexAll("0\nSECTION\nxx\n1\n2\n3\n", r);
    CHECK(g.size() == 1);
    CHECK(r.countCode(code::kBadGroupCode) == 1);
    CHECK(r.countCode(code::kResync) == 1);
    CHECK(r.countCode(code::kMissingEof) == 1);
}

TEST_CASE("an out-of-range group code is treated as a desync") {
    Report r;
    const auto g = lexAll("0\nSECTION\n99999\nx\n0\nENDSEC\n0\nEOF\n", r);
    REQUIRE(g.size() == 3);
    CHECK(g[1].str() == "ENDSEC");
}

TEST_CASE("binary DXF: resynchronises after a bad group code") {
    std::string d("AutoCAD Binary DXF\r\n\x1a\0", 22);
    auto add = [&](std::string_view s) { d.append(s); };
    add(std::string_view("\0\0SECTION\0", 10));  // 0 SECTION
    add(std::string_view("\xff\x7f", 2));        // code 0x7fff: invalid
    add(std::string_view("garbage", 7));
    add(std::string_view("\0\0ENDSEC\0\0\0EOF\0", 16));  // 0 ENDSEC, 0 EOF
    Report r;
    const auto g = lexAll(d, r);
    REQUIRE(g.size() == 3);
    CHECK(g[1].str() == "ENDSEC");
    CHECK(g[2].str() == "EOF");
    CHECK(r.countCode(code::kBadGroupCode) == 1);
    CHECK(r.countCode(code::kResync) == 1);
}

TEST_CASE("a big file lexes without trouble") {
    std::string text = "0\nSECTION\n2\nENTITIES\n";
    for (int i = 0; i < 100000; ++i) {
        text += "0\nLINE\n8\n0\n10\n" + std::to_string(i) + ".5\n20\n1.0\n11\n2.0\n21\n3.0\n";
    }
    text += "0\nENDSEC\n0\nEOF\n";
    Report r;
    const auto g = lexAll(text, r);
    CHECK(g.size() == 2 + 100000 * 6 + 2);
    CHECK(r.empty());
}

}  // namespace tbcad::dxf

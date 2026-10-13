
#include <string>

#include "doctest_include.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/dxf/lexer.h"
#include "test_util.h"

namespace tbcad::dxf {
namespace {

constexpr const char* kThaiRoom = "ห้องนอน";  // UTF-8 source (compiled with /utf-8 on MSVC)

// "ห้องนอน" in cp874.
const std::string kThaiRoomCp874 = "\xCB\xE9\xCD\xA7\xB9\xCD\xB9";

Encoding detect(std::string_view bytes, std::string_view ver, std::string_view cp, Report& r) {
    const auto b = testing::bytesOf(bytes);
    return detectEncoding(b, ver, cp, r);
}

}  // namespace

TEST_CASE("decodeText: cp874 bytes for a Thai word") {
    CHECK(decodeText(kThaiRoomCp874, Encoding::Cp874) == kThaiRoom);
}

TEST_CASE("decodeText: \\U+nnnn escapes") {
    CHECK(decodeText("\\U+0E2B\\U+0E49", Encoding::Cp1252) == "ห้");
    CHECK(decodeText("\\U+0e2b", Encoding::Utf8) == "ห");
    CHECK(decodeText("A\\U+00E9B", Encoding::Cp1252) == "AéB");
    CHECK(decodeText("\\U+D83D\\U+DE00", Encoding::Cp1252) == "\xF0\x9F\x98\x80");
    CHECK(decodeText("\\U+D83D!", Encoding::Cp1252) == "\xEF\xBF\xBD!");
    // Not an escape: too few digits, or another letter.
    CHECK(decodeText("\\U+12", Encoding::Cp1252) == "\\U+12");
    CHECK(decodeText("C:\\Users", Encoding::Cp1252) == "C:\\Users");
}

TEST_CASE("decodeText: cp874 text mixed with escapes and ASCII") {
    const std::string raw = "Room " + kThaiRoomCp874 + " \\U+0E2B\\U+0E49 #1";
    CHECK(decodeText(raw, Encoding::Cp874) == std::string("Room ") + kThaiRoom + " ห้ #1");
}

TEST_CASE("decodeText: single-byte codepages") {
    CHECK(decodeText("caf\xE9", Encoding::Cp1252) == "café");
    CHECK(decodeText("\x80", Encoding::Cp1252) == "€");
    CHECK(decodeText("\xCF\xF0\xE8\xE2\xE5\xF2", Encoding::Cp1251) == "Привет");
    CHECK(decodeText("\xDB", Encoding::Cp874) == "\xEF\xBF\xBD");  // unassigned
    CHECK(decodeText("\xE1\xE2", Encoding::Cp1253) == "αβ");
    CHECK(decodeText("\xE0", Encoding::Cp1256) == "à");
}

TEST_CASE("decodeText: double-byte codepages keep trail bytes out of escape handling") {
    CHECK(decodeText("\x95\x5C", Encoding::Cp932) == "表");  // trail byte is a backslash
    CHECK(decodeText("a\x82\xA0z", Encoding::Cp932) == "aあz");
    CHECK(decodeText("\xB0\xA1", Encoding::Cp936) == "啊");
    CHECK(decodeText("\xB0\xA1", Encoding::Cp949) == "가");
    CHECK(decodeText("\xA4\x40", Encoding::Cp950) == "一");
    CHECK(decodeText("\xB1", Encoding::Cp932) == "ｱ");  // half-width katakana, single byte
    CHECK(decodeText("\x81", Encoding::Cp932) == "\xEF\xBF\xBD");  // lead byte at the end
}

TEST_CASE("decodeText: \\M+ multibyte escapes") {
    CHECK(decodeText("\\M+1955C", Encoding::Cp1252) == "表");
    CHECK(decodeText("\\M+482A0", Encoding::Cp1252) == "\\M+482A0");  // Johab: unsupported
    CHECK(decodeText("\\M+5B0A1", Encoding::Cp1252) == "啊");
    CHECK(decodeText("\\M+3B0A1", Encoding::Cp1252) == "가");
    CHECK(decodeText("\\M+2A440", Encoding::Cp1252) == "一");
}

TEST_CASE("decodeText: UTF-8 input is validated") {
    CHECK(decodeText(kThaiRoom, Encoding::Utf8) == kThaiRoom);
    CHECK(decodeText("a\xFF"
                     "b",
                     Encoding::Utf8) ==
          "a\xEF\xBF\xBD"
          "b");
    CHECK(decodeText("\xE0\xB8", Encoding::Utf8) == "\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(decodeText("\xC0\x80", Encoding::Utf8) == "\xEF\xBF\xBD\xEF\xBF\xBD");  // overlong
}

TEST_CASE("encodingFromName") {
    CHECK(encodingFromName("ANSI_874") == Encoding::Cp874);
    CHECK(encodingFromName("ansi_1252") == Encoding::Cp1252);
    CHECK(encodingFromName(" ANSI_932\r") == Encoding::Cp932);
    CHECK(encodingFromName("UTF-8") == Encoding::Utf8);
    CHECK(encodingFromName("8859_1") == Encoding::Cp1252);
    CHECK_FALSE(encodingFromName("UNDEFINED").has_value());
    CHECK_FALSE(encodingFromName("").has_value());
    CHECK(encodingName(Encoding::Cp874) == "ANSI_874");
}

TEST_CASE("detectEncoding: BOM and R2007+ are UTF-8, whatever $DWGCODEPAGE says") {
    Report r;
    CHECK(detect("\xEF\xBB\xBF"
                 "0\nEOF\n",
                 "AC1015", "ANSI_874", r) == Encoding::Utf8);
    CHECK(detect("1\n" + std::string(kThaiRoom) + "\n", "AC1027", "ANSI_874", r) == Encoding::Utf8);
    CHECK(detect("1\n" + std::string(kThaiRoom) + "\n", "AC1021", "", r) == Encoding::Utf8);
    CHECK(r.empty());
}

TEST_CASE("detectEncoding: invalid UTF-8 in an R2007+ file falls back with a warning") {
    Report r;
    CHECK(detect("1\n" + kThaiRoomCp874 + "\n", "AC1021", "ANSI_874", r) == Encoding::Cp874);
    CHECK(r.countCode(code::kEncodingMismatch) == 1);
}

TEST_CASE("detectEncoding: $DWGCODEPAGE decides for older files") {
    Report r;
    CHECK(detect("1\n\x95\x5C\n", "AC1015", "ANSI_932", r) == Encoding::Cp932);
    CHECK(detect("1\n" + kThaiRoomCp874 + "\n", "AC1015", "ANSI_874", r) == Encoding::Cp874);
    CHECK(detect("1\ncaf\xE9\n", "AC1009", "ANSI_1252", r) == Encoding::Cp1252);
    CHECK(detect("1\ncaf\xE9\n", "AC1009", "ANSI_1251", r) == Encoding::Cp1251);
    CHECK(r.empty());
}

TEST_CASE("detectEncoding: Thai bytes without a codepage are guessed as cp874 and reported") {
    Report r;
    CHECK(detect("1\n" + kThaiRoomCp874 + "\n", "AC1015", "", r) == Encoding::Cp874);
    CHECK(r.countCode(code::kEncodingGuess) == 1);
    Report r2;
    CHECK(detect("1\n" + kThaiRoomCp874 + "\n", "", "", r2) == Encoding::Cp874);
}

TEST_CASE("detectEncoding: Thai text under a declared ANSI_1252 is corrected, with a warning") {
    Report r;
    CHECK(detect("1\n" + kThaiRoomCp874 + "\n", "AC1015", "ANSI_1252", r) == Encoding::Cp874);
    CHECK(r.countCode(code::kEncodingMismatch) == 1);
}

TEST_CASE("detectEncoding: Latin text stays cp1252, valid UTF-8 beats a missing codepage") {
    Report r;
    CHECK(detect("1\ncaf\xE9\n1\nna\xEFve\n", "AC1015", "", r) == Encoding::Cp1252);
    CHECK(detect("1\ncaf\xC3\xA9\n", "AC1015", "ANSI_1252", r) == Encoding::Utf8);
    CHECK(detect("1\nplain ascii\n", "AC1015", "", r) == Encoding::Cp1252);
    CHECK(detect("1\nplain ascii\n", "AC1015", "ANSI_874", r) == Encoding::Cp874);
}

TEST_CASE("detectEncoding: unknown codepage names are reported and ignored") {
    Report r;
    CHECK(detect("1\ncaf\xE9\n", "AC1015", "DOS437", r) == Encoding::Cp1252);
    CHECK(r.countCode(code::kEncodingUnknown) == 1);
}

TEST_CASE("scanHeaderHints reads $ACADVER and $DWGCODEPAGE from the HEADER only") {
    const auto b = testing::bytesOf(
        "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1015\n9\n$DWGCODEPAGE\n3\nANSI_874\n"
        "0\nENDSEC\n0\nSECTION\n2\nENTITIES\n9\n$ACADVER\n1\nAC9999\n0\nENDSEC\n0\nEOF\n");
    const HeaderHints h = scanHeaderHints(b);
    CHECK(h.acadVer == "AC1015");
    CHECK(h.dwgCodepage == "ANSI_874");
    CHECK(scanHeaderHints(testing::bytesOf("not dxf")).acadVer.empty());
}

TEST_CASE("corpus: cp874 Thai without $DWGCODEPAGE decodes to the Thai word") {
    const auto bytes =
        testing::readFile(testing::corpusPath("synthetic/lexer/r2000_cp874_thai.dxf"));
    const HeaderHints h = scanHeaderHints(bytes);
    CHECK(h.acadVer == "AC1015");
    CHECK(h.dwgCodepage.empty());
    Report r;
    const Encoding enc = detectEncoding(bytes, h.acadVer, h.dwgCodepage, r);
    CHECK(enc == Encoding::Cp874);
    auto lx = Lexer::open(bytes, r);
    REQUIRE(lx.ok());
    std::string text;
    while (auto g = lx->next()) {
        if (g->code == 1)
            text = decodeText(g->str(), enc);
    }
    CHECK(text == kThaiRoom);
}

TEST_CASE("corpus: R2013 UTF-8 Thai with a stale ANSI_1252 header") {
    const auto bytes =
        testing::readFile(testing::corpusPath("synthetic/lexer/r2013_utf8_thai.dxf"));
    const HeaderHints h = scanHeaderHints(bytes);
    CHECK(h.acadVer == "AC1027");
    CHECK(h.dwgCodepage == "ANSI_1252");
    Report r;
    const Encoding enc = detectEncoding(bytes, h.acadVer, h.dwgCodepage, r);
    CHECK(enc == Encoding::Utf8);
    auto lx = Lexer::open(bytes, r);
    REQUIRE(lx.ok());
    std::string text;
    while (auto g = lx->next()) {
        if (g->code == 1)
            text = decodeText(g->str(), enc);
    }
    CHECK(text == kThaiRoom);
}

TEST_CASE("stripMText") {
    CHECK(stripMText("{\\fArial|b0;Room}\\P\\H2.5;ห้อง") == "Room\nห้อง");
    CHECK(stripMText("plain") == "plain");
    CHECK(stripMText("line1\\Pline2\\Pline3") == "line1\nline2\nline3");
    CHECK(stripMText("\\C1;red \\Lunder\\l \\Oover\\o \\Kstrike\\k") == "red under over strike");
    CHECK(stripMText("\\W0.8;\\T1.2;\\Q15;\\A1;text") == "text");
    CHECK(stripMText("\\pxqc;centered") == "centered");
    CHECK(stripMText("a\\\\b \\{x\\}") == "a\\b {x}");
    CHECK(stripMText("1\\S1/2;\"") == "11/2\"");
    CHECK(stripMText("\\S+0.1^-0.1;") == "+0.1 -0.1");
    CHECK(stripMText("A\\~B") ==
          "A\xC2\xA0"
          "B");
    CHECK(stripMText("{\\H2x;{\\C3;nested}}") == "nested");
    // Tolerant: missing terminators and a lone trailing backslash.
    CHECK(stripMText("\\fArial no semicolon") == "Arial no semicolon");
    CHECK(stripMText("abc\\") == "abc");
    CHECK(stripMText("") == "");
    CHECK(stripMText("%%c25 %%d %%p0.1") ==
          "\xE2\x8C\x80"
          "25 \xC2\xB0 \xC2\xB1"
          "0.1");
}

TEST_CASE("replacePercentCodes") {
    CHECK(replacePercentCodes("%%C25") ==
          "\xE2\x8C\x80"
          "25");
    CHECK(replacePercentCodes("100%%%") == "100%");
    CHECK(replacePercentCodes("%%uunder%%u") == "under");
    CHECK(replacePercentCodes("%%065") == "A");
    CHECK(replacePercentCodes("50%") == "50%");
    CHECK(replacePercentCodes("%%") == "%%");
    CHECK(replacePercentCodes("%%x") == "%%x");
}

}  // namespace tbcad::dxf

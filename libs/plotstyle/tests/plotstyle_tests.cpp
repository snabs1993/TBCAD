#include <doctest/doctest.h>
#include <miniz.h>

#include <cstring>

#include "tbcad/plotstyle/plotstyle.h"

using namespace tbcad;
using namespace tbcad::plotstyle;

namespace {

// A .ctb image written the way the format describes it: header, three words, zlib stream.
std::vector<std::uint8_t> makeCtb(const std::string& text) {
    static const char header[] = "PIAFILEVERSION_2.0,CTBVER1,compress\r\npmzlibcodec";
    std::vector<std::uint8_t> out(header, header + sizeof header - 1);
    mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(text.size()));
    std::vector<std::uint8_t> z(cap);
    mz_ulong n = cap;
    REQUIRE(mz_compress(z.data(), &n, reinterpret_cast<const unsigned char*>(text.data()),
                        static_cast<mz_ulong>(text.size())) == MZ_OK);
    auto put = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    put(0);  // checksum
    put(static_cast<std::uint32_t>(text.size()));
    put(static_cast<std::uint32_t>(n));
    out.insert(out.end(), z.begin(), z.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

}  // namespace

TEST_CASE("palette colours map to indices; other colours do not") {
    CHECK(paletteIndexOf(paletteColor(1)) == 1);
    CHECK(paletteIndexOf(paletteColor(200)) == 200);
    CHECK(paletteIndexOf(Rgba{1, 2, 3, 255}) == 0);
}

TEST_CASE("monochrome plots every palette colour black and leaves true colours alone") {
    auto t = builtinTable("Monochrome.CTB");
    REQUIRE(t);
    CHECK(mapColor(*t, paletteColor(1)) == Rgba{0, 0, 0, 255});
    CHECK(mapColor(*t, paletteColor(7)) == Rgba{0, 0, 0, 255});
    CHECK(mapColor(*t, Rgba{10, 200, 30, 255}) == Rgba{10, 200, 30, 255});
    CHECK_FALSE(builtinTable("nope.ctb"));
    for (const auto& n : builtinTableNames())
        CHECK(builtinTable(n));
}

TEST_CASE("grayscale and screening") {
    auto g = builtinTable("grayscale.ctb");
    Rgba c = mapColor(*g, paletteColor(1));  // red
    CHECK(c.r == c.g);
    CHECK(c.g == c.b);
    CHECK(c.r > 0);
    auto s = builtinTable("screening-50.ctb");
    Rgba red = paletteColor(1);
    Rgba half = mapColor(*s, red);
    CHECK(half.r == 255);
    CHECK(half.g == doctest::Approx(127.5).epsilon(0.01));
    CHECK(mapLineweight(*s, red, 0.35) == doctest::Approx(0.35));
}

TEST_CASE("an entry can set the lineweight") {
    PlotStyleTable t;
    t.entries[3].lineweightMm = 0.7;
    CHECK(mapLineweight(t, paletteColor(3), 0.25) == doctest::Approx(0.7));
    CHECK(mapLineweight(t, paletteColor(4), 0.25) == doctest::Approx(0.25));
}

TEST_CASE("the text format round-trips and rejects nonsense") {
    PlotStyleTable t;
    t.name = "mine";
    t.description = "Walls heavy, text light";
    t.entries[1].color = Rgba{0x12, 0xab, 0xcd, 255};
    t.entries[1].lineweightMm = 0.5;
    t.entries[2].screen = 40;
    t.entries[2].grayscale = true;
    t.entries[5].name = "Doors";
    auto back = fromText(toText(t));
    REQUIRE(back.ok());
    CHECK(back.value() == t);

    CHECK_FALSE(fromText("hello").ok());
    CHECK_FALSE(fromText("TBCAD-PLOT-STYLE 1\n300 color=#000000\n").ok());
    CHECK_FALSE(fromText("TBCAD-PLOT-STYLE 1\n1 color=red\n").ok());
    CHECK_FALSE(fromText("TBCAD-PLOT-STYLE 1\n1 lineweight=-1\n").ok());
}

TEST_CASE("a .ctb file is imported: colours, lineweights, screening") {
    const std::string text =
        "description=\"Office standard\"\n"
        "aci_table_available=TRUE\n"
        "lineweight_table{\n"
        " 0=0.0000\n"
        " 1=0.0500\n"
        " 2=0.2500\n"
        "}\n"
        "aci_table{\n"
        " 0=\"Color_1\"\n"
        " 1=\"Color_2\"\n"
        " 2=\"Color_3\"\n"
        "}\n"
        "plot_style{\n"
        " 0{\n"
        "  name=\"Color_1\"\n"
        "  color=-1006632961\n"  // 0xC3FFFFFF: white
        "  screen=100\n"
        "  lineweight=2\n"
        " }\n"
        " 1{\n"
        "  name=\"Color_2\"\n"
        "  color=-1\n"
        "  screen=30\n"
        "  lineweight=0\n"
        " }\n"
        " 2{\n"
        "  name=\"Color_3\"\n"
        "  color=-1023409921\n"  // 0xC30000FF = blue
        "  screen=100\n"
        "  lineweight=1\n"
        " }\n"
        "}\n";
    auto r = importCtb(makeCtb(text));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const PlotStyleTable& t = r.value();
    CHECK(t.description == "Office standard");
    REQUIRE(t.entries[1].color);
    CHECK(*t.entries[1].color == Rgba{255, 255, 255, 255});
    CHECK(t.entries[1].lineweightMm.value() == doctest::Approx(0.25));
    CHECK_FALSE(t.entries[2].color);
    CHECK(t.entries[2].screen == 30);
    CHECK_FALSE(t.entries[2].lineweightMm);
    REQUIRE(t.entries[3].color);
    CHECK(*t.entries[3].color == Rgba{0, 0, 255, 255});
    CHECK(t.entries[3].lineweightMm.value() == doctest::Approx(0.05));
    CHECK(t.entries[1].name == "Color_1");
}

TEST_CASE("bad .ctb input is refused without crashing") {
    CHECK_FALSE(importCtb({}).ok());
    std::vector<std::uint8_t> junk(200, 7);
    CHECK_FALSE(importCtb(junk).ok());
    auto good = makeCtb("plot_style{\n}\n");
    for (std::size_t cut : {std::size_t{60}, good.size() - 3}) {
        std::vector<std::uint8_t> bad(good.begin(),
                                      good.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(importCtb(bad).ok());
    }
    // A table without colour styles (named-style .stb) says so.
    auto stb = importCtb(makeCtb("description=\"x\"\n"));
    REQUIRE_FALSE(stb.ok());
    CHECK(stb.error().code == ErrorCode::Unsupported);
    // Absurd declared size.
    auto huge = good;
    huge[sizeof("PIAFILEVERSION_2.0,CTBVER1,compress\r\npmzlibcodec") - 1 + 7] = 0x7f;
    CHECK_FALSE(importCtb(huge).ok());
}

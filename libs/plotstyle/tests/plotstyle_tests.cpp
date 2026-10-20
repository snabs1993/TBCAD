#include <doctest/doctest.h>
#include <miniz.h>

#include <cstring>

#include "tbcad/plotstyle/plotstyle.h"

using namespace tbcad;
using namespace tbcad::plotstyle;

namespace {

// A .ctb image written the way the format describes it: header, three words, zlib stream.
std::vector<std::uint8_t> makeCtb(const std::string& text, bool stb = false) {
    static const char header[] = "PIAFILEVERSION_2.0,CTBVER1,compress\r\npmzlibcodec";
    static const char stbHeader[] = "PIAFILEVERSION_2.0,STBVER1,compress\r\npmzlibcodec";
    if (stb)
        REQUIRE(sizeof header == sizeof stbHeader);
    const char* h = stb ? stbHeader : header;
    std::vector<std::uint8_t> out(h, h + sizeof header - 1);
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

TEST_CASE("palette colours map to indices - other colours do not") {
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
    // A named style with only a name (found by fuzzing: threw out_of_range).
    auto onlyName = fromText("TBCAD-PLOT-STYLE 1\nnamed 1\nS name=Thin\n");
    REQUIRE(onlyName.ok());
    REQUIRE(onlyName.value().styles.size() == 1);
    CHECK(onlyName.value().styles[0].name == "Thin");
}

TEST_CASE("ctbText returns the decompressed text of a .ctb file") {
    auto t = ctbText(makeCtb("plot_style{\n 0{\n }\n}\n"));
    REQUIRE(t.ok());
    CHECK(t.value() == "plot_style{\n 0{\n }\n}\n");
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
        "  lineweight=3\n"
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
        "  lineweight=2\n"
        " }\n"
        " 3{\n"
        "  name=\"Color_4\"\n"
        "  color=-1006632961\n"  // placeholder white: colour policy 0 keeps the object colour
        "  color_policy=0\n"
        "  screen=100\n"
        "  lineweight=0\n"
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
    CHECK_FALSE(t.entries[4].color);
}

// AutoCAD writes custom_lineweight_table after plot_style, as here.
// Pens count from 1 into the table: _EP-100's pens 5 / 12 / 13 / 18 plot 0.13 / 0.30 / 0.33 / 0.55
// mm in AutoCAD's PDF of the owner's drawing.
TEST_CASE(
    "AutoCAD's custom_lineweight_table gives each pen its width, 0 and 255 keep the object's") {
    const std::string text =
        "description=\"Color 7 = 0.35mm\"\n"
        "aci_table_available=TRUE\n"
        "scale_factor=1.0\n"
        "apply_factor=FALSE\n"
        "plot_style{\n"
        " 0{\n  name=\"Color_1\"\n  color=-1023410176\n  color_policy=4\n  lineweight=5\n }\n"
        " 1{\n  name=\"Color_2\"\n  color=-1023410176\n  color_policy=4\n  lineweight=13\n }\n"
        " 2{\n  name=\"Color_3\"\n  color=-1006632961\n  color_policy=0\n  lineweight=255\n }\n"
        " 3{\n  name=\"Color_4\"\n  color=-1023410176\n  color_policy=4\n  lineweight=18\n }\n"
        " 4{\n  name=\"Color_5\"\n  color=-1023410176\n  color_policy=4\n  lineweight=0\n }\n"
        "}\n"
        "custom_lineweight_table{\n"
        " 0=0.0\n 1=0.08\n 2=0.1\n 3=0.12\n 4=0.13\n 5=0.15\n 6=0.18\n 7=0.2\n 8=0.23\n"
        " 9=0.25\n 10=0.27\n 11=0.3\n 12=0.33\n 13=0.35\n 14=0.4\n 15=0.45\n 16=0.5\n"
        " 17=0.55\n 18=0.6\n"
        "}\n";
    auto r = importCtb(makeCtb(text));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const PlotStyleTable& t = r.value();
    CHECK(t.entries[1].lineweightMm.value() == doctest::Approx(0.13));
    CHECK(t.entries[2].lineweightMm.value() == doctest::Approx(0.33));
    CHECK_FALSE(t.entries[3].lineweightMm);
    CHECK(t.entries[4].lineweightMm.value() == doctest::Approx(0.55));
    CHECK_FALSE(t.entries[5].lineweightMm);
}

TEST_CASE("an .stb file imports as a named table that round-trips through text") {
    const std::string text =
        "description=\"Named\"\n"
        "lineweight_table{\n 0=0.0000\n 1=0.2500\n}\n"
        "plot_style{\n"
        " 0{\n  name=\"Normal\"\n  color=-1\n  screen=100\n  lineweight=0\n }\n"
        " 1{\n  name=\"Thin Red\"\n  color=-1023409921\n  screen=60\n  lineweight=2\n }\n"
        "}\n";
    auto r = importCtb(makeCtb(text, true));
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const PlotStyleTable& t = r.value();
    CHECK(t.named);
    REQUIRE(t.styles.size() == 2);
    const PlotStyleEntry* red = t.findNamed("thin red");
    REQUIRE(red);
    CHECK(red->screen == 60);
    CHECK(red->lineweightMm.value() == doctest::Approx(0.25));
    CHECK(t.findNamed("NORMAL") != nullptr);
    CHECK(t.findNamed("Nope") == nullptr);
    auto again = fromText(toText(t));
    REQUIRE(again.ok());
    CHECK(again.value().named);
    CHECK(again.value().styles == t.styles);
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

TEST_CASE("a table saved from an old CTB import loses its white placeholder pens") {
    auto r = fromText(
        "TBCAD-PLOT-STYLE 1\nname EP100\n"
        "251 color=#ffffff lineweight=object screen=100 gray=0 name=Color_252\n"
        "7 color=#000000 lineweight=0.25 screen=100 gray=0 name=Color_7\n"
        "9 color=#ffffff lineweight=object screen=100 gray=0\n");
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().entries[251].color);  // placeholder
    REQUIRE(r.value().entries[7].color);        // a real black pen stays
    REQUIRE(r.value().entries[9].color);        // a white pen in a table of our own stays
}

TEST_CASE("a dark grey true colour takes a pen of the colour range, not the grey ramp") {
    CHECK(penIndexOfTrueColor(paletteColor(253)) == 253);  // palette colours keep their own
    CHECK(penIndexOfTrueColor({97, 97, 97, 255}) < 250);   // a pen of the colour range
    CHECK(penIndexOfTrueColor({0, 0, 66, 255}) > 0);
}

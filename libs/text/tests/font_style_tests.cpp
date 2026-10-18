#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

namespace {

struct Reg {
    Reg() { registerDisplay(); }
};
const Reg kReg;

// A compiled shapes font with one glyph: 'L', 10 units high, advance 8.
std::vector<unsigned char> tinyShx() {
    std::vector<unsigned char> out;
    const std::string head = "AutoCAD-86 shapes 1.0\r\n\x1a";
    out.insert(out.end(), head.begin(), head.end());
    auto u16 = [&](unsigned x) {
        out.push_back(x & 255);
        out.push_back(x >> 8);
    };
    const std::vector<unsigned char> info = {'T', 0, 10, 3, 0, 0};
    const std::vector<unsigned char> el = {'L',  0, 2, 8, 0, 10, 1, 8, 0,
                                           0xF6, 8, 6, 0, 2, 8,  2, 0, 0};
    u16(0);
    u16(255);
    u16(2);
    u16(0);
    u16(info.size());
    u16('L');
    u16(el.size());
    out.insert(out.end(), info.begin(), info.end());
    out.insert(out.end(), el.begin(), el.end());
    return out;
}

double maxY(const DisplayList& l) {
    double m = -1e18;
    for (const auto& it : l.items)
        if (const auto* p = std::get_if<DisplayPolyline>(&it))
            for (const Vec2& v : p->pts)
                m = std::max(m, v.y);
    return m;
}

DisplayList build(const Document& doc, const Entity& e) {
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.01;
    DisplayList out;
    geom::buildDisplay(doc, e, ctx, out);
    return out;
}

}  // namespace

TEST_CASE("SHX font styles: found file is drawn, missing file is reported and replaced") {
    const auto dir = std::filesystem::temp_directory_path() / "tbcad_shx_dir";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "Tiny.SHX", std::ios::binary);
        const auto b = tinyShx();
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    Document doc;
    TextStyleId found, missing;
    {
        Transaction tx = doc.begin("styles");
        TextStyle a;
        a.name = "Tiny";
        a.shxFile = "tiny.shx";
        a.fontFamily = "TBCAD Stroke";
        a.widthFactor = 1.0 / 0.92;  // as read from a DXF
        found = tx.addTextStyle(a).value();
        TextStyle b;
        b.name = "Gone";
        b.shxFile = "nowhere.shx";
        b.fontFamily = "TBCAD Stroke";
        missing = tx.addTextStyle(b).value();
        tx.commit();
    }
    setFontSearchPaths({});
    CHECK(locateFontFile("tiny.shx").empty());
    auto before = missingFonts(doc);
    CHECK(before.size() == 2);

    setFontSearchPaths({dir});
    CHECK_FALSE(locateFontFile("tiny.shx").empty());
    auto after = missingFonts(doc);
    REQUIRE(after.size() == 1);
    CHECK(after[0].first == "Gone");
    CHECK(after[0].second.wanted == "nowhere.shx");
    CHECK(resolveStyleFont(*doc.textStyle(found)).kind == FontResolution::Kind::Shx);

    // The real font's L is 10 units high, so a 5 mm text is 5 mm high; its width factor is back
    // to 1.
    CHECK(effectiveWidthFactor(doc.textStyle(found)) == doctest::Approx(1.0));
    CHECK(effectiveWidthFactor(doc.textStyle(missing)) == doctest::Approx(1.0));
    Entity e;
    TextData t;
    t.utf8 = "L";
    t.height = 5;
    t.style = found;
    e.data = t;
    DisplayList l = build(doc, e);
    CHECK(maxY(l) == doctest::Approx(5.0).epsilon(0.02));

    setFontSearchPaths({});
    std::filesystem::remove_all(dir);
}

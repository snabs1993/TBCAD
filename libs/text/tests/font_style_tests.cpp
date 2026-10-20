#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

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
    auto u16 = [&](std::size_t x) {
        out.push_back(static_cast<unsigned char>(x & 255));
        out.push_back(static_cast<unsigned char>(x >> 8));
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

TEST_CASE("SHX font styles: 20,000 texts lay out without scanning the font folders each time") {
    const auto dir = std::filesystem::temp_directory_path() / "tbcad_shx_many";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (int i = 0; i < 3000; ++i)
        std::ofstream(dir / ("font" + std::to_string(i) + ".shx"), std::ios::binary) << "x";
    Document doc;
    TextStyleId style;
    {
        Transaction tx = doc.begin("style");
        TextStyle s;
        s.name = "Romans";
        s.shxFile = "romans.shx";  // not in the folder: the stroke font stands in
        s.fontFamily = "TBCAD Stroke";
        style = tx.addTextStyle(s).value();
        tx.commit();
    }
    setFontSearchPaths({dir});
    TextData t;
    t.utf8 = "12.5";
    t.height = 2;
    t.style = style;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20000; ++i)
        (void)layoutText(doc, t);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 3000);
    setFontSearchPaths({});
    std::filesystem::remove_all(dir);
}

TEST_CASE("a font on an unreachable file server costs no wait when the font folders have it") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tbcad_unc_font";
    fs::create_directories(dir);
    {
        const auto bytes = tinyShx();
        std::ofstream(dir / "tiny.shx", std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    std::atomic<int> asked{0};
    // The server never answers within the test: each question hangs for 3 s.
    setFontFileProbe([&asked](const fs::path&) {
        asked.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return true;
    });
    setFontSearchPaths({dir});
    auto timed = [](const std::string& name, std::vector<fs::path>& got) {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> lookups;
        for (std::size_t k = 0; k < got.size(); ++k)
            lookups.emplace_back([&got, &name, k] { got[k] = locateFontFile(name); });
        for (auto& t : lookups)
            t.join();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    // The folders have tiny.shx: the server is not asked.
    std::vector<fs::path> got(3);
    CHECK(timed("//gone-server/share/fonts/tiny.shx", got) < 0.2);
    for (const fs::path& p : got)
        CHECK(p.parent_path() == dir);
    CHECK(asked.load() == 0);

    // Nowhere but on the server: asked once, one short wait shared by the lookups at the same
    // time, none after it (was 2.7 s per lookup on the owner's PC).
    const std::string only = "//gone-server/share/fonts/only-there.shx";
    CHECK(timed(only, got) < 1.0);
    for (const fs::path& p : got)
        CHECK(p.empty());
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(locateFontFile(only).empty());
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.1);
    CHECK(asked.load() == 1);
    setFontFileProbe({});
    setFontSearchPaths({});
}

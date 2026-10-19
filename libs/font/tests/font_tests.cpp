#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <random>
#include <thread>

#include "tbcad/font/font_library.h"

using namespace tbcad::font;

namespace {

struct Fonts {
    FontLibrary lib;
    FontId sarabun, lato;
    Fonts() {
        auto s = lib.loadBundled("Sarabun");
        auto l = lib.loadBundled("Lato");
        REQUIRE_MESSAGE(s.ok(), "bundled Sarabun missing");
        REQUIRE_MESSAGE(l.ok(), "bundled Lato missing");
        sarabun = s.value();
        lato = l.value();
    }
};

double signedArea(const Contour& c) {
    double a = 0;
    for (std::size_t i = 0; i < c.points.size(); ++i) {
        const FPoint& p = c.points[i];
        const FPoint& q = c.points[(i + 1) % c.points.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}

std::vector<ShapedGlyph> flat(const std::vector<ShapedRun>& runs) {
    std::vector<ShapedGlyph> out;
    for (const auto& r : runs)
        out.insert(out.end(), r.glyphs.begin(), r.glyphs.end());
    return out;
}

}  // namespace

TEST_CASE("Latin text shapes to one glyph per letter with positive advances") {
    Fonts f;
    for (FontId id : {f.lato, f.sarabun}) {
        const auto runs = f.lib.shape(id, "Hello");
        REQUIRE(runs.size() == 1);
        REQUIRE(runs[0].glyphs.size() == 5);
        for (const auto& g : runs[0].glyphs)
            CHECK(g.xAdvance > 0);
        for (std::uint32_t i = 0; i < 5; ++i)
            CHECK(runs[0].glyphs[i].cluster == i);
    }
}

TEST_CASE("Shaping the same text twice gives the same runs (cached)") {
    Fonts f;
    auto a = f.lib.shape(f.sarabun, "Label 42 สวัสดี");
    auto b = f.lib.shape(f.sarabun, "Label 42 สวัสดี");
    auto c = f.lib.shape(f.lato, "Label 42 สวัสดี");
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].font == b[i].font);
        REQUIRE(a[i].glyphs.size() == b[i].glyphs.size());
        for (std::size_t g = 0; g < a[i].glyphs.size(); ++g) {
            CHECK(a[i].glyphs[g].glyphId == b[i].glyphs[g].glyphId);
            CHECK(a[i].glyphs[g].xAdvance == b[i].glyphs[g].xAdvance);
        }
    }
    // A different font is a different cache entry.
    CHECK(c.front().font == f.lato);
    CHECK(a.front().font == f.sarabun);
}

TEST_CASE("Thai tone mark and vowel stack on the base letter") {
    Fonts f;
    const auto upem = f.lib.metrics(f.sarabun).unitsPerEm;
    REQUIRE(upem == 1000);

    SUBCASE("sara am with tone mark (น้ำ)") {
        const auto runs = f.lib.shape(f.sarabun, "น้ำ");
        REQUIRE(runs.size() == 1);
        const auto& g = runs[0].glyphs;
        // Expected glyph ids for Sarabun-Regular.ttf as bundled: the font
        // decomposes sara am and substitutes a stacked tone mark glyph.
        REQUIRE(g.size() == 3);
        CHECK(g[0].glyphId == 462);
        CHECK(g[1].glyphId == 762);
        CHECK(g[2].glyphId == 488);
        // The mark takes no horizontal space and sits above the base letter.
        CHECK(g[0].xAdvance > 0);
        CHECK(g[1].xAdvance == 0);
        CHECK(g[2].xAdvance > 0);
        const auto& mark = f.lib.outline(f.sarabun, g[1].glyphId);
        REQUIRE(!mark.contours.empty());
        double minY = 1e9;
        for (const auto& c : mark.contours)
            for (const auto& p : c.points)
                minY = std::min(minY, p.y + g[1].yOffset / 1000.0);
        CHECK(minY > f.lib.metrics(f.sarabun).xHeight);
        // One grapheme cluster: all glyphs map back to byte 0.
        for (const auto& gl : g)
            CHECK(gl.cluster == 0);
    }

    SUBCASE("mai han-akat plus tone (ปั่น)") {
        const auto runs = f.lib.shape(f.sarabun, "ปั่น");
        REQUIRE(runs.size() == 1);
        const auto& g = runs[0].glyphs;
        REQUIRE(g.size() == 4);
        CHECK(g[0].glyphId == 464);
        CHECK(g[1].glyphId == 734);
        CHECK(g[2].glyphId == 736);
        CHECK(g[3].glyphId == 462);
        CHECK(g[1].xAdvance == 0);
        CHECK(g[2].xAdvance == 0);
        // The tone mark is positioned by GPOS: non-zero offsets.
        CHECK(g[2].xOffset == -163);
        CHECK(g[2].yOffset == -32);
        // Clusters: ป ั ่ share the first syllable, น starts at byte 9.
        CHECK(g[0].cluster == 0);
        CHECK(g[1].cluster == 0);
        CHECK(g[2].cluster == 0);
        CHECK(g[3].cluster == 9);
    }
}

TEST_CASE("Missing glyphs fall back to the Thai font within one call") {
    Fonts f;
    const auto runs = f.lib.shape(f.lato, "Room ห้องนอน");
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].font == f.lato);
    CHECK(runs[1].font == f.sarabun);
    CHECK(runs[0].glyphs.size() == 5);  // "Room "
    CHECK(runs[1].glyphs.size() > 5);
    // No .notdef glyphs anywhere.
    for (const auto& g : flat(runs))
        CHECK(g.glyphId != 0);
    // Cluster offsets are byte offsets into the whole string.
    CHECK(runs[1].glyphs.front().cluster == 5);
    CHECK_FALSE(f.lib.hasGlyph(f.lato, 0x0E01));
    CHECK(f.lib.hasGlyph(f.sarabun, 0x0E01));
}

TEST_CASE("Outline of O has two closed contours with opposite winding") {
    Fonts f;
    for (FontId id : {f.lato, f.sarabun}) {
        const auto runs = f.lib.shape(id, "O");
        REQUIRE(runs.size() == 1);
        const auto& o = f.lib.outline(id, runs[0].glyphs[0].glyphId);
        REQUIRE(o.contours.size() == 2);
        const double a0 = signedArea(o.contours[0]);
        const double a1 = signedArea(o.contours[1]);
        CHECK(a0 * a1 < 0);
        CHECK(o.contours[0].points.size() > 8);
        // Points are in em units.
        for (const auto& c : o.contours)
            for (const auto& p : c.points) {
                CHECK(std::abs(p.x) < 2.0);
                CHECK(std::abs(p.y) < 2.0);
            }
        // Cached: second call returns the same object.
        CHECK(&f.lib.outline(id, runs[0].glyphs[0].glyphId) == &o);
    }
}

TEST_CASE("Glyph bounds are the box of the outline, and a glyph with no outline has none") {
    Fonts f;
    for (FontId id : {f.lato, f.sarabun}) {
        const auto gid = f.lib.shape(id, "O")[0].glyphs[0].glyphId;
        const auto& o = f.lib.outline(id, gid);
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (const auto& c : o.contours)
            for (const auto& p : c.points) {
                x0 = std::min<double>(x0, p.x);
                y0 = std::min<double>(y0, p.y);
                x1 = std::max<double>(x1, p.x);
                y1 = std::max<double>(y1, p.y);
            }
        const auto b = f.lib.glyphBounds(id, gid);
        REQUIRE_FALSE(b.empty);
        CHECK(b.minX == x0);
        CHECK(b.minY == y0);
        CHECK(b.maxX == x1);
        CHECK(b.maxY == y1);
        const auto again = f.lib.glyphBounds(id, gid);  // from the cache
        CHECK(again.minX == b.minX);
        CHECK(again.maxY == b.maxY);
        const auto space = f.lib.shape(id, " ")[0].glyphs[0].glyphId;
        CHECK(f.lib.glyphBounds(id, space).empty);
    }
}

TEST_CASE("Flattening tolerance controls point count") {
    Fonts f;
    const auto gid = f.lib.shape(f.lato, "S")[0].glyphs[0].glyphId;
    const auto fine = f.lib.outline(f.lato, gid, 1.0 / 5000).contours[0].points.size();
    const auto coarse = f.lib.outline(f.lato, gid, 1.0 / 50).contours[0].points.size();
    CHECK(fine > coarse);
    CHECK(coarse >= 8);
}

TEST_CASE("Metrics") {
    Fonts f;
    for (FontId id : {f.lato, f.sarabun}) {
        const auto m = f.lib.metrics(id);
        CHECK(m.unitsPerEm > 0);
        CHECK(m.ascender > 0.5);
        CHECK(m.descender < 0);
        CHECK(m.capHeight > 0.5);
        CHECK(m.capHeight < 0.9);
        CHECK(m.xHeight > 0.3);
        CHECK(m.xHeight < m.capHeight);
    }
    CHECK(f.lib.familyName(f.sarabun) == "Sarabun");
    CHECK(f.lib.familyName(f.lato) == "Lato");
}

TEST_CASE("Loading is idempotent and errors are reported") {
    FontLibrary lib;
    auto a = lib.loadBundled("sarabun");
    auto b = lib.loadBundled("Sarabun");
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.value() == b.value());
    CHECK_FALSE(lib.loadBundled("NoSuchFont").ok());
    CHECK_FALSE(lib.load("/definitely/not/a/font.ttf").ok());
    CHECK_FALSE(lib.loadFromMemory({}).ok());
    CHECK_FALSE(lib.loadFromMemory({'n', 'o', 't', 'a', 'f', 'o', 'n', 't'}).ok());
    // Invalid ids are harmless.
    CHECK(lib.shape(FontId{999}, "x").empty());
    CHECK(lib.outline(FontId{999}, 1).contours.empty());
    CHECK_FALSE(lib.hasGlyph(FontId{999}, 'x'));
}

TEST_CASE("Invalid UTF-8 and odd input do not crash") {
    Fonts f;
    CHECK(f.lib.shape(f.lato, "").empty());
    for (const char* s :
         {"\xff\xfe", "ab\xe0\xb8", "\xf0\x9f\x98\x80", "\xc0\x80", "\xed\xa0\x80", "a\0b"}) {
        const auto runs =
            f.lib.shape(f.lato, std::string_view(s, std::char_traits<char>::length(s)));
        CHECK(!runs.empty());
    }
    CHECK(!f.lib.shape(f.lato, std::string_view("a\0b", 3)).empty());
    CHECK(!f.lib.shape(f.lato, "abc", Direction::RTL).empty());
}

TEST_CASE("findSystem is safe when nothing matches") {
    FontLibrary lib;
    CHECK_FALSE(lib.findSystem("Definitely Not A Font Family 12345", false, false).has_value());
}

TEST_CASE("Concurrent shaping and outlines") {
    Fonts f;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&f] {
            for (int i = 0; i < 50; ++i) {
                const auto runs = f.lib.shape(f.lato, "Room ห้องนอน น้ำ");
                for (const auto& r : runs)
                    for (const auto& g : r.glyphs)
                        (void)f.lib.outline(r.font, g.glyphId);
            }
        });
    }
    for (auto& th : threads)
        th.join();
}

TEST_CASE("Corrupt font data returns an error and never crashes (1000 mutations)") {
    const std::string path = std::string(TBCAD_FONT_TEST_ASSETS) + "/Sarabun-Regular.ttf";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    const std::vector<unsigned char> original((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
    REQUIRE(original.size() > 1000);

    std::mt19937 rng(12345);
    int loaded = 0, rejected = 0;
    for (int iter = 0; iter < 1000; ++iter) {
        std::vector<unsigned char> data = original;
        switch (iter % 4) {
            case 0:  // a few random bytes anywhere
                for (int k = 0; k < 1 + static_cast<int>(rng() % 16); ++k)
                    data[rng() % data.size()] = static_cast<unsigned char>(rng());
                break;
            case 1:  // damage the table directory / header region
                for (int k = 0; k < 1 + static_cast<int>(rng() % 8); ++k)
                    data[rng() % 512] = static_cast<unsigned char>(rng());
                break;
            case 2:  // truncate
                data.resize(1 + rng() % data.size());
                break;
            default:  // overwrite a run
            {
                const std::size_t at = rng() % data.size();
                const std::size_t n = std::min<std::size_t>(1 + rng() % 64, data.size() - at);
                for (std::size_t k = 0; k < n; ++k)
                    data[at + k] = static_cast<unsigned char>(rng());
            }
        }
        FontLibrary lib;
        auto r = lib.loadFromMemory(std::move(data));
        if (!r.ok()) {
            CHECK_FALSE(r.error().message.empty());
            ++rejected;
            continue;
        }
        ++loaded;
        // A mutated font that still loads must survive shaping and outlines.
        (void)lib.metrics(r.value());
        for (const auto& run : lib.shape(r.value(), "Hello Room น้ำ ปั่น"))
            for (const auto& g : run.glyphs)
                (void)lib.outline(r.value(), g.glyphId);
        for (std::uint32_t g = 0; g < 64; ++g)
            (void)lib.outline(r.value(), g);
    }
    MESSAGE("loaded " << loaded << ", rejected " << rejected);
    CHECK(rejected > 0);
}

// ---- single-stroke fonts (.tbsf) -------------------------------------------------------------

namespace {
const char* kTinyStroke =
    "tbsf 1\nfamily Test\nupm 10\ncap 7\nxheight 5\nascender 7\ndescender -2\ntonedrop 1\n"
    "# comment\nunknownkey 3\n"
    "glyph 41 6\ns 0 0 3 7 6 0\ns 1 3 5 3\n"
    "glyph 42 5\ns 0 0 0 7\n"
    "glyph E34 0 mark=upper\ns 0 8 2 9\n"
    "glyph E48 0 mark=tone\ns 0 8 1 9\n";
}

TEST_CASE("stroke font: parse, metrics and strokes are scaled to em units") {
    FontLibrary lib;
    auto r = lib.loadStrokeFromMemory(kTinyStroke);
    REQUIRE(r.ok());
    FontId f = r.value();
    CHECK(lib.isStroke(f));
    FontMetrics m = lib.metrics(f);
    CHECK(m.capHeight == doctest::Approx(0.7));
    CHECK(m.descender == doctest::Approx(-0.2));
    const StrokeGlyph& a = lib.strokes(f, 'A');
    REQUIRE(a.paths.size() == 2);
    CHECK(a.paths[0].points.size() == 3);
    CHECK(a.paths[0].points[1].x == doctest::Approx(0.3));
    CHECK(a.paths[0].points[1].y == doctest::Approx(0.7));
    CHECK(lib.strokes(f, 'Z').paths.empty());
    CHECK(lib.outline(f, 'A').contours.empty());
}

TEST_CASE("stroke font: malformed input reports an error") {
    FontLibrary lib;
    CHECK_FALSE(lib.loadStrokeFromMemory("").ok());
    CHECK_FALSE(lib.loadStrokeFromMemory("glyph 41 5\ns 0 0 1 1\n").ok());        // no header
    CHECK_FALSE(lib.loadStrokeFromMemory("tbsf 1\nupm 10\n").ok());               // no glyphs
    CHECK_FALSE(lib.loadStrokeFromMemory("tbsf 1\nglyph 41 5\ns 0 0 1\n").ok());  // odd coordinate
    CHECK_FALSE(lib.loadStrokeFromMemory("tbsf 1\nglyph zz 5\n").ok());
}

TEST_CASE("stroke font: shaping advances and centres marks over the base glyph") {
    FontLibrary lib;
    FontId f = lib.loadStrokeFromMemory(kTinyStroke).value();
    // U+0E01 is absent from the tiny font, so only A + upper vowel + tone are used.
    auto runs = lib.shape(f, "A\xE0\xB8\xB4\xE0\xB9\x88");
    auto g = flat(runs);
    REQUIRE(g.size() == 3);
    CHECK(g[0].glyphId == 'A');
    CHECK(g[0].xAdvance == doctest::Approx(0.6 * runs[0].unitsPerEm).epsilon(0.01));
    CHECK(g[1].xAdvance == 0);
    CHECK(g[1].xOffset == doctest::Approx(-0.3 * runs[0].unitsPerEm).epsilon(0.01));
    CHECK(g[2].xAdvance == 0);
    CHECK(g[2].yOffset == 0);
}

TEST_CASE("stroke font: a tone mark with no upper vowel under it drops down") {
    FontLibrary lib;
    FontId f = lib.loadStrokeFromMemory(kTinyStroke).value();
    auto g = flat(lib.shape(f, "A\xE0\xB9\x88"));  // A + mai ek
    REQUIRE(g.size() == 2);
    CHECK(g[1].glyphId == 0xE48);
    CHECK(g[1].yOffset < 0);
    auto h = flat(lib.shape(f, "A\xE0\xB8\xB4\xE0\xB9\x88"));
    CHECK(h[2].yOffset == 0);
}

TEST_CASE("bundled stroke font loads and covers ASCII") {
    FontLibrary lib;
    auto r = lib.loadBundledStroke();
    REQUIRE_MESSAGE(r.ok(), "bundled tbcad-stroke.tbsf missing");
    for (char32_t c = 0x21; c < 0x7F; ++c)
        CHECK_MESSAGE(lib.strokes(r.value(), c).paths.size() > 0, "missing U+" << (unsigned)c);
}

TEST_CASE("bundled stroke font covers the Thai block and places marks") {
    FontLibrary lib;
    FontId f = lib.loadBundledStroke().value();
    for (char32_t c = 0x0E01; c <= 0x0E2E; ++c)
        CHECK_MESSAGE(lib.hasGlyph(f, c), "missing consonant U+" << std::hex << (unsigned)c);
    for (char32_t c : {0xE2F, 0xE30, 0xE31, 0xE32, 0xE33, 0xE34, 0xE35, 0xE36, 0xE37, 0xE38,
                       0xE39, 0xE3A, 0xE3F, 0xE40, 0xE41, 0xE42, 0xE43, 0xE44, 0xE45, 0xE46,
                       0xE47, 0xE48, 0xE49, 0xE4A, 0xE4B, 0xE4C, 0xE4D, 0xE4E, 0xE50, 0xE59})
        CHECK_MESSAGE(lib.hasGlyph(f, c), "missing U+" << std::hex << (unsigned)c);
    // "ก้อง": the tone mark sits on the consonant, so it drops to the vowel-less height
    auto runs = lib.shape(f, "\xE0\xB8\x81\xE0\xB9\x89\xE0\xB8\xAD\xE0\xB8\x87");
    auto g = flat(runs);
    REQUIRE(g.size() == 4);
    CHECK(g[1].xAdvance == 0);
    CHECK(g[1].yOffset < 0);
    CHECK(g[2].xAdvance > 0);
    CHECK(runs.size() == 1);  // no fallback font was needed
}

namespace {
// A tiny compiled shapes font: 'L' (vectors), 'O' (octant circle), 'B' (bulge arc, sub-shape).
std::vector<unsigned char> tinyShx(bool unicode) {
    std::vector<unsigned char> out;
    auto put = [&](const std::string& s) { out.insert(out.end(), s.begin(), s.end()); };
    auto u16 = [&](std::vector<unsigned char>& v, unsigned x) {
        v.push_back(x & 255);
        v.push_back(x >> 8);
    };
    struct Shape {
        unsigned num;
        std::string name;
        std::vector<unsigned char> codes;
    };
    std::vector<Shape> shapes;
    shapes.push_back({0, "TINY", {10, 3, 0, 0}});
    shapes.push_back({'L', "L", {2, 8, 0, 10, 1, 8, 0, 0xF6 /*-10*/, 8, 6, 0, 2, 8, 2, 0, 0}});
    shapes.push_back({'O', "O", {2, 8, 5, 0, 1, 0xA, 5, 0x08, 2, 8, 5, 0, 0}});
    shapes.push_back({'B', "B", {7, 'L', 0}});
    if (unicode) {
        put("AutoCAD-86 unifont 1.0\r\n\x1a");
        std::vector<unsigned char> head = {'T', 'I', 'N', 'Y', 0, 10, 3, 0, 0, 0, 0};
        out.push_back(static_cast<unsigned char>(shapes.size() - 1));
        out.insert(out.end(), {0, 0, 0});
        u16(out, head.size());
        out.insert(out.end(), head.begin(), head.end());
        for (std::size_t i = 1; i < shapes.size(); ++i) {
            std::vector<unsigned char> def(shapes[i].name.begin(), shapes[i].name.end());
            def.push_back(0);
            std::vector<unsigned char> codes = shapes[i].codes;
            if (shapes[i].num == 'B')
                codes = {7, 'L', 0, 0};
            def.insert(def.end(), codes.begin(), codes.end());
            u16(out, shapes[i].num);
            u16(out, def.size());
            out.insert(out.end(), def.begin(), def.end());
        }
        return out;
    }
    put("AutoCAD-86 shapes 1.0\r\n\x1a");
    u16(out, 0);
    u16(out, 255);
    u16(out, shapes.size());
    std::vector<std::vector<unsigned char>> defs;
    for (const Shape& s : shapes) {
        std::vector<unsigned char> d(s.name.begin(), s.name.end());
        d.push_back(0);
        d.insert(d.end(), s.codes.begin(), s.codes.end());
        defs.push_back(d);
        u16(out, s.num);
        u16(out, d.size());
    }
    for (const auto& d : defs)
        out.insert(out.end(), d.begin(), d.end());
    return out;
}

FontId writeAndLoad(FontLibrary& lib, const std::vector<unsigned char>& bytes, const char* file) {
    const auto path = std::filesystem::temp_directory_path() / file;
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    auto r = lib.loadStroke(path);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return r.value();
}
}  // namespace

TEST_CASE("compiled SHX fonts (shapes and unifont) become stroke fonts") {
    for (bool uni : {false, true}) {
        CAPTURE(uni);
        FontLibrary lib;
        FontId f = writeAndLoad(lib, tinyShx(uni), uni ? "tbcad_tiny_uni.shx" : "tbcad_tiny.shx");
        CHECK(lib.isStroke(f));
        CHECK(lib.metrics(f).capHeight == doctest::Approx(1.0));
        REQUIRE(lib.hasGlyph(f, 'L'));
        const StrokeGlyph& l = lib.strokes(f, 'L');
        REQUIRE(l.paths.size() == 1);
        REQUIRE(l.paths[0].points.size() == 3);
        CHECK(l.paths[0].points[0].y == doctest::Approx(1.0));  // 10 units = 1 em
        CHECK(l.paths[0].points[1].y == doctest::Approx(0.0));
        CHECK(l.paths[0].points[2].x == doctest::Approx(0.6));
        auto runs = lib.shape(f, "L");
        REQUIRE(!runs.empty());
        CHECK(runs[0].glyphs[0].xAdvance ==
              doctest::Approx(0.8 * runs[0].unitsPerEm).epsilon(0.01));
        // The octant circle closes on itself and has radius 0.5 em.
        const StrokeGlyph& o = lib.strokes(f, 'O');
        REQUIRE(o.paths.size() == 1);
        const auto& pts = o.paths[0].points;
        CHECK(pts.front().x == doctest::Approx(pts.back().x).epsilon(1e-6));
        CHECK(pts.front().y == doctest::Approx(pts.back().y).epsilon(1e-6));
        CHECK(pts.size() > 16);
        // A sub-shape is run in place.
        CHECK(lib.strokes(f, 'B').paths.size() == 1);
    }
}

TEST_CASE("broken SHX data is refused, never crashes") {
    std::mt19937 rng(7);
    std::vector<unsigned char> good = tinyShx(false);
    for (int i = 0; i < 300; ++i) {
        std::vector<unsigned char> bad = good;
        for (int k = 0; k < 4; ++k)
            bad[24 + rng() % (bad.size() - 24)] = static_cast<unsigned char>(rng());
        if (i % 3 == 0)
            bad.resize(24 + rng() % (bad.size() - 24));
        FontLibrary lib;
        const auto path = std::filesystem::temp_directory_path() / "tbcad_bad.shx";
        {
            std::ofstream f(path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bad.data()),
                    static_cast<std::streamsize>(bad.size()));
        }
        (void)lib.loadStroke(path);
    }
}

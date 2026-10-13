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

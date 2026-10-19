#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/font/font.h"
#include "tbcad/font/result.h"

namespace tbcad::font {

// Default flattening tolerance for outline(): 1/2000 em.
inline constexpr double kDefaultFlattenTolerance = 1.0 / 2000.0;

// Owns loaded fonts. All members are thread-safe. FontIds stay valid, and
// references returned by outline() stay valid, for the life of the library.
class FontLibrary {
public:
    FontLibrary();
    ~FontLibrary();
    FontLibrary(const FontLibrary&) = delete;
    FontLibrary& operator=(const FontLibrary&) = delete;

    // Loads face 0 of a TrueType/OpenType file. Corrupt files give an error.
    Result<FontId> load(const std::filesystem::path& path);

    // Same, from bytes already in memory (the library keeps a copy).
    Result<FontId> loadFromMemory(std::vector<unsigned char> data, int faceIndex = 0);

    // Loads one of the fonts shipped in assets/fonts: "Sarabun" (Thai + Latin)
    // or "Lato" (Latin only). Loading the same name twice returns the same id.
    Result<FontId> loadBundled(std::string_view name);

    // Single-stroke fonts (TBCAD's own .tbsf format, see docs/font.md). A stroke font has FontId,
    // metrics and shaping like an outline font (glyph id = code point, no outlines), and its
    // glyphs come from strokes(). Characters it lacks fall back like for any other font.
    // Also reads compiled AutoCAD shape fonts (.shx: shapes 1.0/1.1 and unifont 1.0; big fonts are
    // not read). Shapes fonts numbered by Windows-874 (Thai) are recognised by a file name that
    // starts with "th" or contains "thai"; other fonts read codes above 0x9F as Latin-1.
    Result<FontId> loadStroke(const std::filesystem::path& path);
    Result<FontId> loadStrokeFromMemory(std::string_view tbsfText);
    // "tbcad-stroke" (Latin + Thai), looked up in <bundled directory>/stroke.
    Result<FontId> loadBundledStroke(std::string_view name = "tbcad-stroke");
    bool isStroke(FontId font);
    // Strokes of a glyph in em units, y up; empty for outline fonts and unknown glyphs. The
    // reference stays valid for the life of the library.
    const StrokeGlyph& strokes(FontId font, std::uint32_t glyphId);

    // Directory searched for bundled font files. Defaults to the source tree's
    // assets/fonts (set at build time), then ./assets/fonts. The installer sets
    // it to the shipped location.
    void setBundledDirectory(std::filesystem::path dir);
    std::filesystem::path bundledDirectory();

    // Finds an installed font by family name (case-insensitive) without any
    // Win32 font API: Windows scans C:\Windows\Fonts, Linux /usr/share/fonts.
    // Prefers an exact bold/italic match, else any face of that family.
    std::optional<FontId> findSystem(std::string_view family, bool bold, bool italic);

    // Shapes UTF-8 text. Characters the requested font lacks are taken from
    // the bundled Thai font, then the bundled Latin font, so the result can
    // contain several runs. Invalid UTF-8 is replaced by U+FFFD.
    std::vector<ShapedRun> shape(FontId font, std::string_view utf8,
                                 Direction direction = Direction::LTR);

    // Glyph contours flattened to polylines; cached per (font, glyph, tolerance).
    const GlyphOutline& outline(FontId font, std::uint32_t glyphId,
                                double toleranceEm = kDefaultFlattenTolerance);

    // Box around a glyph's outline (or strokes), em units, y up; `empty` for a glyph that draws
    // nothing. Cached in this library, so text laid out on several threads does not share one.
    struct GlyphBounds {
        double minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool empty = true;
    };
    GlyphBounds glyphBounds(FontId font, std::uint32_t glyphId);

    FontMetrics metrics(FontId font);

    // True if the font has a glyph for the code point.
    bool hasGlyph(FontId font, char32_t codePoint);

    // Family name as stored in the font ("Sarabun").
    std::string familyName(FontId font);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tbcad::font

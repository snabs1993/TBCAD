#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tbcad::font {

struct FPoint {
    double x, y;
};

struct FontId {
    std::uint32_t value = 0;
    friend bool operator==(FontId a, FontId b) { return a.value == b.value; }
    friend bool operator!=(FontId a, FontId b) { return a.value != b.value; }
};

enum class Direction { LTR, RTL };

// One glyph of a shaped run. Advances and offsets are in font units
// (ShapedRun::unitsPerEm per em); y grows upward. `cluster` is the byte offset
// into the UTF-8 input of the first character this glyph belongs to.
struct ShapedGlyph {
    std::uint32_t glyphId = 0;
    std::uint32_t cluster = 0;
    std::int32_t xAdvance = 0;
    std::int32_t yAdvance = 0;
    std::int32_t xOffset = 0;
    std::int32_t yOffset = 0;
};

// Consecutive glyphs drawn with one font. Text that needs fallback fonts comes
// back as several runs; concatenating their advances gives the whole line.
struct ShapedRun {
    FontId font;
    int unitsPerEm = 1000;
    std::vector<ShapedGlyph> glyphs;
};

// Closed contours (the glyph's outline), points in em units, y up.
struct Contour {
    std::vector<FPoint> points;
};
struct GlyphOutline {
    std::vector<Contour> contours;
};

// Vertical metrics in em units.
struct FontMetrics {
    int unitsPerEm = 1000;
    double ascender = 0.0;
    double descender = 0.0;  // negative below the baseline
    double capHeight = 0.7;
    double xHeight = 0.5;
};

}  // namespace tbcad::font

template <>
struct std::hash<tbcad::font::FontId> {
    std::size_t operator()(tbcad::font::FontId id) const noexcept { return id.value; }
};

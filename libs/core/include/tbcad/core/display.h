#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "tbcad/core/color.h"
#include "tbcad/core/ids.h"
#include "tbcad/core/math.h"

namespace tbcad {

// A fill in this colour is drawn in the background colour of whatever shows it (screen theme or
// white paper): what WIPEOUT uses to hide the objects behind it.
inline constexpr Rgba kBackgroundColor{0, 0, 0, 1};
inline bool isBackgroundColor(Rgba c) {
    return c.a == 1 && c.r == 0 && c.g == 0 && c.b == 0;
}

struct LinetypeMark;

// Fully resolved drawing style (ByLayer/ByBlock already applied by whoever builds the list).
struct DisplayStyle {
    Rgba color{};
    double lineweight = 0;  // mm; 0 = thinnest line
    // Dash pattern in mm (+ dash, - gap, 0 dot); null or empty = continuous. Points into the
    // document's linetype table and is valid while that linetype exists.
    const std::vector<double>* pattern = nullptr;
    double patternScale = 1;
    // Text and shapes the linetype draws along the line (null = none). Drawn by buildDisplay, not
    // by the renderer; points into the linetype table like `pattern`.
    const std::vector<LinetypeMark>* marks = nullptr;
    LayerId layer{};
    bool noPlot = false;  // shown on screen only (a wipeout frame with WIPEOUTFRAME 2)
};

struct DisplayPolyline {
    std::vector<Vec2> pts{};
    bool closed = false;
    DisplayStyle style{};
};

// The character an outline-font glyph fill draws, so a PDF can write it as text.
struct GlyphRef {
    // The outline font file (.ttf / .ttc) the glyph comes from. The font library owns the bytes
    // and keeps them for the life of the process.
    const unsigned char* fontData = nullptr;
    std::size_t fontSize = 0;
    std::uint32_t faceIndex = 0;
    std::uint32_t glyphId = 0;
    std::u32string text{};   // the character(s) it stands for; empty after a cluster's first glyph
    Transform2 emToWorld{};  // the glyph's em square (1 = one em, y up) to drawing space
};

// Even-odd filled area: outer ring(s) plus holes.
struct DisplayFill {
    std::vector<std::vector<Vec2>> rings{};
    DisplayStyle style{};
    bool glyph = false;  // the outline of a text character (plots without an edge stroke)
    // For a glyph of an outline font: which one, placed where (kept through block transforms,
    // dropped where the outline is cut or bent).
    std::optional<GlyphRef> glyphRef{};
};

struct DisplayPoints {
    std::vector<Vec2> pts{};
    DisplayStyle style{};
};

using DisplayItem = std::variant<DisplayPolyline, DisplayFill, DisplayPoints>;

struct DisplayList {
    std::vector<DisplayItem> items{};
};

}  // namespace tbcad

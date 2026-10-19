#pragma once

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

// Even-odd filled area: outer ring(s) plus holes.
struct DisplayFill {
    std::vector<std::vector<Vec2>> rings{};
    DisplayStyle style{};
    bool glyph = false;  // the outline of a text character (plots without an edge stroke)
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

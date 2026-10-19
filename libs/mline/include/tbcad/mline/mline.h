// Multilines: several parallel lines drawn along one path. A style (MLSTYLE) lists the offsets of
// the lines from the path, in drawing units for scale 1, positive to the left of the path
// direction. Styles are kept in the drawing's header settings as "mlstyle.<NAME>".
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::mline {

struct Style {
    std::string name = "STANDARD";
    std::string description{};
    std::vector<double> offsets{0.5, -0.5};  // one line each, any order
    bool startCap = false;                   // a line across the first end
    bool endCap = false;                     // a line across the last end
    // Per line, in the order of `offsets`; shorter or empty = every line takes the multiline's.
    std::vector<int> colors{};             // palette index; 256 = as the multiline, 0 = ByBlock
    std::vector<std::string> linetypes{};  // linetype name; empty = as the multiline
    int fill = -1;        // palette index of the fill between the outer lines; -1 none
    bool joints = false;  // a line across each corner
    int colorOf(std::size_t line) const { return line < colors.size() ? colors[line] : 256; }
    const std::string& linetypeOf(std::size_t line) const {
        static const std::string none;
        return line < linetypes.size() ? linetypes[line] : none;
    }
};

// The style every drawing has: two lines, 1 unit apart, no caps.
Style standardStyle();
std::string encode(const Style& style);
std::optional<Style> decode(const std::string& name, const std::string& text);

// A style by name (case-insensitive); STANDARD is built in and can be overridden by a stored one.
std::optional<Style> findStyle(const Document& doc, const std::string& name);
std::vector<std::string> styleNames(const Document& doc);
// Stores or replaces the style in a header (use inside Transaction::modifyHeader).
void storeStyle(Header& header, const Style& style);
bool removeStyle(Header& header, const std::string& name);

// The polylines a multiline is made of: one per style line (closed when the multiline is
// closed), then the end caps. Corners are mitred. An unknown style falls back to STANDARD.
struct Strokes {
    std::vector<std::vector<Vec2>> lines{};
    std::vector<bool> closed{};
    std::vector<int> element{};  // per line: the style line it belongs to, -1 for caps and joints
    std::vector<std::vector<Vec2>> fill{};  // rings of the style's fill (even-odd), if any
};
Strokes strokes(const Document& doc, const MlineData& d);
Strokes strokes(const MlineData& d, const Style& style);

// Plain polylines in place of the multiline, with its layer, colour and so on.
Result<std::vector<Entity>> explodeMline(const Document& doc, const Entity& e);

// Text form kept in DXF extended data so a multiline survives other programs.
std::string serialize(const MlineData& d, const Style& style);
struct Parsed {
    MlineData data;
    Style style;
};
std::optional<Parsed> parse(const std::string& text);

// Registers the display and bounds of EntityKind::MLine.
void registerDisplay();

}  // namespace tbcad::mline

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::gdt {

// The geometric characteristics of ISO 1101 / ASME Y14.5.
enum class Symbol {
    Straightness,
    Flatness,
    Circularity,
    Cylindricity,
    ProfileLine,
    ProfileSurface,
    Angularity,
    Perpendicularity,
    Parallelism,
    Position,
    Concentricity,
    Symmetry,
    CircularRunout,
    TotalRunout
};
inline constexpr int kSymbolCount = 14;
const char* symbolName(Symbol s);  // "Position", "Flatness" ...
std::optional<Symbol> symbolByName(std::string_view name);

struct Spec {
    Symbol symbol = Symbol::Position;
    std::string value{};  // "0.05"
    bool diameterZone = false;
    char material = 0;                  // 0 none, 'M' maximum, 'L' least material condition
    std::vector<std::string> datums{};  // up to three datum letters
    bool operator==(const Spec&) const = default;
};

// Frame size at paper scale: cells are kCellHeight high and the text kTextHeight.
inline constexpr double kCellHeight = 5.0;
inline constexpr double kTextHeight = 2.5;

// The frame, drawn at paper size with its left edge middle at the origin: the outline and
// dividers, the symbol drawn as lines, and the texts. Pure data (no font needed for the symbols).
std::vector<Entity> frameEntities(const Spec& spec);

// ---- the table of frames in the header --------------------------------------------------------
// A frame is an ordinary block insert; this table (header entry "tbcad.tolerances") says which
// blocks are frames and what they say, so DXF can write them as TOLERANCE entities.
using FrameTable = std::map<std::string, Spec>;
FrameTable readFrames(const Header& header);
void writeFrames(Header& header, const FrameTable& table);
Result<void> addFrame(Transaction& tx, const std::string& blockName, const Spec& spec);

// ---- DXF TOLERANCE text ------------------------------------------------------------------------
// "{\Fgdt;j}%%v{\Fgdt;n}0.05{\Fgdt;m}%%vA%%v..." : cells separated by %%v, symbols from the gdt
// font.
std::string dxfText(const Spec& spec);
std::optional<Spec> parseDxfText(std::string_view text);

}  // namespace tbcad::gdt

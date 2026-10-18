#pragma once

#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::symbolscmd {

// The symbols of the library. Each is drawn at paper size (millimetres on the sheet, insertion
// point at its centre or anchor) and inserted as an annotative block, so it follows the
// annotation scale like text does.
enum class Symbol {
    NorthArrow,
    GridBubble,
    LevelMark,
    SectionMarker,
    ElevationMarker,
    DetailMarker
};

struct SymbolInfo {
    Symbol symbol{};
    const char* command;    // "NORTHARROW"
    const char* alias;      // "NA"
    const char* blockName;  // "TB_NORTH"
    const char* title;      // shown in the menu
};
const std::vector<SymbolInfo>& symbolList();

// The entities of the symbol's block (in block coordinates, on layer 0) and the attributes it
// asks for. Pure data, so tests can look at it.
std::vector<Entity> symbolEntities(Symbol s);

// Makes sure the document has the symbol's block, adding it to `tx` when missing. Returns its id.
Result<BlockId> ensureSymbolBlock(Transaction& tx, const Document& doc, Symbol s);

// ---- feature control frames (TOLERANCE) ---------------------------------------------------------

// The geometric characteristics of ISO 1101 / ASME Y14.5.
enum class GdtSymbol {
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
const char* gdtName(GdtSymbol s);  // "Position", "Flatness" ...

struct ToleranceSpec {
    GdtSymbol symbol = GdtSymbol::Position;
    std::string value{};  // "0.05"
    bool diameterZone = false;
    char material = 0;                  // 0 none, 'M' maximum, 'L' least material condition
    std::vector<std::string> datums{};  // up to three datum letters
};

// The frame, drawn at paper size (text 2.5 mm, cells 5 mm high) with its left edge middle at the
// origin: the outline and dividers, the symbol drawn as lines, and the texts. Pure data.
std::vector<Entity> toleranceEntities(const ToleranceSpec& spec);

void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::symbolscmd

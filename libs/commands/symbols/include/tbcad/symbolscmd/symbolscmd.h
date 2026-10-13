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

void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::symbolscmd

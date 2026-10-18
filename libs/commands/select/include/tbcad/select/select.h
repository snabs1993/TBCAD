#pragma once

#include <optional>
#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"

namespace tbcad::select {

// PURGE, QSELECT [QSEL], SELECTSIMILAR [SELSIM], ISOLATE, HIDE and UNISOLATE. Called by the
// generated tbcad::registerAllCommands (see module.cmake).
void registerCommands(cmd::Registry& registry);

// ---- Pure helpers (used by the commands and tested directly) ----------------------------------

enum class PurgeKind : unsigned {
    Layers = 1,
    Blocks = 2,
    TextStyles = 4,
    Linetypes = 8,
    DimStyles = 16,
    All = 31,
};

struct PurgeCounts {
    int layers = 0, blocks = 0, textStyles = 0, linetypes = 0, dimStyles = 0;
    int total() const { return layers + blocks + textStyles + linetypes + dimStyles; }
};

// Erases the unused rows of the chosen tables, repeating until nothing more can go (a block's
// contents or a sub-layer may be the last user of another row). Built-in rows, the current layer
// and styles, and anonymous blocks stay.
PurgeCounts purgeUnused(const Document& doc, Transaction& tx,
                        unsigned kinds = static_cast<unsigned>(PurgeKind::All));

enum class Property {
    Type,
    Layer,
    Color,
    Linetype,
    Lineweight,
    Level,
    Transparency,  // 0..90 percent, ByLayer counts as 0
    Pattern,       // hatch pattern name ("SOLID" for a solid fill)
    Height,        // text and MText height
    Radius,        // circles and arcs
    Length         // length of the curve (circumference for circles)
};

// How a numeric property (Transparency, Height, Radius, Length) is compared with the value.
enum class Compare { Equals, Greater, Less };

struct Query {
    Property property = Property::Type;
    std::string value{};
    bool negate = false;
    Compare compare = Compare::Equals;  // Greater and Less only apply to numeric properties
};

// Visible model-space objects whose property equals (or, with `negate`, differs from) the value.
// Type takes a kind name (LINE, CIRCLE, POLYLINE, ...), Layer and Linetype a name, Color an index
// 1..255, "r,g,b", ByLayer or ByBlock, Lineweight a width in mm, ByLayer or Default. Colour,
// linetype and lineweight compare what is drawn, so ByLayer objects match their layer's value.
// Empty (nullopt) when the value cannot be read for that property.
std::optional<std::vector<EntityId>> queryEntities(const Document& doc, const Query& query);

// Objects that satisfy every query (AND). Empty (nullopt) when any value cannot be read.
std::optional<std::vector<EntityId>> filterEntities(const Document& doc,
                                                    const std::vector<Query>& queries);

// Named filters (FILTER Save / Use), stored in the file as Header::custom "filter.<name>".
std::string encodeFilter(const std::vector<Query>& queries);
std::vector<Query> decodeFilter(const std::string& text);
std::vector<std::string> savedFilters(const Document& doc);

enum class Match : unsigned {
    Type = 1,
    Layer = 2,
    Color = 4,
    Linetype = 8,
    Lineweight = 16,
    Level = 32,
    Transparency = 64,
    Style = 128,  // text style, dimension style or hatch pattern
    Size = 256    // text height or circle and arc radius
};

// Visible model-space objects that agree with `reference` on every property in `match` (the
// reference itself is included).
std::vector<EntityId> similarEntities(const Document& doc, const Entity& reference, unsigned match);

// Objects hidden by HIDE / ISOLATE, remembered in the file so UNISOLATE works after a reload.
std::vector<EntityId> hiddenByIsolate(const Document& doc);

}  // namespace tbcad::select

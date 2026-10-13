// What the object decoder produces: tables and entities with their references still as
// handles, plus the DXF-equivalent data groups of each entity.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace tbcad::dwg::detail {

struct Value {
    std::variant<long long, double, std::string> v{0LL};
    Value() = default;
    Value(int x) : v(static_cast<long long>(x)) {}
    Value(long long x) : v(x) {}
    Value(double x) : v(x) {}
    Value(std::string x) : v(std::move(x)) {}
    Value(const char* x) : v(std::string(x)) {}
};

struct Group {
    int code = 0;
    Value value{};
};

struct LayerRec {
    std::uint64_t handle = 0;
    std::string name{};
    int flags = 0;  // DXF 70
    int color = 7;  // ACI; negative = off
    bool frozen = false;
    bool locked = false;
    bool plot = true;
    int lineweight = -3;
    std::uint64_t linetype = 0;
};

struct LinetypeRec {
    std::uint64_t handle = 0;
    std::string name{};
    std::string description{};
    double patternLength = 0;
    std::vector<double> dashes{};
};

struct StyleRec {
    std::uint64_t handle = 0;
    std::string name{};
    int flags = 0;
    double height = 0, width = 1, oblique = 0, lastHeight = 0;
    int generation = 0;
    std::string font{}, bigFont{};
};

struct DimStyleRec {
    std::uint64_t handle = 0;
    std::string name{};
    int flags = 0;
    std::vector<Group> groups{};
};

struct BlockRec {
    std::uint64_t handle = 0;
    std::string name{};
    double baseX = 0, baseY = 0, baseZ = 0;
    int flags = 0;  // DXF 70 of the BLOCK record
    std::string xrefPath{};
};

// One entity: DXF type name, common properties and data groups (without layer/linetype/colour,
// which come from the common fields).
struct EntityRec {
    std::uint64_t handle = 0;
    std::string dxfType{};
    int entmode = 0;
    std::uint64_t owner = 0;  // owner block header when entmode == 0
    std::uint64_t layer = 0;
    std::uint64_t linetype = 0;  // explicit linetype handle (0 = by layer)
    int ltFlags = 0;
    std::uint64_t style = 0;        // text style handle for TEXT-like entities
    std::uint64_t blockHeader = 0;  // INSERT: the inserted block's header; DIMENSION: its block
    std::uint64_t dimStyle = 0;     // DIMENSION and LEADER
    // POLYLINE (vertices) and INSERT (attributes): the first and last owned object handle.
    std::uint64_t firstChild = 0, lastChild = 0;
    bool hasChildren = false;  // INSERT: the attribute bit was set
    int color = 256;           // ACI, 256 = by layer
    double ltScale = 1;
    bool invisible = false;
    int lineweight = 29;  // DWG lineweight code (29 = by layer)
    std::vector<Group> groups{};
};

struct Drawing {
    int codepage = 0;
    bool unitsKnown = false;
    int insunits = 0;
    double ltscale = 1;
    std::map<std::uint64_t, LayerRec> layers{};
    std::map<std::uint64_t, LinetypeRec> linetypes{};
    std::map<std::uint64_t, StyleRec> styles{};
    std::map<std::uint64_t, DimStyleRec> dimStyles{};
    std::map<std::uint64_t, BlockRec> blocks{};
    std::vector<EntityRec> entities{};  // in handle order
    std::size_t skipped = 0;            // objects of types not decoded
    std::map<std::string, std::size_t> skippedTypes{};
};

}  // namespace tbcad::dwg::detail

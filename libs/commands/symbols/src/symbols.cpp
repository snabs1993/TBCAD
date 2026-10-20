#include <cmath>

#include "tbcad/symbolscmd/symbolscmd.h"

namespace tbcad::symbolscmd {

namespace {

Entity line(Vec2 a, Vec2 b) {
    Entity e;
    e.data = LineData{a, b};
    return e;
}

Entity circle(Vec2 c, double r) {
    Entity e;
    e.data = CircleData{c, r};
    return e;
}

Entity poly(std::vector<Vec2> pts, bool closed) {
    PolylineData p;
    p.closed = closed;
    for (Vec2 v : pts)
        p.v.push_back(PolyVertex{.p = v});
    Entity e;
    e.data = std::move(p);
    return e;
}

Entity label(Vec2 at, double height, std::string text, HAlign h, VAlign v) {
    TextData t;
    t.position = t.alignPoint = at;
    t.height = height;
    t.utf8 = std::move(text);
    t.h = h;
    t.v = v;
    Entity e;
    e.data = Indirect<TextData>(std::move(t));
    return e;
}

Entity attribute(Vec2 at, double height, const char* tag, const char* prompt, const char* value,
                 HAlign h, VAlign v) {
    TextData t;
    t.position = t.alignPoint = at;
    t.height = height;
    t.utf8 = tag;  // a definition shows its tag, as ATTDEF and a DXF give it
    t.h = h;
    t.v = v;
    AttDefInfo a;
    a.tag = tag;
    a.prompt = prompt;
    a.defaultValue = value;
    t.attdef = a;
    Entity e;
    e.data = Indirect<TextData>(std::move(t));
    return e;
}

}  // namespace

const std::vector<SymbolInfo>& symbolList() {
    static const std::vector<SymbolInfo> list = {
        {Symbol::NorthArrow, "NORTHARROW", "NA", "TB_NORTH", "North Arrow"},
        {Symbol::GridBubble, "GRIDBUBBLE", "GB", "TB_GRID", "Grid Bubble"},
        {Symbol::LevelMark, "LEVELMARK", "LM", "TB_LEVEL", "Level Mark"},
        {Symbol::SectionMarker, "SECTIONMARK", "SM", "TB_SECTION", "Section Marker"},
        {Symbol::ElevationMarker, "ELEVATIONMARK", "EM", "TB_ELEVATION", "Elevation Marker"},
        {Symbol::DetailMarker, "DETAILMARK", "DM", "TB_DETAIL", "Detail Marker"},
    };
    return list;
}

// Sizes follow common Thai / ISO 128 drafting practice on the sheet: bubbles 8-10 mm across with
// 3.5 mm figures, markers 10 mm, notes 2.5 mm.
std::vector<Entity> symbolEntities(Symbol s) {
    std::vector<Entity> out;
    switch (s) {
        case Symbol::NorthArrow:
            out.push_back(circle({0, 0}, 6));
            out.push_back(poly({{0, 6}, {-2.2, -4.5}, {0, -2.5}, {2.2, -4.5}}, true));
            out.push_back(label({0, 8}, 3.5, "N", HAlign::Center, VAlign::Bottom));
            break;
        case Symbol::GridBubble:
            out.push_back(circle({0, 0}, 4));
            out.push_back(
                attribute({0, 0}, 3.5, "ID", "Grid label", "A", HAlign::Center, VAlign::Middle));
            break;
        case Symbol::LevelMark:
            out.push_back(poly({{-2, 3}, {2, 3}, {0, 0}}, true));
            out.push_back(line({-4, 0}, {14, 0}));
            out.push_back(
                attribute({4, 1}, 2.5, "LEVEL", "Level", "+0.00", HAlign::Left, VAlign::Bottom));
            break;
        case Symbol::SectionMarker:
            out.push_back(circle({0, 0}, 5));
            out.push_back(line({-5, 0}, {5, 0}));
            out.push_back(line({0, 5}, {0, 12}));
            out.push_back(attribute({0, 2.5}, 3.0, "SECTION", "Section mark", "A", HAlign::Center,
                                    VAlign::Middle));
            out.push_back(
                attribute({0, -2.5}, 2.5, "SHEET", "Sheet", "-", HAlign::Center, VAlign::Middle));
            break;
        case Symbol::ElevationMarker:
            out.push_back(circle({0, 0}, 5));
            out.push_back(line({0, 5}, {0, -5}));
            out.push_back(poly({{5, 0}, {9, 2.5}, {9, -2.5}}, true));
            out.push_back(attribute({-2.5, 0}, 3.0, "VIEW", "View number", "1", HAlign::Center,
                                    VAlign::Middle));
            out.push_back(
                attribute({2.5, 0}, 2.5, "SHEET", "Sheet", "-", HAlign::Center, VAlign::Middle));
            break;
        case Symbol::DetailMarker:
            out.push_back(circle({0, 0}, 5));
            out.push_back(line({-5, 0}, {5, 0}));
            out.push_back(attribute({0, 2.5}, 3.0, "DETAIL", "Detail number", "1", HAlign::Center,
                                    VAlign::Middle));
            out.push_back(
                attribute({0, -2.5}, 2.5, "SHEET", "Sheet", "-", HAlign::Center, VAlign::Middle));
            break;
    }
    return out;
}

Result<BlockId> ensureSymbolBlock(Transaction& tx, const Document& doc, Symbol s) {
    const SymbolInfo* info = nullptr;
    for (const SymbolInfo& i : symbolList())
        if (i.symbol == s)
            info = &i;
    if (!info)
        return makeError(ErrorCode::InvalidArgument, "unknown symbol");
    if (const BlockDef* have = doc.blockByName(info->blockName))
        return have->id;
    BlockDef def;
    def.name = info->blockName;
    auto id = tx.addBlock(std::move(def));
    if (!id)
        return id.error();
    for (Entity& e : symbolEntities(s)) {
        e.common.layer = kLayerZero;
        auto added = tx.add(std::move(e), id.value());
        if (!added)
            return added.error();
    }
    return id.value();
}

}  // namespace tbcad::symbolscmd

#include "tbcad/ui/customize.h"

#include <algorithm>
#include <cctype>

namespace tbcad::ui {

const std::vector<DoubleClickType>& doubleClickTypes() {
    static const std::vector<DoubleClickType> types = {
        {"Line", "Line", kOpenProperties},
        {"Circle", "Circle", kOpenProperties},
        {"Arc", "Arc", kOpenProperties},
        {"Ellipse", "Ellipse", kOpenProperties},
        {"Point", "Point", kOpenProperties},
        {"Ray", "Ray and construction line", kOpenProperties},
        {"Polyline", "Polyline", "PEDIT"},
        {"Spline", "Spline", "SPLINEDIT"},
        {"MLine", "Multiline", "MLEDIT"},
        {"Text", "Text", "TEXTEDIT"},
        {"MText", "Multiline text", "TEXTEDIT"},
        {"Hatch", "Hatch", "HATCHEDIT"},
        {"Block", "Block reference", "BEDIT"},
        {"AttributeBlock", "Block with attributes", "EATTEDIT"},
        {"Xref", "External reference", kOpenProperties},
        {"Dimension", "Dimension", "TEXTEDIT"},
        {"Leader", "Leader", kOpenProperties},
        {"MLeader", "Multileader", "TEXTEDIT"},
        {"Table", "Table", "TABLEEDIT"},
        {"Other", "Other objects", kOpenProperties},
    };
    return types;
}

std::string doubleClickKey(const Document& doc, const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
            return "Line";
        case EntityKind::Circle:
            return "Circle";
        case EntityKind::Arc:
            return "Arc";
        case EntityKind::Ellipse:
            return "Ellipse";
        case EntityKind::Point:
            return "Point";
        case EntityKind::Ray:
            return "Ray";
        case EntityKind::Polyline:
            return "Polyline";
        case EntityKind::Spline:
            return "Spline";
        case EntityKind::MLine:
            return "MLine";
        case EntityKind::Text:
            return "Text";
        case EntityKind::MText:
            return "MText";
        case EntityKind::Hatch:
            return "Hatch";
        case EntityKind::Insert: {
            const auto* ins = entityAs<InsertData>(e);
            const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
            if (def && !def->xrefPath.empty())
                return "Xref";
            return ins && !ins->attribs.empty() ? "AttributeBlock" : "Block";
        }
        case EntityKind::Dimension:
            return "Dimension";
        case EntityKind::Leader:
            return "Leader";
        case EntityKind::MLeader:
            return "MLeader";
        case EntityKind::Table:
            return "Table";
        case EntityKind::Proxy:
            break;
    }
    return "Other";
}

std::string doubleClickAction(const platform::Settings& s, const Document& doc, const Entity& e) {
    const std::string key = doubleClickKey(doc, e);
    const std::string mine = s.getString("dblclick." + key);
    if (!mine.empty())
        return mine;
    for (const DoubleClickType& t : doubleClickTypes())
        if (key == t.key)
            return t.defaultAction;
    return kOpenProperties;
}

std::string commandLabel(const std::string& line) {
    std::size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos)
        return {};
    std::size_t b = line.find_first_of(" \t", a);
    std::string w = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
    std::transform(w.begin(), w.end(), w.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return w;
}

}  // namespace tbcad::ui

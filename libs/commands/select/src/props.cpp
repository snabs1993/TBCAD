#include "common.h"

namespace tbcad::select::detail {

Color resolvedColor(const Document& doc, const Entity& e) {
    if (e.common.color.kind == ColorKind::ByLayer || e.common.color.kind == ColorKind::ByParent)
        return doc.effective(e.common.layer).color;
    return e.common.color;
}

LinetypeId resolvedLinetype(const Document& doc, const Entity& e) {
    if (e.common.linetype == kLinetypeByLayer || e.common.linetype == kLinetypeByParent)
        return doc.effective(e.common.layer).linetype;
    return e.common.linetype;
}

LineWeight resolvedLineweight(const Document& doc, const Entity& e) {
    const LineWeight w = e.common.lineweight;
    if (w.isByLayer() || w.isByParent())
        return doc.effective(e.common.layer).lineweight;
    return w;
}

std::string kindName(const Entity& e) {
    static const char* const kNames[] = {"LINE",    "CIRCLE",    "ARC",    "POLYLINE", "POINT",
                                         "ELLIPSE", "SPLINE",    "TEXT",   "MTEXT",    "HATCH",
                                         "INSERT",  "DIMENSION", "LEADER", "PROXY"};
    return kNames[static_cast<std::size_t>(kind(e))];
}

std::vector<const Entity*> shownEntities(const Document& doc) {
    std::vector<const Entity*> out;
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (e && e->common.visible && doc.effective(e->common).live())
            out.push_back(e);
    }
    return out;
}

std::vector<const Entity*> shownOnAnyLevel(const Document& doc) {
    std::vector<const Entity*> out;
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (e && e->common.visible && doc.effective(e->common).visible())
            out.push_back(e);
    }
    return out;
}

}  // namespace tbcad::select::detail

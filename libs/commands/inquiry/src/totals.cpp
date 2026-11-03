// Live selection totals (Q227): how many objects, their total length and the total area of the
// closed ones, for the Properties panel and the TOTALS command.

#include <cmath>
#include <cstdio>

#include "tbcad/geom/geom.h"
#include "tbcad/hatch/fill.h"
#include "tbcad/inquiry/inquiry.h"

namespace tbcad::inquiry {

namespace {

// Area of a shape that encloses one: circles, full ellipses, closed polylines and splines, and
// hatches. An open polyline is a run of lines here, not a shape (AREA closes it, totals do not).
std::optional<double> closedArea(const Entity& e) {
    if (const auto* h = entityAs<HatchData>(e))
        return hatch::hatchArea(*h);
    if (const auto* pl = entityAs<PolylineData>(e)) {
        if (!pl->closed || pl->v.size() < 3)
            return std::nullopt;
    }
    if (auto r = areaOfEntity(e))
        return r->area;
    return std::nullopt;
}

}  // namespace

SelectionTotals selectionTotals(const Document& doc, const std::vector<EntityId>& ids,
                                std::optional<EntityKind> only) {
    SelectionTotals t;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e || (only && kind(*e) != *only))
            continue;
        ++t.count;
        const double len = kind(*e) == EntityKind::Hatch ? 0.0 : geom::length(*e);
        if (len > 0 && std::isfinite(len)) {
            ++t.withLength;
            t.length += len;
        }
        if (auto a = closedArea(*e); a && std::isfinite(*a)) {
            ++t.closed;
            t.area += *a;
        }
    }
    return t;
}

std::string formatTotals(const Document& doc, const SelectionTotals& t, Precision precision) {
    std::string out = "Objects = " + std::to_string(t.count);
    if (t.withLength > 0)
        out += "\nTotal length = " + formatLengthIn(doc, t.length, precision) + " (" +
               std::to_string(t.withLength) + " object" + (t.withLength == 1 ? "" : "s") + ")";
    if (t.closed > 0)
        out += "\nTotal area = " + formatAreaIn(doc, t.area, precision) + " (" +
               std::to_string(t.closed) + " closed shape" + (t.closed == 1 ? "" : "s") + ")";
    return out;
}

}  // namespace tbcad::inquiry

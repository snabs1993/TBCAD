#include "tbcad/ui/selection.h"

#include <algorithm>
#include <limits>

#include "tbcad/geom/entity.h"

namespace tbcad::ui {

bool isVisible(const Document& doc, const Entity& e) {
    if (!e.common.visible)
        return false;
    return doc.effective(e.common).visible();
}

bool isSelectable(const Document& doc, const Entity& e) {
    if (!e.common.visible)
        return false;
    if (doc.ownerBlock(e.common.id))
        return false;
    EffectiveLayer layer = doc.effective(e.common);
    return layer.visible() && !layer.locked;
}

std::optional<EntityId> pickEntity(const Document& doc, const index::SpatialIndex& index, Vec2 p,
                                   double apertureMm) {
    std::optional<EntityId> best;
    double bestDist = std::numeric_limits<double>::infinity();
    index.queryPoint(p, apertureMm, [&](EntityId id, const BBox2&) {
        const Entity* e = doc.find(id);
        if (!e || !isSelectable(doc, *e))
            return;
        if (!geom::hitTest(*e, p, apertureMm, &doc))
            return;
        double d = geom::distance(*e, p, &doc);
        if (d < bestDist || (d == bestDist && best && id.value > best->value)) {
            bestDist = d;
            best = id;
        }
    });
    return best;
}

std::vector<EntityId> selectInBox(const Document& doc, const index::SpatialIndex& index,
                                  const BBox2& box, bool crossing) {
    std::vector<EntityId> out;
    if (box.empty())
        return out;
    index.query(box, [&](EntityId id, const BBox2& entityBox) {
        const Entity* e = doc.find(id);
        if (!e || !isSelectable(doc, *e))
            return;
        bool hit;
        if (crossing)
            hit = box.containsBox(entityBox) || geom::crossesWindow(*e, box, &doc);
        else
            hit = box.containsBox(entityBox) || geom::insideWindow(*e, box, &doc);
        if (hit)
            out.push_back(id);
    });
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace tbcad::ui

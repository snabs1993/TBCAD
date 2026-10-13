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

std::vector<EntityId> pickAllAt(const Document& doc, const index::SpatialIndex& index, Vec2 p,
                                double apertureMm) {
    std::vector<std::pair<double, EntityId>> hits;
    index.queryPoint(p, apertureMm, [&](EntityId id, const BBox2&) {
        const Entity* e = doc.find(id);
        if (e && isSelectable(doc, *e) && geom::hitTest(*e, p, apertureMm, &doc))
            hits.emplace_back(geom::distance(*e, p, &doc), id);
    });
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second.value > b.second.value;
    });
    std::vector<EntityId> out;
    out.reserve(hits.size());
    for (const auto& h : hits)
        out.push_back(h.second);
    return out;
}

std::optional<EntityId> cycleNext(const std::vector<EntityId>& stack,
                                  std::optional<EntityId> current) {
    if (stack.empty())
        return std::nullopt;
    if (current) {
        auto it = std::find(stack.begin(), stack.end(), *current);
        if (it != stack.end())
            return *(++it == stack.end() ? stack.begin() : it);
    }
    return stack.front();
}

namespace {

bool insidePolygon(const std::vector<Vec2>& poly, Vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    auto side = [](Vec2 p, Vec2 q, Vec2 r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    const double d1 = side(a, b, c), d2 = side(a, b, d), d3 = side(c, d, a), d4 = side(c, d, b);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 &&
           d4 != 0;
}

// The outline of an entity as chains of points: flattened curves, or the bounding rectangle for
// kinds without intrinsic curves (text, hatch, inserts, ...).
std::vector<std::vector<Vec2>> outlineOf(const Entity& e, const BBox2& box) {
    std::vector<std::vector<Vec2>> chains;
    std::vector<geom::CurveRef> curves;
    if (geom::curvesOf(e, curves) && !curves.empty()) {
        const double tolerance =
            std::max(1e-3, std::max(box.max.x - box.min.x, box.max.y - box.min.y) / 400);
        for (const geom::CurveRef& c : curves) {
            std::vector<Vec2> pts;
            geom::flatten(c, tolerance, pts);
            if (!pts.empty())
                chains.push_back(std::move(pts));
        }
    }
    if (chains.empty())
        chains.push_back(
            {box.min, {box.max.x, box.min.y}, box.max, {box.min.x, box.max.y}, box.min});
    return chains;
}

}  // namespace

std::vector<EntityId> selectInPolygon(const Document& doc, const index::SpatialIndex& index,
                                      const std::vector<Vec2>& polygon, bool crossing) {
    std::vector<EntityId> out;
    if (polygon.size() < 3)
        return out;
    BBox2 box;
    for (const Vec2& v : polygon)
        box.expand(v);
    index.query(box, [&](EntityId id, const BBox2& entityBox) {
        const Entity* e = doc.find(id);
        if (!e || !isSelectable(doc, *e))
            return;
        bool anyIn = false, allIn = true, cross = false;
        for (const auto& chain : outlineOf(*e, entityBox)) {
            for (std::size_t i = 0; i < chain.size(); ++i) {
                if (insidePolygon(polygon, chain[i]))
                    anyIn = true;
                else
                    allIn = false;
                if (i + 1 < chain.size() && !cross)
                    for (std::size_t k = 0; k < polygon.size() && !cross; ++k)
                        cross = segmentsCross(chain[i], chain[i + 1], polygon[k],
                                              polygon[(k + 1) % polygon.size()]);
            }
        }
        if (crossing ? (anyIn || cross) : (allIn && !cross))
            out.push_back(id);
    });
    std::sort(out.begin(), out.end());
    return out;
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

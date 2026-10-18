#include "tbcad/ui/selection.h"

#include <algorithm>
#include <limits>

#include "tbcad/geom/entity.h"
#include "tbcad/hatch/fill.h"

namespace tbcad::ui {

bool isVisible(const Document& doc, const Entity& e) {
    if (!e.common.visible)
        return false;
    return doc.effective(e.common).visible();
}

bool isSelectable(const Document& doc, const Entity& e) {
    if (!e.common.visible)
        return false;
    if (const BlockId owner = doc.ownerBlock(e.common.id)) {
        // Only the objects drawn on a layout sheet can be picked; those inside blocks cannot.
        const BlockDef* b = doc.block(owner);
        if (!b || b->name.rfind("*LAYOUT-", 0) != 0)
            return false;
    }
    EffectiveLayer layer = doc.effective(e.common);
    return layer.visible() && !layer.locked;
}

namespace {
bool isFillKind(const Entity& e) {
    return kind(e) == EntityKind::Hatch;
}

bool insidePolygon(const std::vector<Vec2>& poly, Vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

// A click anywhere inside a hatch's area (even-odd over its loops) selects it, like AutoCAD.
bool insideHatch(const Entity& e, Vec2 p) {
    const auto* h = entityAs<HatchData>(e);
    if (!h)
        return false;
    bool in = false;
    for (const auto& ring : hatch::loopRings(*h, 0.05))
        if (ring.size() >= 3 && insidePolygon(ring, p))
            in = !in;
    return in;
}

// Distance used for picking: 0 for a point inside a hatch, else the distance to its geometry.
// Returns false when the entity is not hit at all.
bool pickHit(const Document& doc, const Entity& e, Vec2 p, double apertureMm, double& d) {
    if (geom::hitTest(e, p, apertureMm, &doc)) {
        d = geom::distance(e, p, &doc);
        return true;
    }
    if (isFillKind(e) && insideHatch(e, p)) {
        d = 0;
        return true;
    }
    return false;
}
}  // namespace

std::optional<EntityId> pickEntity(const Document& doc, const index::SpatialIndex& index, Vec2 p,
                                   double apertureMm) {
    std::optional<EntityId> best;
    bool bestIsFill = true;
    double bestDist = std::numeric_limits<double>::infinity();
    index.queryPoint(p, apertureMm, [&](EntityId id, const BBox2&) {
        const Entity* e = doc.find(id);
        if (!e || !isSelectable(doc, *e))
            return;
        double d = 0;
        if (!pickHit(doc, *e, p, apertureMm, d))
            return;
        // A hatch is hit anywhere inside it, so a line or curve under the pointer wins over it.
        const bool fill = isFillKind(*e);
        if (best && fill && !bestIsFill)
            return;
        if ((bestIsFill && !fill) || d < bestDist ||
            (d == bestDist && best && id.value > best->value)) {
            bestDist = d;
            bestIsFill = fill;
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
        double d = 0;
        if (e && isSelectable(doc, *e) && pickHit(doc, *e, p, apertureMm, d))
            // Fills sort after every curve.
            hits.emplace_back(d + (isFillKind(*e) ? 1e9 : 0.0), id);
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

std::vector<EntityId> selectAlongFence(const Document& doc, const index::SpatialIndex& index,
                                       const std::vector<Vec2>& fence) {
    std::vector<EntityId> out;
    if (fence.size() < 2)
        return out;
    auto side = [](Vec2 p, Vec2 q, Vec2 r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    auto onSeg = [](Vec2 p, Vec2 q, Vec2 r) {  // r collinear with pq: inside its box?
        return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) &&
               std::min(p.y, q.y) <= r.y && r.y <= std::max(p.y, q.y);
    };
    auto touch = [&](Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
        const double d1 = side(a, b, c), d2 = side(a, b, d), d3 = side(c, d, a), d4 = side(c, d, b);
        if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
            ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
            return true;
        return (d1 == 0 && onSeg(a, b, c)) || (d2 == 0 && onSeg(a, b, d)) ||
               (d3 == 0 && onSeg(c, d, a)) || (d4 == 0 && onSeg(c, d, b));
    };
    std::vector<EntityId> seen;
    for (std::size_t k = 0; k + 1 < fence.size(); ++k) {
        BBox2 segBox;
        segBox.expand(fence[k]);
        segBox.expand(fence[k + 1]);
        index.query(segBox, [&](EntityId id, const BBox2& entityBox) {
            const Entity* e = doc.find(id);
            if (!e || !isSelectable(doc, *e))
                return;
            for (const auto& chain : outlineOf(*e, entityBox))
                for (std::size_t i = 0; i + 1 < chain.size(); ++i)
                    if (touch(chain[i], chain[i + 1], fence[k], fence[k + 1])) {
                        seen.push_back(id);
                        return;
                    }
        });
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    return seen;
}

}  // namespace tbcad::ui

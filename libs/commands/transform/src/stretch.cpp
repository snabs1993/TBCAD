#include "tbcad/transform/stretch.h"

#include "tbcad/dim/dim.h"
#include "tbcad/geom/geom.h"
#include "tbcad/transform/transform.h"

namespace tbcad::transform {

namespace {

// How many leading grips of the entity are real vertices; the rest (midpoints ...) just follow.
// -1 = the entity has one anchor, 0 = moves whole only when its bounding box is inside.
int vertexGrips(const Entity& e, std::size_t gripCount) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Arc:
            return 2;
        case EntityKind::Polyline:
            return static_cast<int>(std::get<PolylineData>(e.data).v.size());
        case EntityKind::Spline:
        case EntityKind::Leader:
        case EntityKind::MLeader:
        case EntityKind::Hatch:  // the boundary corners, as AutoCAD stretches a hatch's outline
            return static_cast<int>(gripCount);
        case EntityKind::MLine:
            return static_cast<int>(entityAs<MlineData>(e)->v.size());
        case EntityKind::Ray:    // rays and construction lines move with their base point
        case EntityKind::Table:  // tables with their insertion point
        case EntityKind::Circle:
        case EntityKind::Ellipse:
        case EntityKind::Text:
        case EntityKind::MText:
        case EntityKind::Insert:
        case EntityKind::Point:
            return -1;
        default:
            return 0;
    }
}

}  // namespace

namespace {

bool inPolygon(const std::vector<Vec2>& poly, Vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 a = poly[i];
        const Vec2 b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

// Objects whose bounding box lies in a convex window move whole, whatever their grips are.
bool simpleKind(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Circle:
        case EntityKind::Point:
            return true;
        default:
            return false;
    }
}

template <class Contains, class ContainsBox>
std::vector<std::pair<EntityId, Entity>> stretchImpl(const Document& doc,
                                                     const std::vector<EntityId>& ids,
                                                     Contains contains, ContainsBox containsBox,
                                                     Vec2 delta, std::string* error, bool convex) {
    std::vector<std::pair<EntityId, Entity>> out;
    const Transform2 shift = Transform2::translate(delta);
    auto fail = [&](const std::string& msg) {
        if (error && error->empty())
            *error = msg;
    };
    for (EntityId id : ids) {
        const Entity* src = doc.find(id);
        if (!src)
            continue;
        if (convex && simpleKind(*src) && containsBox(geom::bounds(*src, doc))) {
            auto moved = geom::transformed(*src, shift);
            if (moved)
                out.emplace_back(id, std::move(moved).value());
            else
                fail(moved.error().message);
            continue;
        }
        const std::vector<Vec2> grips = geom::gripPoints(*src);
        const int nv = vertexGrips(*src, grips.size());
        bool whole = false;
        std::vector<int> inside;
        if (const auto* dm = entityAs<DimensionData>(*src)) {
            // The definition points that the kind uses, and the text: those inside the window
            // move, so the measurement follows. Everything inside moves the dimension whole.
            const std::size_t used = dm->kind == DimKind::Linear || dm->kind == DimKind::Aligned ||
                                             dm->kind == DimKind::Ordinate
                                         ? 3
                                     : dm->kind == DimKind::Radius || dm->kind == DimKind::Diameter
                                         ? 2
                                         : 4;
            std::size_t total = 0;
            for (std::size_t i = 0; i <= used && i < grips.size(); ++i) {
                const std::size_t idx = i < used ? i : 4;  // 4 = the text
                if (idx >= grips.size())
                    break;
                ++total;
                if (contains(grips[idx]))
                    inside.push_back(static_cast<int>(idx));
            }
            whole = !inside.empty() && inside.size() == total;
        } else if (nv < 0) {
            whole = !grips.empty() && contains(grips[0]);
        } else if (nv == 0) {
            whole = containsBox(geom::bounds(*src, doc));
        } else {
            for (int i = 0; i < nv && i < static_cast<int>(grips.size()); ++i)
                if (contains(grips[i]))
                    inside.push_back(i);
            whole = static_cast<int>(inside.size()) == nv;
        }
        if (!whole && inside.empty())
            continue;
        if (whole) {
            auto moved = geom::transformed(*src, shift);
            if (moved)
                out.emplace_back(id, std::move(moved).value());
            else
                fail(moved.error().message);
            continue;
        }
        Entity cur = *src;
        bool ok = true;
        for (int idx : inside) {
            auto moved = geom::moveGrip(cur, idx, grips[idx] + delta);
            if (!moved) {
                fail(moved.error().message);
                ok = false;
                break;
            }
            cur = std::move(moved).value();
        }
        if (ok) {
            if (auto* dm = entityAs<DimensionData>(cur))
                dim::refresh(doc, *dm);
            out.emplace_back(id, std::move(cur));
        }
    }
    return out;
}

}  // namespace

std::vector<std::pair<EntityId, Entity>> stretchByWindow(const Document& doc,
                                                         const std::vector<EntityId>& ids,
                                                         const BBox2& window, Vec2 delta,
                                                         std::string* error) {
    return stretchImpl(
        doc, ids, [&](Vec2 p) { return window.contains(p); },
        [&](const BBox2& b) { return window.containsBox(b); }, delta, error, true);
}

std::vector<std::pair<EntityId, Entity>> stretchByPolygon(const Document& doc,
                                                          const std::vector<EntityId>& ids,
                                                          const std::vector<Vec2>& polygon,
                                                          Vec2 delta, std::string* error) {
    if (polygon.size() < 3)
        return {};
    return stretchImpl(
        doc, ids, [&](Vec2 p) { return inPolygon(polygon, p); },
        [&](const BBox2& b) {
            return inPolygon(polygon, b.min) && inPolygon(polygon, b.max) &&
                   inPolygon(polygon, {b.min.x, b.max.y}) && inPolygon(polygon, {b.max.x, b.min.y});
        },
        delta, error, false);
}

}  // namespace tbcad::transform

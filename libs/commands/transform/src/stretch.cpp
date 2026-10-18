#include "tbcad/transform/stretch.h"

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
            return static_cast<int>(gripCount);
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

std::vector<std::pair<EntityId, Entity>> stretchByWindow(const Document& doc,
                                                         const std::vector<EntityId>& ids,
                                                         const BBox2& window, Vec2 delta,
                                                         std::string* error) {
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
        const std::vector<Vec2> grips = geom::gripPoints(*src);
        const int nv = vertexGrips(*src, grips.size());
        bool whole = false;
        std::vector<int> inside;
        if (nv < 0) {
            whole = !grips.empty() && window.contains(grips[0]);
        } else if (nv == 0) {
            whole = window.containsBox(geom::bounds(*src, doc));
        } else {
            for (int i = 0; i < nv && i < static_cast<int>(grips.size()); ++i)
                if (window.contains(grips[i]))
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
        if (ok)
            out.emplace_back(id, std::move(cur));
    }
    return out;
}

}  // namespace tbcad::transform

#include "tbcad/transform/apply.h"

#include "tbcad/geom/geom.h"

namespace tbcad::transform {

ApplyReport applyTransform(cmd::CommandContext& ctx, std::span<const EntityId> ids,
                           const Transform2& t, bool copy) {
    ApplyReport rep;
    for (EntityId id : ids) {
        const Entity* src = ctx.doc().find(id);
        if (!src)
            continue;
        auto moved = geom::transformed(*src, t);
        if (!moved) {
            ++rep.failed;
            if (rep.firstError.empty())
                rep.firstError = moved.error().message;
            continue;
        }
        if (copy) {
            auto added = ctx.tx().add(std::move(moved).value());
            if (added) {
                ++rep.applied;
                rep.created.push_back(added.value());
            } else {
                ++rep.failed;
                if (rep.firstError.empty())
                    rep.firstError = added.error().message;
            }
        } else {
            Entity result = std::move(moved).value();
            auto r = ctx.tx().modify(id, [&](Entity& e) { e.data = std::move(result.data); });
            if (r) {
                ++rep.applied;
            } else {
                ++rep.failed;
                if (rep.firstError.empty())
                    rep.firstError = r.error().message;
            }
        }
    }
    return rep;
}

void buildPreview(const Document& doc, std::span<const EntityId> ids, const Transform2& t,
                  double pixelSizeMm, DisplayList& out) {
    geom::DisplayContext dc;
    dc.pixelSizeMm = pixelSizeMm > 0 ? pixelSizeMm : 1;
    if (ids.size() > kPreviewEntityLimit) {
        BBox2 box;
        for (EntityId id : ids) {
            if (const Entity* e = doc.find(id))
                box.merge(geom::bounds(*e, doc));
        }
        if (box.empty())
            return;
        DisplayPolyline pl;
        pl.closed = true;
        pl.style.color = Rgba{255, 255, 255, 255};
        for (Vec2 c : {box.min, Vec2{box.max.x, box.min.y}, box.max, Vec2{box.min.x, box.max.y}})
            pl.pts.push_back(t.apply(c));
        out.items.emplace_back(std::move(pl));
        return;
    }
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        auto moved = geom::transformed(*e, t);
        if (moved)
            geom::buildDisplay(doc, moved.value(), dc, out);
    }
}

}  // namespace tbcad::transform

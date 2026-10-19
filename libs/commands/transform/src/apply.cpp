#include "tbcad/transform/apply.h"

#include <cmath>
#include <cstdint>

#include "tbcad/geom/geom.h"

namespace tbcad::transform {

namespace {

// MIRRTEXT = 1: text is reflected with the rest, which AutoCAD shows as backward or upside-down
// text. `e` already went through geom::transformed, which keeps text readable.
void reflectText(const Entity& src, Entity& e, const Transform2& t) {
    const auto* from = entityAs<TextData>(src);
    auto* to = entityAs<TextData>(e);
    if (!from || !to)
        return;
    const Vec2 u = t.applyVector({std::cos(from->rotation), std::sin(from->rotation)});
    const double ang = std::atan2(u.y, u.x);
    if (u.x < 0) {
        to->rotation = ang + kPi;
        to->backward = !from->backward;
        to->upsideDown = from->upsideDown;
    } else {
        to->rotation = ang;
        to->upsideDown = !from->upsideDown;
        to->backward = from->backward;
    }
    to->oblique = -from->oblique;
    if (to->backward && to->upsideDown) {  // both flips are a half turn
        to->backward = to->upsideDown = false;
        to->rotation += kPi;
    }
    to->rotation = std::remainder(to->rotation, kTwoPi);
}

}  // namespace

ApplyReport applyTransform(cmd::CommandContext& ctx, std::span<const EntityId> ids,
                           const Transform2& t, bool copy) {
    ApplyReport rep;
    const bool reflect = t.determinant() < 0 && mirrText(ctx.doc().header());
    if (!copy)
        ctx.tx().expectTouching(ids.size());
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
        if (reflect)
            reflectText(*src, moved.value(), t);
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
        // The cursor moves many times over the same selection: measure it once.
        struct Cached {
            std::uint64_t uid = 0, revision = 0;
            std::uint64_t hash = 0;
            std::size_t size = 0;
            BBox2 box{};
        };
        thread_local Cached cache;
        std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a over the ids: the same selection
        for (EntityId id : ids)
            hash = (hash ^ id.value) * 1099511628211ULL;
        BBox2 box;
        if (cache.uid == doc.uid() && cache.revision == doc.revision() && cache.hash == hash &&
            cache.size == ids.size()) {
            box = cache.box;
        } else {
            for (EntityId id : ids) {
                if (const Entity* e = doc.find(id))
                    box.merge(geom::bounds(*e, doc));
            }
            cache = {doc.uid(), doc.revision(), hash, ids.size(), box};
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
    const bool reflect = t.determinant() < 0 && mirrText(doc.header());
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        auto moved = geom::transformed(*e, t);
        if (moved && reflect)
            reflectText(*e, moved.value(), t);
        if (moved)
            geom::buildDisplay(doc, moved.value(), dc, out);
    }
}

}  // namespace tbcad::transform

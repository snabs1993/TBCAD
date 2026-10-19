#include "tbcad/blocks/picture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "tbcad/blocks/blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"

namespace tbcad::blocks {

namespace {

// The frame's corners in block coordinates, when the Insert shows a picture.
std::optional<std::array<Vec2, 4>> frameCorners(const Document& doc, const InsertData& ins,
                                                const BlockDef*& def) {
    def = doc.block(ins.block);
    if (!def || def->xrefPath.empty() || ins.array)
        return std::nullopt;
    if (underlay::isPdfPath(def->xrefPath)) {
        // A PDF page: the corners of what the page drew.
        BBox2 box;
        for (EntityId id : def->entities)
            if (const Entity* e = doc.find(id))
                box.merge(geom::bounds(*e, doc));
        if (!(box.max.x > box.min.x) || !(box.max.y > box.min.y))
            return std::nullopt;
        return std::array<Vec2, 4>{Vec2{box.min.x, box.min.y}, Vec2{box.max.x, box.min.y},
                                   Vec2{box.max.x, box.max.y}, Vec2{box.min.x, box.max.y}};
    }
    if (!underlay::isImagePath(def->xrefPath))
        return std::nullopt;
    for (EntityId id : def->entities) {
        const Entity* e = doc.find(id);
        const auto* pl = e ? entityAs<PolylineData>(*e) : nullptr;
        if (!pl || !pl->closed || pl->v.size() != 4)
            continue;
        Vec2 lo = pl->v[0].p, hi = pl->v[0].p;
        for (const PolyVertex& v : pl->v) {
            lo = {std::min(lo.x, v.p.x), std::min(lo.y, v.p.y)};
            hi = {std::max(hi.x, v.p.x), std::max(hi.y, v.p.y)};
        }
        if (!(hi.x > lo.x) || !(hi.y > lo.y))
            return std::nullopt;
        return std::array<Vec2, 4>{Vec2{lo.x, lo.y}, Vec2{hi.x, lo.y}, Vec2{hi.x, hi.y},
                                   Vec2{lo.x, hi.y}};
    }
    return std::nullopt;
}

}  // namespace

std::vector<PictureHandle> pictureHandles(const Document& doc, const Entity& insert) {
    std::vector<PictureHandle> out;
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = nullptr;
    auto c = ins ? frameCorners(doc, *ins, def) : std::nullopt;
    if (!c)
        return out;
    const Transform2 t = instanceTransform(*ins, def->base);
    for (int i = 0; i < 4; ++i)
        out.push_back({i, t.apply((*c)[static_cast<std::size_t>(i)])});
    return out;
}

Result<Entity> pictureDrag(const Document& doc, const Entity& insert, int corner, Vec2 world) {
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = nullptr;
    auto c = ins ? frameCorners(doc, *ins, def) : std::nullopt;
    if (!c)
        return makeError(ErrorCode::InvalidArgument, "This object is not a picture");
    if (corner < 0 || corner > 3)
        return makeError(ErrorCode::OutOfRange, "There is no such corner");
    const Transform2 t = instanceTransform(*ins, def->base);
    const Vec2 grabbed = t.apply((*c)[static_cast<std::size_t>(corner)]);
    const Vec2 fixed = t.apply((*c)[static_cast<std::size_t>((corner + 2) % 4)]);
    const Vec2 diag = grabbed - fixed;
    const double len2 = dot(diag, diag);
    if (!(len2 > 0) || !std::isfinite(len2))
        return makeError(ErrorCode::InvalidArgument, "The picture has no size");
    double s = dot(world - fixed, diag) / len2;
    if (!std::isfinite(s))
        return makeError(ErrorCode::InvalidArgument, "That point is not valid");
    s = std::max(s, 1e-3);  // a picture never turns inside out or vanishes
    Entity out = insert;
    auto& d = *entityAs<InsertData>(out);
    d.position = fixed + (ins->position - fixed) * s;
    d.scale = ins->scale * s;
    return out;
}

}  // namespace tbcad::blocks

#include "tbcad/blocks/explode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

#include "internal.h"
#include "tbcad/blocks/array.h"
#include "tbcad/blocks/attributes.h"
#include "tbcad/blocks/dynamic.h"

namespace tbcad::blocks {

namespace detail {

Entity resolveChild(const Entity& child, const Entity& insert) {
    Entity e = child;
    e.common.id = {};
    if (e.common.layer == kLayerZero)
        e.common.layer = insert.common.layer;
    if (e.common.color.kind == ColorKind::ByBlock)
        e.common.color = insert.common.color;
    if (e.common.linetype == kLinetypeByBlock)
        e.common.linetype = insert.common.linetype;
    if (e.common.lineweight.isByBlock())
        e.common.lineweight = insert.common.lineweight;
    return e;
}

namespace {

bool hasCurveOrPoint(EntityKind k) {
    switch (k) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Point:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
            return true;
        default:
            return false;
    }
}

bool needsUniformScale(EntityKind k) {
    return k == EntityKind::Text || k == EntityKind::MText || k == EntityKind::Dimension ||
           k == EntityKind::Leader;
}

// Rotation-and-uniform-scale approximation of t (a mirror is kept), with the same translation as
// t at `anchor`.
Transform2 uniformApprox(const Transform2& t, Vec2 anchor) {
    Vec2 c0 = t.applyVector({1, 0});
    double det = t.determinant();
    double s = std::sqrt(std::abs(det));
    double a = std::atan2(c0.y, c0.x);
    Transform2 u = Transform2::rotate(a) * Transform2::scale(s, det < 0 ? -s : s);
    Vec2 delta = t.apply(anchor) - u.apply(anchor);
    return Transform2::translate(delta) * u;
}

}  // namespace

void Flatten::note(const std::string& m) {
    if (std::find(messages.begin(), messages.end(), m) == messages.end())
        messages.push_back(m);
}

void Flatten::contents(const Entity& insert, const Transform2& outer, int depth) {
    if (depth > kMaxNesting) {
        note("Block nesting is too deep; the inner blocks were skipped");
        return;
    }
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def)
        return;
    std::optional<AsmResolved> res;
    if (def->assembly) {
        res = resolveAssembly(doc, *def, *ins);
        ins = &res->insert;
    }
    std::vector<Entity> storage;
    for (const Entity* child : blockItems(doc, *def, ins, storage, res ? &res->nested : nullptr)) {
        if (!child->common.visible)
            continue;
        if (const auto* t = entityAs<TextData>(*child); t && t->attdef) {
            // An attribute turns into plain text holding this insert's value.
            auto shown = attributeText(*child, *ins);
            if (shown)
                place(resolveChild(*shown, insert), outer, depth);
            continue;
        }
        place(resolveChild(*child, insert), outer, depth);
    }
}

void Flatten::place(Entity child, const Transform2& t, int depth) {
    const EntityKind k = kind(child);
    if (k == EntityKind::Insert) {
        const auto* ins = entityAs<InsertData>(child);
        const BlockDef* def = doc.block(ins->block);
        if (!def)
            return;
        if (!expandNested) {
            auto r = geom::transformed(child, t);
            if (r) {
                out.push_back(std::move(r).value());
                return;
            }
            note("A nested block cannot be scaled this way, so it was exploded too");
        }
        for (const Transform2& it : instanceTransforms(*ins, def->base))
            contents(child, t * it, depth + 1);
        return;
    }
    if (curvesOnly && !hasCurveOrPoint(k))
        return;
    auto r = geom::transformed(child, t);
    if (r) {
        out.push_back(std::move(r).value());
        return;
    }
    if (k == EntityKind::Polyline) {
        note("Polylines with arcs were split into lines and elliptical arcs");
        for (Entity& piece : explodePolyline(child))
            place(std::move(piece), t, depth);
        return;
    }
    if (needsUniformScale(k)) {
        auto gp = geom::gripPoints(child);
        Vec2 anchor = gp.empty() ? Vec2{} : gp.front();
        auto approx = geom::transformed(child, uniformApprox(t, anchor));
        if (approx) {
            note("Text and dimensions cannot be stretched, so they were scaled evenly");
            out.push_back(std::move(approx).value());
            return;
        }
    }
    note("An object could not be transformed and was skipped: " + r.error().message);
}

}  // namespace detail

// ---- Registry --------------------------------------------------------------------------------

namespace {

struct HookTable {
    std::mutex mutex;
    std::array<ExplodeFn, 14> fns{};
};

HookTable& hooks() {
    static HookTable t;
    return t;
}

std::size_t slot(EntityKind k) {
    return static_cast<std::size_t>(k);
}

bool builtIn(EntityKind k) {
    return k == EntityKind::Insert || k == EntityKind::Polyline;
}

}  // namespace

void ExplodeRegistry::set(EntityKind kind, ExplodeFn fn) {
    if (builtIn(kind))
        return;
    auto& h = hooks();
    std::lock_guard lock(h.mutex);
    h.fns[slot(kind)] = std::move(fn);
}

void ExplodeRegistry::clear(EntityKind kind) {
    auto& h = hooks();
    std::lock_guard lock(h.mutex);
    h.fns[slot(kind)] = nullptr;
}

bool ExplodeRegistry::has(EntityKind kind) {
    auto& h = hooks();
    std::lock_guard lock(h.mutex);
    return static_cast<bool>(h.fns[slot(kind)]);
}

bool canExplode(EntityKind kind) {
    return builtIn(kind) || ExplodeRegistry::has(kind);
}

// ---- Polyline -----------------------------------------------------------------------------------

std::vector<Entity> explodePolyline(const Entity& e) {
    std::vector<Entity> out;
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl || pl->v.size() < 2)
        return out;
    const std::size_t n = pl->v.size();
    const std::size_t segs = pl->closed ? n : n - 1;
    for (std::size_t i = 0; i < segs; ++i) {
        const PolyVertex& a = pl->v[i];
        const PolyVertex& b = pl->v[(i + 1) % n];
        if (tol::isZeroLength(b.p - a.p))
            continue;
        Entity piece;
        piece.common = e.common;
        piece.common.id = {};
        auto arc = a.bulge != 0 ? geom::bulgeArc(a.p, b.p, a.bulge) : std::nullopt;
        if (arc) {
            ArcData d;
            d.center = arc->center;
            d.radius = arc->radius;
            d.startAngle = normalizeAngle(arc->startAngle);
            d.endAngle = normalizeAngle(arc->startAngle + arc->sweep);
            piece.data = d;
        } else {
            piece.data = LineData{a.p, b.p};
        }
        out.push_back(std::move(piece));
    }
    return out;
}

// ---- explode ------------------------------------------------------------------------------------

Result<ExplodeResult> explode(const Document& doc, const Entity& entity) {
    switch (kind(entity)) {
        case EntityKind::Insert: {
            const auto* ins = entityAs<InsertData>(entity);
            const BlockDef* def = doc.block(ins->block);
            if (!def)
                return makeError(ErrorCode::NotFound, "The block of this insert no longer exists");
            detail::Flatten f{doc};
            for (const Transform2& t : instanceTransforms(*ins, def->base))
                f.contents(entity, t, 0);
            return ExplodeResult{std::move(f.out), std::move(f.messages)};
        }
        case EntityKind::Polyline: {
            ExplodeResult r;
            const auto* pl = entityAs<PolylineData>(entity);
            bool wide = pl->constantWidth != 0;
            for (const auto& v : pl->v)
                wide = wide || v.startWidth != 0 || v.endWidth != 0;
            r.entities = explodePolyline(entity);
            if (wide)
                r.messages.push_back("Polyline widths were dropped");
            return r;
        }
        default:
            break;
    }
    ExplodeFn fn;
    {
        auto& h = hooks();
        std::lock_guard lock(h.mutex);
        fn = h.fns[slot(kind(entity))];
    }
    if (!fn)
        return makeError(ErrorCode::Unsupported, "This kind of object cannot be exploded");
    return fn(doc, entity);
}

}  // namespace tbcad::blocks

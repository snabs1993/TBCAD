#include <cmath>
#include <mutex>

#include "internal.h"
#include "tbcad/blocks/array.h"
#include "tbcad/blocks/attributes.h"
#include "tbcad/blocks/clip.h"
#include "tbcad/blocks/display.h"
#include "tbcad/blocks/dynamic.h"

namespace tbcad::blocks {

namespace {

thread_local BlockDisplayCache* g_active = nullptr;
thread_local int g_depth = 0;

struct DepthGuard {
    DepthGuard() { ++g_depth; }
    ~DepthGuard() { --g_depth; }
};

// Largest factor the instance transform stretches by; block-space tolerance is the drawing-space
// one divided by it so that the result still looks smooth after scaling.
double maxStretch(const Transform2& t) {
    return std::max(std::hypot(t.m00, t.m10), std::hypot(t.m01, t.m11));
}

// Rounds down to a power of two so that nearby zoom levels and scales share one cache entry.
double quantizeTolerance(double tol) {
    if (!(tol > 0) || !std::isfinite(tol))
        return 1e-3;
    return std::exp2(std::floor(std::log2(tol)));
}

void transformList(const DisplayList& in, const Transform2& t, DisplayList& out) {
    out.items.reserve(out.items.size() + in.items.size());
    for (const DisplayItem& item : in.items) {
        std::visit(
            [&](const auto& src) {
                auto copy = src;
                if constexpr (std::is_same_v<std::decay_t<decltype(src)>, DisplayFill>) {
                    for (auto& ring : copy.rings)
                        for (Vec2& p : ring)
                            p = t.apply(p);
                } else {
                    for (Vec2& p : copy.pts)
                        p = t.apply(p);
                }
                out.items.emplace_back(std::move(copy));
            },
            item);
    }
}

// The block's content in block space for one combination of insert style and tolerance.
std::shared_ptr<DisplayList> buildBlockSpace(const Document& doc, const BlockDef& def,
                                             const Entity& insert, const InsertData* effective,
                                             const std::vector<NestedSet>* nested,
                                             const geom::DisplayContext& child) {
    auto list = std::make_shared<DisplayList>();
    std::vector<Entity> storage;
    for (const Entity* e : blockItems(doc, def, effective, storage, nested)) {
        if (!e->common.visible)
            continue;
        // Attribute definitions show per-insert values and are added by buildInsertDisplay.
        if (const auto* t = entityAs<TextData>(*e); t && t->attdef)
            continue;
        // Only the layer is substituted here: ByBlock values stay symbolic and are resolved by
        // the generators through child.byBlock*.
        Entity resolved = *e;
        if (resolved.common.layer == kLayerZero)
            resolved.common.layer = insert.common.layer;
        if (kind(resolved) == EntityKind::Insert) {
            buildInsertDisplay(doc, resolved, child, *list);
        } else {
            geom::buildDisplay(doc, resolved, child, *list);
        }
    }
    return list;
}

}  // namespace

// ---- cache
// ----------------------------------------------------------------------------------------

void BlockDisplayCache::sync(std::uint64_t revision) {
    if (revision != revision_) {
        entries_.clear();
        revision_ = revision;
    }
}

std::shared_ptr<const DisplayList> BlockDisplayCache::find(const Key& key) const {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second;
}

void BlockDisplayCache::put(const Key& key, std::shared_ptr<const DisplayList> list) {
    entries_[key] = std::move(list);
}

void BlockDisplayCache::clear() {
    entries_.clear();
    revision_ = ~std::uint64_t{0};
}

BlockDisplayScope::BlockDisplayScope(BlockDisplayCache& cache) : previous_(g_active) {
    g_active = &cache;
}

BlockDisplayScope::~BlockDisplayScope() {
    g_active = previous_;
}

// ---- generator
// --------------------------------------------------------------------------------------

void buildInsertDisplay(const Document& doc, const Entity& insert, const geom::DisplayContext& ctx,
                        DisplayList& out) {
    const auto* ins = entityAs<InsertData>(insert);
    if (!ins || g_depth >= kMaxNesting)
        return;
    const BlockDef* def = doc.block(ins->block);
    if (!def || def->entities.empty())
        return;
    DepthGuard guard;
    const std::size_t firstItem = out.items.size();

    // An assembly's formulas turn its parameter values into the values the content is built from.
    std::optional<AsmResolved> resolved;
    const InsertData* shown = ins;
    if (def->assembly) {
        resolved = resolveAssembly(doc, *def, *ins);
        shown = &resolved->insert;
    }

    // Style of the insert itself: what ByBlock means for everything inside.
    // The insert's own transparency reaches the content through ByBlock, not through its colour.
    geom::DisplayContext opaque = ctx;
    opaque.showTransparency = false;
    const DisplayStyle self = geom::resolveStyle(doc, insert, opaque);
    const Transform2 t = instanceTransform(*ins, def->base);
    const std::vector<Transform2> items = instanceTransforms(*ins, def->base);

    geom::DisplayContext child = ctx;
    child.byBlockColor = self.color;
    child.byBlockLineweightMm = self.lineweight;
    child.byBlockPattern = self.pattern;
    child.byBlockTransparency = geom::resolveTransparency(doc, insert, ctx);
    const double stretch = std::max(maxStretch(t), 1e-12);
    child.chordTolMm = quantizeTolerance(ctx.tolerance() / stretch);

    // A dynamic block or assembly looks different for every set of values, so it is not shared.
    BlockDisplayCache* cache = (def->dynamic || def->assembly) ? nullptr : g_active;
    std::shared_ptr<const DisplayList> blockSpace;
    BlockDisplayCache::Key key;
    if (cache) {
        cache->sync(doc.revision());
        key = {def->id,
               insert.common.layer,
               self.color,
               self.lineweight,
               self.pattern,
               child.chordTolMm,
               ctx.showTransparency ? child.byBlockTransparency : -1};
        blockSpace = cache->find(key);
    }
    if (!blockSpace) {
        blockSpace = buildBlockSpace(doc, *def, insert, shown,
                                     resolved ? &resolved->nested : nullptr, child);
        if (cache) {
            cache->countBuild();
            cache->put(key, blockSpace);
        }
    }
    const bool clipped = ins->clipOn && ins->clip.size() >= 3;
    if (clipped) {
        // The boundary is in block coordinates; array items act in block space before it.
        for (const Transform2& it : items) {
            std::vector<Vec2> boundary;
            boundary.reserve(ins->clip.size());
            for (const Vec2& p : ins->clip)
                boundary.push_back(p);
            transformList(clipDisplay(*blockSpace, boundary, ins->clipInvert), it, out);
        }
    } else {
        for (const Transform2& it : items)
            transformList(*blockSpace, it, out);
    }

    // Attribute values differ per insert, so they are built here and not shared.
    for (EntityId id : def->entities) {
        const Entity* e = doc.find(id);
        if (!e || !e->common.visible)
            continue;
        auto text = attributeText(*e, *shown);
        if (!text)
            continue;
        if (text->common.layer == kLayerZero)
            text->common.layer = insert.common.layer;
        DisplayList own;
        geom::buildDisplay(doc, *text, child, own);
        if (clipped)
            own = clipDisplay(own, ins->clip, ins->clipInvert);
        for (const Transform2& it : items)
            transformList(own, it, out);
    }

    // Overlapping assemblies of one join class read as a single body.
    if (g_depth == 1 && detail::joinApplies(doc, insert, *def, *ins))
        detail::joinClip(doc, insert, *def, ctx, out, firstItem);
}

bool buildInsertInstance(const Document& doc, const Entity& insert, const geom::DisplayContext& ctx,
                         InsertInstance& out) {
    const auto* ins = entityAs<InsertData>(insert);
    BlockDisplayCache* cache = g_active;
    if (!ins || !cache || g_depth != 0)
        return false;
    const BlockDef* def = doc.block(ins->block);
    if (!def || def->entities.empty() || def->dynamic || def->assembly)
        return false;
    if (ins->clipOn && ins->clip.size() >= 3)
        return false;
    std::vector<Transform2> items = instanceTransforms(*ins, def->base);
    if (items.empty())
        return false;
    DepthGuard guard;
    geom::DisplayContext opaque = ctx;
    opaque.showTransparency = false;
    const DisplayStyle self = geom::resolveStyle(doc, insert, opaque);
    const Transform2 t = instanceTransform(*ins, def->base);
    geom::DisplayContext child = ctx;
    child.byBlockColor = self.color;
    child.byBlockLineweightMm = self.lineweight;
    child.byBlockPattern = self.pattern;
    child.byBlockTransparency = geom::resolveTransparency(doc, insert, ctx);
    child.chordTolMm = quantizeTolerance(ctx.tolerance() / std::max(maxStretch(t), 1e-12));

    cache->sync(doc.revision());
    const BlockDisplayCache::Key key{def->id,
                                     insert.common.layer,
                                     self.color,
                                     self.lineweight,
                                     self.pattern,
                                     child.chordTolMm,
                                     ctx.showTransparency ? child.byBlockTransparency : -1};
    std::shared_ptr<const DisplayList> blockSpace = cache->find(key);
    if (!blockSpace) {
        blockSpace = buildBlockSpace(doc, *def, insert, ins, nullptr, child);
        cache->countBuild();
        cache->put(key, blockSpace);
    }
    // Attribute values differ per insert: drawn here, once per array item.
    out.extra.items.clear();
    for (EntityId id : def->entities) {
        const Entity* e = doc.find(id);
        if (!e || !e->common.visible)
            continue;
        auto text = attributeText(*e, *ins);
        if (!text)
            continue;
        if (text->common.layer == kLayerZero)
            text->common.layer = insert.common.layer;
        DisplayList own;
        geom::buildDisplay(doc, *text, child, own);
        for (const Transform2& it : items)
            transformList(own, it, out.extra);
    }
    out.blockSpace = std::move(blockSpace);
    out.placements = std::move(items);
    return true;
}

void registerInsertDisplay() {
    static std::once_flag once;
    std::call_once(once, [] {
        geom::DisplayRegistry::set(
            EntityKind::Insert,
            [](const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
               DisplayList& out) { buildInsertDisplay(doc, e, ctx, out); },
            [](const Document& doc, const Entity& e) { return insertBounds(doc, e); });
    });
}

}  // namespace tbcad::blocks

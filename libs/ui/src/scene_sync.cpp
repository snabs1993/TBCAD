#include "tbcad/ui/scene_sync.h"

#include <algorithm>

#include "tbcad/geom/entity.h"
#include "tbcad/render/layer_sync.h"

namespace tbcad::ui {

namespace {

constexpr double kLockedFade = 0.4;

void fade(DisplayList& list) {
    auto dim = [](DisplayStyle& s) {
        s.color.a = static_cast<std::uint8_t>(static_cast<double>(s.color.a) * kLockedFade);
    };
    for (auto& item : list.items)
        std::visit([&](auto& it) { dim(it.style); }, item);
}

// Curved or generated geometry is flattened for one zoom level; straight kinds never need it.
bool needsTessellation(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Point:
            return false;
        case EntityKind::Polyline: {
            const auto* pl = entityAs<PolylineData>(e);
            for (const auto& v : pl->v)
                if (v.bulge != 0)
                    return true;
            return false;
        }
        default:
            return true;
    }
}

bool mentionsField(const std::string& s) {
    return s.find("%<") != std::string::npos;
}

bool holdsFields(const Entity& e) {
    if (const auto* t = entityAs<TextData>(e))
        return mentionsField(t->utf8);
    if (const auto* m = entityAs<MTextData>(e))
        return mentionsField(m->utf8);
    if (const auto* tb = entityAs<TableData>(e)) {
        for (const TableCell& c : tb->cells)
            if (mentionsField(c.utf8))
                return true;
    }
    return false;
}

std::uint32_t drawOrderOf(EntityId id) {
    return static_cast<std::uint32_t>(id.value);
}

}  // namespace

SceneSync::SceneSync(Document& doc, render::Scene& scene) : doc_(doc), scene_(scene) {
    doc_.addObserver(this);
}

SceneSync::~SceneSync() {
    doc_.removeObserver(this);
}

// Objects on a hidden level (or, in "hide others" view, on another level) are taken out of the
// scene; layer visibility is handled by the scene itself.
bool SceneSync::hiddenByLevel(const Entity& e) const {
    return doc_.effective(e.common.layer).on && !doc_.effective(e.common).on;
}

void SceneSync::upsertEntity(const Entity& e, std::optional<std::uint32_t> drawOrder) {
    const EntityId id = e.common.id;
    if (!e.common.visible || doc_.ownerBlock(id) || hiddenByLevel(e)) {
        scene_.remove(id);
        fieldTexts_.erase(id);
        return;
    }
    if (holdsFields(e))
        fieldTexts_.insert(id);
    else
        fieldTexts_.erase(id);
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = pixelSizeMm_;
    ctx.showTransparency = showTransparency_;
    DisplayList list;
    {
        blocks::BlockDisplayScope scope(blockCache_);
        geom::buildDisplay(doc_, e, ctx, list);
    }
    if (doc_.effective(e.common).locked)
        fade(list);
    // An object already in the scene keeps its place in the draw order.
    scene_.upsert(
        id, list,
        drawOrder.value_or(scene_.contains(id) ? scene_.drawOrderOf(id) : drawOrderOf(id)),
        needsTessellation(e) ? 1.0 / pixelSizeMm_ : 0.0);
}

void SceneSync::upsertMany(const std::vector<const Entity*>& list) {
    constexpr std::size_t kChunk = 16384;
    if (list.size() <= 8) {
        for (const Entity* e : list)
            upsertEntity(*e);
        return;
    }
    std::vector<EntityId> ids;
    std::vector<std::uint32_t> orders;
    for (std::size_t from = 0; from < list.size(); from += kChunk) {
        const std::size_t to = std::min(list.size(), from + kChunk);
        ids.clear();
        orders.clear();
        for (std::size_t i = from; i < to; ++i) {
            const EntityId id = list[i]->common.id;
            ids.push_back(id);
            orders.push_back(scene_.contains(id) ? scene_.drawOrderOf(id) : drawOrderOf(id));
        }
        scene_.removeMany(ids);
        for (std::size_t i = from; i < to; ++i)
            upsertEntity(*list[i], orders[i - from]);
    }
}

void SceneSync::syncLayers() {
    render::syncLayerVisibility(scene_, doc_);
    // Entities whose layer became locked or unlocked change appearance.
    std::unordered_set<LayerId> locked;
    for (const auto& [id, layer] : doc_.layers())
        if (doc_.effective(id).locked)
            locked.insert(id);
    // Entities on a layer whose transparency changed (ByLayer ones follow it).
    std::unordered_map<LayerId, std::uint8_t> transparency;
    for (const auto& [id, layer] : doc_.layers())
        transparency[id] = layer.transparency;
    std::unordered_set<LayerId> changed;
    if (transparency != layerTransparency_) {
        for (const auto& [id, t] : transparency) {
            auto it = layerTransparency_.find(id);
            if (it != layerTransparency_.end() && it->second != t)
                changed.insert(id);
        }
        layerTransparency_ = std::move(transparency);
    }
    if (locked == lockedLayers_ && changed.empty())
        return;
    for (LayerId id : locked)
        if (!lockedLayers_.count(id))
            changed.insert(id);
    for (LayerId id : lockedLayers_)
        if (!locked.count(id))
            changed.insert(id);
    lockedLayers_ = std::move(locked);
    std::vector<const Entity*> redraw;
    doc_.forEachEntity([&](const Entity& e) {
        if (changed.count(e.common.layer))
            redraw.push_back(&e);
    });
    upsertMany(redraw);
}

void SceneSync::rebuildDrawOrder() {
    // Draw order overrides (bring to front / send to back) change the order of existing
    // entities: rebuild every entity with its position in the document's draw order.
    std::vector<EntityId> order;
    for (EntityId id : doc_.modelSpace()) {
        const Entity* e = doc_.find(id);
        if (e && e->common.visible && !hiddenByLevel(*e))
            order.push_back(id);
    }
    scene_.removeMany(order);  // one pass instead of one per object
    std::uint32_t rank = 0;
    for (EntityId id : order) {
        const Entity* e = doc_.find(id);
        geom::DisplayContext ctx;
        ctx.pixelSizeMm = pixelSizeMm_;
        ctx.showTransparency = showTransparency_;
        DisplayList list;
        {
            blocks::BlockDisplayScope scope(blockCache_);
            geom::buildDisplay(doc_, *e, ctx, list);
        }
        if (doc_.effective(e->common).locked)
            fade(list);
        scene_.upsert(id, list, ++rank, needsTessellation(*e) ? 1.0 / pixelSizeMm_ : 0.0);
    }
}

// A wipeout was added, removed or moved in the draw order: everything is drawn in levels between
// the wipeouts, so each object is built again (with the draw order it has).
void SceneSync::relevelMasks() {
    if (!scene_.needsRelevel())
        return;
    scene_.adoptMaskOrders();
    std::vector<EntityId> ids;
    doc_.forEachEntity([&](const Entity& e) {
        if (scene_.contains(e.common.id))
            ids.push_back(e.common.id);
    });
    std::vector<const Entity*> redraw;
    for (EntityId id : ids)
        if (const Entity* e = doc_.find(id))
            redraw.push_back(e);
    upsertMany(redraw);
}

void SceneSync::onChanged(const ChangeSet& cs) {
    struct Relevel {
        SceneSync& s;
        ~Relevel() { s.relevelMasks(); }
    } relevel{*this};
    bool blockTouched = !cs.blocks.empty();
    std::vector<EntityId> removed;
    for (const EntityChange& c : cs.entities)
        if (!cs.after(c))
            removed.push_back(cs.idOf(c));
    scene_.removeMany(removed);
    std::vector<const Entity*> redraw;
    for (const EntityChange& c : cs.entities) {
        const Entity* after = cs.after(c);
        if (!after)
            continue;
        if (doc_.ownerBlock(after->common.id)) {
            blockTouched = true;
            continue;
        }
        const Entity* live = doc_.find(after->common.id);
        if (live)
            redraw.push_back(live);
    }
    upsertMany(redraw);
    if (!fieldTexts_.empty() && (!cs.entities.empty() || cs.header)) {
        std::vector<EntityId> ids(fieldTexts_.begin(), fieldTexts_.end());
        for (EntityId id : ids)
            if (const Entity* e = doc_.find(id))
                upsertEntity(*e);
            else
                fieldTexts_.erase(id);
    }
    if (!cs.layers.empty() || cs.header)
        syncLayers();
    if (!cs.levels.empty() ||
        (cs.header && (cs.header->before.currentLevel != cs.header->after.currentLevel ||
                       cs.header->before.levelView != cs.header->after.levelView))) {
        // Level visibility, the current level or the view of other levels changed.
        std::vector<const Entity*> all;
        doc_.forEachEntity([&](const Entity& e) {
            if (!doc_.ownerBlock(e.common.id))
                all.push_back(&e);
        });
        upsertMany(all);
    }
    if (blockTouched) {
        // Inserts show the contents of their block: redraw them.
        std::vector<const Entity*> inserts;
        doc_.forEachEntity([&](const Entity& e) {
            if (kind(e) == EntityKind::Insert)
                inserts.push_back(&e);
        });
        upsertMany(inserts);
    }
    if (!cs.order.empty())
        rebuildDrawOrder();
}

void SceneSync::rebuildAll(double tileSizeMm) {
    scene_.clearAndRetile(tileSizeMm);
    lockedLayers_.clear();
    layerTransparency_.clear();
    for (const auto& [id, layer] : doc_.layers()) {
        layerTransparency_[id] = layer.transparency;
        if (doc_.effective(id).locked)
            lockedLayers_.insert(id);
    }
    render::syncLayerVisibility(scene_, doc_);
    doc_.forEachEntity([&](const Entity& e) { upsertEntity(e); });
    relevelMasks();
}

std::size_t SceneSync::refreshStale(const render::Camera2D& camera, std::size_t limit) {
    std::vector<EntityId> stale = scene_.staleEntities(camera);
    if (stale.empty())
        return 0;
    pixelSizeMm_ = camera.pixelSizeMm();
    std::vector<const Entity*> redraw;
    for (EntityId id : stale) {
        if (redraw.size() >= limit)
            break;
        if (const Entity* e = doc_.find(id))
            redraw.push_back(e);
    }
    upsertMany(redraw);
    return redraw.size();
}

}  // namespace tbcad::ui

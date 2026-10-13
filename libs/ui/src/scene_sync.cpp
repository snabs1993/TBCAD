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

void SceneSync::upsertEntity(const Entity& e) {
    const EntityId id = e.common.id;
    if (!e.common.visible || doc_.ownerBlock(id)) {
        scene_.remove(id);
        return;
    }
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = pixelSizeMm_;
    DisplayList list;
    {
        blocks::BlockDisplayScope scope(blockCache_);
        geom::buildDisplay(doc_, e, ctx, list);
    }
    if (doc_.effective(e.common.layer).locked)
        fade(list);
    scene_.upsert(id, list, drawOrderOf(id), needsTessellation(e) ? 1.0 / pixelSizeMm_ : 0.0);
}

void SceneSync::syncLayers() {
    render::syncLayerVisibility(scene_, doc_);
    // Entities whose layer became locked or unlocked change appearance.
    std::unordered_set<LayerId> locked;
    for (const auto& [id, layer] : doc_.layers())
        if (doc_.effective(id).locked)
            locked.insert(id);
    if (locked == lockedLayers_)
        return;
    std::unordered_set<LayerId> changed;
    for (LayerId id : locked)
        if (!lockedLayers_.count(id))
            changed.insert(id);
    for (LayerId id : lockedLayers_)
        if (!locked.count(id))
            changed.insert(id);
    lockedLayers_ = std::move(locked);
    doc_.forEachEntity([&](const Entity& e) {
        if (changed.count(e.common.layer))
            upsertEntity(e);
    });
}

void SceneSync::rebuildDrawOrder() {
    // Draw order overrides (bring to front / send to back) change the order of existing
    // entities: rebuild every entity with its position in the document's draw order.
    std::uint32_t rank = 0;
    for (EntityId id : doc_.modelSpace()) {
        const Entity* e = doc_.find(id);
        if (!e || !e->common.visible)
            continue;
        geom::DisplayContext ctx;
        ctx.pixelSizeMm = pixelSizeMm_;
        DisplayList list;
        {
            blocks::BlockDisplayScope scope(blockCache_);
            geom::buildDisplay(doc_, *e, ctx, list);
        }
        if (doc_.effective(e->common.layer).locked)
            fade(list);
        scene_.upsert(id, list, ++rank, needsTessellation(*e) ? 1.0 / pixelSizeMm_ : 0.0);
    }
}

void SceneSync::onChanged(const ChangeSet& cs) {
    bool blockTouched = !cs.blocks.empty();
    for (const EntityChange& c : cs.entities) {
        const Entity* after = cs.after(c);
        if (!after) {
            scene_.remove(cs.idOf(c));
            continue;
        }
        if (doc_.ownerBlock(after->common.id)) {
            blockTouched = true;
            continue;
        }
        const Entity* live = doc_.find(after->common.id);
        if (live)
            upsertEntity(*live);
    }
    if (!cs.layers.empty() || cs.header)
        syncLayers();
    if (blockTouched) {
        // Inserts show the contents of their block: redraw them.
        doc_.forEachEntity([&](const Entity& e) {
            if (kind(e) == EntityKind::Insert)
                upsertEntity(e);
        });
    }
    if (!cs.order.empty())
        rebuildDrawOrder();
}

void SceneSync::rebuildAll() {
    scene_.clear();
    lockedLayers_.clear();
    for (const auto& [id, layer] : doc_.layers())
        if (doc_.effective(id).locked)
            lockedLayers_.insert(id);
    render::syncLayerVisibility(scene_, doc_);
    doc_.forEachEntity([&](const Entity& e) { upsertEntity(e); });
}

std::size_t SceneSync::refreshStale(const render::Camera2D& camera, std::size_t limit) {
    std::vector<EntityId> stale = scene_.staleEntities(camera);
    if (stale.empty())
        return 0;
    pixelSizeMm_ = camera.pixelSizeMm();
    std::size_t n = 0;
    for (EntityId id : stale) {
        if (n >= limit)
            break;
        if (const Entity* e = doc_.find(id)) {
            upsertEntity(*e);
            ++n;
        }
    }
    return n;
}

}  // namespace tbcad::ui

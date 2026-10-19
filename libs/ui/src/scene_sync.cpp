#include "tbcad/ui/scene_sync.h"

#include <algorithm>
#include <future>
#include <thread>
#include <unordered_set>

#include "tbcad/geom/entity.h"
#include "tbcad/render/layer_sync.h"
#include "tbcad/text/text.h"

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

bool SceneSync::readDisplayModes() {
    const bool fill = fillMode(doc_.header());
    const int att = attDisplay(doc_.header());
    const bool qt = quickText(doc_.header());
    const bool changed = fill != fillMode_ || att != attDisplay_ || qt != quickText_;
    quickText_ = qt;
    fillMode_ = fill;
    attDisplay_ = att;
    return changed;
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
    ctx.fillMode = fillMode_;
    ctx.attDisplay = attDisplay_;
    ctx.quickText = quickText_;
    const bool locked = doc_.effective(e.common).locked;
    // An object already in the scene keeps its place in the draw order.
    const std::uint32_t order = drawOrder.value_or(orderFor(id));
    const double builtZoom = needsTessellation(e) ? 1.0 / pixelSizeMm_ : 0.0;
    DisplayList list;
    {
        blocks::BlockDisplayScope scope(blockCache_);
        // A reference to a block is drawn from the block's shared geometry and one transform.
        if (!locked && kind(e) == EntityKind::Insert) {
            blocks::InsertInstance inst;
            if (blocks::buildInsertInstance(doc_, e, ctx, inst) &&
                scene_.upsertInstance(id, inst.blockSpace, inst.placements, &inst.extra, order,
                                      builtZoom))
                return;
        }
        geom::buildDisplay(doc_, e, ctx, list);
    }
    if (locked)
        fade(list);
    scene_.upsert(id, list, order, builtZoom);
}

void SceneSync::prepare(const Entity& e, std::uint32_t order, Prepared& out) const {
    out.entity = &e;
    out.order = order;
    const EntityId id = e.common.id;
    if (!e.common.visible || doc_.ownerBlock(id) || hiddenByLevel(e)) {
        out.what = Prepared::What::Remove;
        return;
    }
    out.fields = holdsFields(e);
    // References to blocks share geometry through the (single threaded) block cache.
    if (kind(e) == EntityKind::Insert) {
        out.what = Prepared::What::Defer;
        return;
    }
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = pixelSizeMm_;
    ctx.showTransparency = showTransparency_;
    ctx.fillMode = fillMode_;
    ctx.attDisplay = attDisplay_;
    ctx.quickText = quickText_;
    out.builtZoom = needsTessellation(e) ? 1.0 / pixelSizeMm_ : 0.0;
    geom::buildDisplay(doc_, e, ctx, out.list);
    if (doc_.effective(e.common).locked)
        fade(out.list);
    out.what = Prepared::What::List;
}

void SceneSync::commit(Prepared& p) {
    const EntityId id = p.entity->common.id;
    switch (p.what) {
        case Prepared::What::Remove:
            scene_.remove(id);
            fieldTexts_.erase(id);
            return;
        case Prepared::What::Defer:
            upsertEntity(*p.entity, p.order);
            return;
        case Prepared::What::List:
            if (p.fields)
                fieldTexts_.insert(id);
            else
                fieldTexts_.erase(id);
            scene_.upsert(id, p.list, p.order, p.builtZoom);
            return;
    }
}

void SceneSync::upsertParallel(const std::vector<const Entity*>& list,
                               const std::vector<std::uint32_t>& orders) {
    constexpr std::size_t kChunk = 8192;
    const std::size_t lanes = std::clamp<std::size_t>(
        maxThreads_ ? maxThreads_ : std::thread::hardware_concurrency(), 1, 16);
    if (list.size() < 2 * kChunk || lanes < 2) {
        for (std::size_t i = 0; i < list.size(); ++i)
            upsertEntity(*list[i], orders[i]);
        return;
    }
    // Several threads read the document: the lookups that fill a cache on first use are done now.
    for (const auto& [id, layer] : doc_.layers())
        (void)doc_.effective(id);
    auto buildChunk = [&](std::size_t from, std::size_t to) {
        std::vector<Prepared> out(to - from);
        const std::size_t n = to - from;
        const std::size_t parts = std::min(lanes, (n + 255) / 256);
        std::vector<std::future<void>> jobs;
        auto run = [&](std::size_t a, std::size_t b) {
            text::FontScope fonts;  // texts are laid out without waiting for the other threads
            for (std::size_t i = a; i < b; ++i)
                prepare(*list[from + i], orders[from + i], out[i]);
        };
        for (std::size_t k = 1; k < parts; ++k)
            jobs.push_back(std::async(std::launch::async, run, n * k / parts, n * (k + 1) / parts));
        run(0, n / parts);
        for (auto& j : jobs)
            j.get();
        return out;
    };
    // The next chunk is built while the scene takes the last one.
    std::size_t from = 0;
    std::vector<Prepared> current = buildChunk(0, std::min(kChunk, list.size()));
    while (from < list.size()) {
        const std::size_t next = from + current.size();
        std::future<std::vector<Prepared>> ahead;
        if (next < list.size())
            ahead = std::async(std::launch::async, buildChunk, next,
                               std::min(next + kChunk, list.size()));
        for (Prepared& p : current)
            commit(p);
        from = next;
        if (ahead.valid())
            current = ahead.get();
    }
}

// Where an object goes in the draw order: where it already is, or where it was waiting to be
// built, or by its id.
std::uint32_t SceneSync::orderFor(EntityId id) {
    if (scene_.contains(id))
        return scene_.drawOrderOf(id);
    if (!pending_.empty())
        if (auto o = takePending(id))
            return *o;
    return drawOrderOf(id);
}

std::optional<std::uint32_t> SceneSync::takePending(EntityId id) {
    auto it = std::lower_bound(pending_.begin(), pending_.end(), id,
                               [](const PendingRec& r, EntityId v) { return r.id < v; });
    if (it == pending_.end() || it->id != id || it->dead)
        return std::nullopt;
    it->dead = true;
    ++pendingDead_;
    return it->order;
}

// Many objects went away at once: when both lists are in id order, one pass marks them.
void SceneSync::dropPending(const std::vector<EntityId>& ids) {
    if (ids.size() < 64 || !std::is_sorted(ids.begin(), ids.end())) {
        for (EntityId id : ids)
            (void)takePending(id);
        return;
    }
    auto p = pending_.begin();
    for (EntityId id : ids) {
        while (p != pending_.end() && p->id < id)
            ++p;
        if (p == pending_.end())
            break;
        if (p->id == id && !p->dead) {
            p->dead = true;
            ++pendingDead_;
        }
    }
}

void SceneSync::compactPending() {
    if (pendingDead_ == 0)
        return;
    std::erase_if(pending_, [](const PendingRec& r) { return r.dead; });
    pendingDead_ = 0;
}

bool SceneSync::lazyOk(std::size_t count) const {
    constexpr std::size_t kLazyMin = 20000;
    // Wipeouts split the draw order into levels that every object depends on: build at once.
    return count >= kLazyMin && !scene_.needsRelevel() && !scene_.hasMasks();
}

void SceneSync::queueLazy(const std::vector<const Entity*>& list) {
    std::vector<const Entity*> sorted = list;
    if (!std::is_sorted(sorted.begin(), sorted.end(), [](const Entity* a, const Entity* b) {
            return a->common.id < b->common.id;
        }))
        std::sort(sorted.begin(), sorted.end(),
                  [](const Entity* a, const Entity* b) { return a->common.id < b->common.id; });
    std::vector<EntityId> ids;
    ids.reserve(sorted.size());
    std::vector<PendingRec> fresh;
    fresh.reserve(sorted.size());
    std::size_t at = 0;  // walks pending_ in step with the sorted list
    for (const Entity* e : sorted) {
        const EntityId id = e->common.id;
        ids.push_back(id);
        PendingRec rec;
        rec.id = id;
        rec.box = geom::bounds(*e, doc_);
        if (scene_.contains(id)) {
            rec.order = scene_.drawOrderOf(id);
        } else {
            while (at < pending_.size() && pending_[at].id < id)
                ++at;
            rec.order = (at < pending_.size() && pending_[at].id == id && !pending_[at].dead)
                            ? pending_[at].order
                            : drawOrderOf(id);
        }
        fresh.push_back(rec);
    }
    if (!scene_.removeAllOf(ids))
        scene_.removeMany(ids);
    // Merge into the waiting ones; a newer record replaces an older one of the same object.
    std::vector<PendingRec> merged;
    merged.reserve(pending_.size() + fresh.size());
    std::size_t i = 0, j = 0;
    while (i < pending_.size() || j < fresh.size()) {
        if (i < pending_.size() && pending_[i].dead) {
            ++i;
        } else if (j == fresh.size() || (i < pending_.size() && pending_[i].id < fresh[j].id)) {
            merged.push_back(pending_[i++]);
        } else if (i == pending_.size() || fresh[j].id < pending_[i].id) {
            merged.push_back(fresh[j++]);
        } else {
            merged.push_back(fresh[j++]);
            ++i;
        }
    }
    pending_ = std::move(merged);
    pendingDead_ = 0;
}

void SceneSync::buildPending(PendingRec& rec) {
    rec.dead = true;
    ++pendingDead_;
    if (const Entity* e = doc_.find(rec.id))
        upsertEntity(*e, rec.order);
}

std::size_t SceneSync::pumpPending(const render::Camera2D& camera, std::size_t limit) {
    if (pendingCount() == 0) {
        pending_.clear();
        pendingDead_ = 0;
        return 0;
    }
    pixelSizeMm_ = camera.pixelSizeMm();
    BBox2 view = camera.visibleWorld();
    const double pad = 0.1 * std::max(view.width(), view.height());
    view = view.inflated(pad);
    std::size_t built = 0;
    for (PendingRec& rec : pending_) {
        if (built >= limit)
            break;
        if (!rec.dead && (rec.box.empty() || view.intersects(rec.box))) {
            buildPending(rec);
            ++built;
        }
    }
    if (built == 0) {  // nothing in view: use the time to catch up on the rest
        for (PendingRec& rec : pending_) {
            if (built >= limit / 2)
                break;
            if (!rec.dead) {
                buildPending(rec);
                ++built;
            }
        }
    }
    if (pendingDead_ * 2 > pending_.size())
        compactPending();
    return built;
}

void SceneSync::flushPending() {
    for (PendingRec& rec : pending_)
        if (!rec.dead)
            buildPending(rec);
    pending_.clear();
    pendingDead_ = 0;
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
            orders.push_back(orderFor(id));
        }
        scene_.removeMany(ids);
        if (to - from >= kChunk) {
            upsertParallel(
                std::vector<const Entity*>(list.begin() + static_cast<std::ptrdiff_t>(from),
                                           list.begin() + static_cast<std::ptrdiff_t>(to)),
                orders);
        } else {
            for (std::size_t i = from; i < to; ++i)
                upsertEntity(*list[i], orders[i - from]);
        }
    }
}

void SceneSync::syncLayers() {
    // Visibility is a property of the scene's layer, so objects still waiting to be built need no
    // flush: they come out hidden or shown by the layer they are on.
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
    if (lazyOk(redraw.size()))
        queueLazy(redraw);
    else
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
    if (lazyOk(order.size())) {
        // A big drawing: only the new ranks are recorded, and the objects are built camera first
        // like any other bulk change (an undo of ERASE ALL with a few objects sent to back must
        // not build the whole drawing in one go).
        scene_.removeMany(order);
        std::vector<PendingRec> recs;
        recs.reserve(order.size());
        std::uint32_t rank = 0;
        for (EntityId id : order) {
            PendingRec rec;
            rec.id = id;
            rec.box = geom::bounds(*doc_.find(id), doc_);
            rec.order = ++rank;
            recs.push_back(rec);
        }
        std::sort(recs.begin(), recs.end(),
                  [](const PendingRec& a, const PendingRec& b) { return a.id < b.id; });
        pending_ = std::move(recs);
        pendingDead_ = 0;
        return;
    }
    flushPending();
    scene_.removeMany(order);  // one pass instead of one per object
    std::uint32_t rank = 0;
    for (EntityId id : order) {
        const Entity* e = doc_.find(id);
        geom::DisplayContext ctx;
        ctx.pixelSizeMm = pixelSizeMm_;
        ctx.showTransparency = showTransparency_;
        ctx.fillMode = fillMode_;
        ctx.attDisplay = attDisplay_;
        ctx.quickText = quickText_;
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
    flushPending();
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
    if (!scene_.removeAllOf(removed))
        scene_.removeMany(removed);
    if (!pending_.empty())
        dropPending(removed);
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
    // Changes of what is already drawn wait. New objects are built at once, so the scene does
    // not lag behind a paste, unless there are so many (an undo of ERASE ALL) that building them
    // all in one go is a long stall: those wait too, camera first.
    constexpr std::size_t kNewWaitMin = 100000;
    const bool lazy = lazyOk(redraw.size()) &&
                      std::none_of(redraw.begin(), redraw.end(),
                                   [](const Entity* e) { return e->common.wipeout; }) &&
                      (redraw.size() >= kNewWaitMin ||
                       2 * static_cast<std::size_t>(std::count_if(
                               redraw.begin(), redraw.end(),
                               [&](const Entity* e) { return scene_.contains(e->common.id); })) >=
                           redraw.size());
    if (lazy)
        queueLazy(redraw);
    else
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
    const bool modesChanged = cs.header && readDisplayModes();
    if (!cs.levels.empty() || modesChanged ||
        (cs.header && (cs.header->before.currentLevel != cs.header->after.currentLevel ||
                       cs.header->before.levelView != cs.header->after.levelView))) {
        // Level visibility, the current level or the view of other levels changed.
        flushPending();
        std::vector<const Entity*> all;
        doc_.forEachEntity([&](const Entity& e) {
            if (!doc_.ownerBlock(e.common.id))
                all.push_back(&e);
        });
        upsertMany(all);
    }
    if (!cs.dimStyles.empty() || !cs.textStyles.empty()) {
        // A style changed (or came back by undo): what is drawn with it is built again. Dimension
        // styles reach dimensions and leaders, text styles also text, tables and their notes.
        flushPending();
        const bool text = !cs.textStyles.empty();
        // Only what is drawn with a style that changed (an unused one purged or restored changes
        // no text at all); a text without a style follows the current one.
        std::unordered_set<TextStyleId> changedStyles;
        for (const auto& c : cs.textStyles) {
            if (c.before)
                changedStyles.insert(c.before->id);
            if (c.after)
                changedStyles.insert(c.after->id);
        }
        if (cs.header && cs.header->before.currentTextStyle != cs.header->after.currentTextStyle) {
            changedStyles.insert(cs.header->before.currentTextStyle);
            changedStyles.insert(cs.header->after.currentTextStyle);
        }
        const TextStyleId current = doc_.header().currentTextStyle;
        auto usesChanged = [&](TextStyleId sid) {
            return changedStyles.count(sid.value != 0 ? sid : current) != 0;
        };
        std::vector<const Entity*> styled;
        doc_.forEachEntity([&](const Entity& e) {
            if (doc_.ownerBlock(e.common.id))
                return;
            switch (kind(e)) {
                case EntityKind::Dimension:
                case EntityKind::Leader:
                case EntityKind::MLeader:
                    styled.push_back(&e);
                    break;
                case EntityKind::Text:
                    if (text && usesChanged(entityAs<TextData>(e)->style))
                        styled.push_back(&e);
                    break;
                case EntityKind::MText:
                    if (text && usesChanged(entityAs<MTextData>(e)->style))
                        styled.push_back(&e);
                    break;
                case EntityKind::Table:  // cells may name styles of their own
                    if (text)
                        styled.push_back(&e);
                    break;
                default:
                    break;
            }
        });
        if (lazyOk(styled.size()))
            queueLazy(styled);
        else
            upsertMany(styled);
        blockTouched = true;  // inserts show blocks that hold such objects
    }
    if (blockTouched) {
        // Inserts show the contents of their block: redraw them.
        flushPending();
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
    readDisplayModes();
    pending_.clear();
    pendingDead_ = 0;
    scene_.clearAndRetile(tileSizeMm);
    lockedLayers_.clear();
    layerTransparency_.clear();
    for (const auto& [id, layer] : doc_.layers()) {
        layerTransparency_[id] = layer.transparency;
        if (doc_.effective(id).locked)
            lockedLayers_.insert(id);
    }
    render::syncLayerVisibility(scene_, doc_);
    std::vector<const Entity*> all;
    std::vector<std::uint32_t> orders;
    doc_.forEachEntity([&](const Entity& e) {
        all.push_back(&e);
        orders.push_back(drawOrderOf(e.common.id));
    });
    upsertParallel(all, orders);
    relevelMasks();
}

std::size_t SceneSync::refreshStale(const render::Camera2D& camera, std::size_t limit) {
    std::vector<EntityId> stale = scene_.staleEntities(camera);
    if (stale.empty())
        return 0;
    pixelSizeMm_ = camera.pixelSizeMm();
    // A tile holds objects far outside the view; those wait until the camera comes to them.
    const BBox2 view = camera.visibleWorld().inflated(8.0 * pixelSizeMm_);
    skippedOffscreen_ = false;
    std::vector<const Entity*> redraw;
    for (EntityId id : stale) {
        if (redraw.size() >= limit)
            break;
        const Entity* e = doc_.find(id);
        if (!e)
            continue;
        if (!geom::bounds(*e, doc_).intersects(view)) {
            skippedOffscreen_ = true;
            continue;
        }
        redraw.push_back(e);
    }
    upsertMany(redraw);
    return redraw.size();
}

}  // namespace tbcad::ui

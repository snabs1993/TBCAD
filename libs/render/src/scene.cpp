#include "tbcad/render/scene.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "scene_cursor.h"
#include "tbcad/render/fill.h"

namespace tbcad::render {
namespace {

constexpr double kMaxCoordinate = 1e15;       // beyond this nothing is drawable in doubles anyway
constexpr double kDashedPiecePeriods = 1000;  // split dashed segments longer than this many periods

std::uint32_t packRgba(Rgba c) {
    return static_cast<std::uint32_t>(c.r) | static_cast<std::uint32_t>(c.g) << 8 |
           static_cast<std::uint32_t>(c.b) << 16 | static_cast<std::uint32_t>(c.a) << 24;
}

bool drawable(const Vec2& p) {
    return isFinite(p) && std::abs(p.x) < kMaxCoordinate && std::abs(p.y) < kMaxCoordinate;
}

// Batch ids are unique across all scenes, so one Renderer can serve several of them.
std::uint64_t nextBatchId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

const BBox2& Tile::geometryBounds() const {
    if (boundsDirty) {
        BBox2 b;
        for (const auto& [batchKey, batch] : batches) {
            for (const GpuVertex& v : batch.verts) {
                b.expand({origin.x + static_cast<double>(v.hx) + static_cast<double>(v.lx),
                          origin.y + static_cast<double>(v.hy) + static_cast<double>(v.ly)});
            }
        }
        bounds = b;
        boundsDirty = false;
    }
    return bounds;
}

Scene::Scene(Options options) : tileSize_(options.tileSizeMm > 0 ? options.tileSizeMm : 1024) {
    patterns_.emplace_back();  // 0 = continuous
    patternIds_[{}] = 0;
}

double Scene::suggestTileSize(const BBox2& extent) {
    double size = 1024;
    if (!extent.empty()) {
        double longest = std::max(extent.width(), extent.height());
        size = std::exp2(std::ceil(std::log2(std::max(longest / 8.0, 1.0))));
    }
    return std::clamp(size, 16.0, 16777216.0);
}

TileKey Scene::tileOf(const Vec2& p) const {
    return {static_cast<std::int64_t>(std::floor(p.x / tileSize_)),
            static_cast<std::int64_t>(std::floor(p.y / tileSize_))};
}

Batch& Scene::batchFor(const TileKey& tk, const BatchKey& key, double halfWidthMm) {
    auto [tit, newTile] = tiles_.try_emplace(tk);
    Tile& tile = tit->second;
    if (newTile) {
        tile.key = tk;
        tile.origin = {static_cast<double>(tk.x) * tileSize_,
                       static_cast<double>(tk.y) * tileSize_};
    }
    tile.maxHalfWidthMm = std::max(tile.maxHalfWidthMm, halfWidthMm);
    auto [bit, newBatch] = tile.batches.try_emplace(key);
    if (newBatch) {
        bit->second.id = nextBatchId();
        bit->second.key = key;
    }
    return bit->second;
}

std::uint32_t Scene::internPattern(const DisplayStyle& style) {
    if (!style.pattern || style.pattern->empty())
        return 0;
    double scale = style.patternScale > 0 ? style.patternScale : 1.0;
    std::vector<double> scaled;
    scaled.reserve(style.pattern->size());
    for (double e : *style.pattern)
        scaled.push_back(e * scale);
    DashPattern p{scaled};
    if (p.continuous())
        return 0;
    auto it = patternIds_.find(scaled);
    if (it != patternIds_.end())
        return it->second;
    auto id = static_cast<std::uint32_t>(patterns_.size());
    patterns_.push_back(p);
    patternIds_.emplace(std::move(scaled), id);
    return id;
}

BatchKey Scene::styleKey(BatchKind kind, const DisplayStyle& style, std::uint32_t level) {
    BatchKey k;
    k.kind = kind;
    k.level = level;
    k.layer = style.layer;
    k.rgba = packRgba(style.color);
    if (kind == BatchKind::Stroke) {
        k.lineweightUm =
            static_cast<std::int32_t>(std::lround(std::max(0.0, style.lineweight) * 1000));
        k.pattern = internPattern(style);
    }
    return k;
}

void Scene::addPolyline(Cursor& c, const DisplayPolyline& pl) {
    const auto& pts = pl.pts;
    const std::size_t n = pts.size();
    if (n < 2)
        return;
    BatchKey key = styleKey(BatchKind::Stroke, pl.style, c.level);
    const double halfWidth = std::max(0.0, pl.style.lineweight) * 0.5;
    const double period = patterns_[key.pattern].period();
    const double maxPiece = period > 0 ? period * kDashedPiecePeriods : 0;
    const std::size_t segments = (pl.closed && n > 2) ? n : n - 1;

    double cum = 0;  // distance along the polyline (mm)
    for (std::size_t i = 0; i < segments; ++i) {
        const Vec2 a = pts[i];
        const Vec2 b = pts[(i + 1) % n];
        if (!drawable(a) || !drawable(b))
            continue;
        const double len = distance(a, b);
        if (len <= 0)
            continue;
        const int pieces = maxPiece > 0 ? static_cast<int>(std::ceil(len / maxPiece)) : 1;
        for (int p = 0; p < pieces; ++p) {
            const double t0 = static_cast<double>(p) / pieces;
            const double t1 = static_cast<double>(p + 1) / pieces;
            const Vec2 pa = pieces == 1 ? a : lerp(a, b, t0);
            const Vec2 pb = pieces == 1 ? b : lerp(a, b, t1);
            const double plen = len / pieces;
            const double d0 = wrapDashDistance(cum, period);
            const TileKey tk = tileOf(pa);
            // The tile may not exist yet; append() creates it and sets c.tile.
            const Vec2 origin = {static_cast<double>(tk.x) * tileSize_,
                                 static_cast<double>(tk.y) * tileSize_};
            const GpuVertex v[2] = {packVertex(pa, origin, period > 0 ? d0 : 0),
                                    packVertex(pb, origin, period > 0 ? d0 + plen : 0)};
            c.append(*this, tk, key, halfWidth, v);
            c.expand(pa);
            c.expand(pb);
            cum += plen;
        }
    }
}

void Scene::addFill(Cursor& c, const DisplayFill& fill) {
    std::vector<Vec2> tris = triangulateEvenOdd(fill.rings);
    if (tris.empty())
        return;
    const bool mask = isBackgroundColor(fill.style.color);
    BatchKey key = mask ? styleKey(BatchKind::Mask, fill.style, c.maskLevel)
                        : styleKey(BatchKind::Fill, fill.style, c.level);
    for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
        const Vec2 centroid = (tris[i] + tris[i + 1] + tris[i + 2]) / 3.0;
        if (!drawable(centroid))
            continue;
        const TileKey tk = tileOf(centroid);
        const Vec2 origin = {static_cast<double>(tk.x) * tileSize_,
                             static_cast<double>(tk.y) * tileSize_};
        const GpuVertex v[3] = {packVertex(tris[i], origin), packVertex(tris[i + 1], origin),
                                packVertex(tris[i + 2], origin)};
        c.append(*this, tk, key, 0, v);
        c.expand(tris[i]);
        c.expand(tris[i + 1]);
        c.expand(tris[i + 2]);
    }
}

void Scene::addPoints(Cursor& c, const DisplayPoints& pts) {
    BatchKey key = styleKey(BatchKind::Point, pts.style, c.level);
    for (const Vec2& p : pts.pts) {
        if (!drawable(p))
            continue;
        const TileKey tk = tileOf(p);
        const Vec2 origin = {static_cast<double>(tk.x) * tileSize_,
                             static_cast<double>(tk.y) * tileSize_};
        const GpuVertex v[1] = {packVertex(p, origin)};
        c.append(*this, tk, key, 0, v);
        c.expand(p);
    }
}

void Scene::upsert(EntityId id, const DisplayList& list, std::uint32_t drawOrder,
                   double builtPixelsPerMm) {
    remove(id);
    EntityRec& rec = entities_[id];
    rec.drawOrder = drawOrder;
    // A wipeout inside a block hides what the block drew before it and not what it draws after
    // it, so every run of mask fills is a mask of its own with its own level.
    {
        bool inMask = false;
        for (const DisplayItem& item : list.items) {
            const auto* f = std::get_if<DisplayFill>(&item);
            const bool isMask = f && isBackgroundColor(f->style.color);
            if (isMask && !inMask)
                ++rec.maskCount;
            inMask = isMask;
        }
        rec.mask = rec.maskCount > 0;
    }
    if (rec.mask)
        masks_[id] = {drawOrder, rec.maskCount};
    Cursor c;
    c.entity = id;
    c.rec = &rec;
    c.drawOrder = drawOrder;
    const std::uint32_t base = levelBelow(drawOrder);
    c.level = base;
    c.maskLevel = base;
    c.builtZoom = static_cast<float>(builtPixelsPerMm);
    bool inMask = false;
    std::uint32_t seen = 0;  // masks of this entity passed so far
    for (const DisplayItem& item : list.items) {
        const auto* mf = std::get_if<DisplayFill>(&item);
        const bool isMask = mf && isBackgroundColor(mf->style.color);
        if (isMask && !inMask)
            ++seen;
        inMask = isMask;
        // Geometry after the n-th mask of the entity lies above it.
        c.level = base + seen;
        c.maskLevel = base + (seen ? seen - 1 : 0);
        // Moving to another item may change tile and style; force a fresh lookup.
        c.batch = nullptr;
        c.tile = nullptr;
        if (const auto* pl = std::get_if<DisplayPolyline>(&item))
            addPolyline(c, *pl);
        else if (const auto* f = std::get_if<DisplayFill>(&item))
            addFill(c, *f);
        else if (const auto* p = std::get_if<DisplayPoints>(&item))
            addPoints(c, *p);
    }
    finishRefs(rec);
    ++changeCounter_;
}

std::uint32_t Scene::internRef(const TileKey& tile, const BatchKey& key) {
    auto [it, fresh] =
        refIds_.try_emplace(RefPair{tile, key}, static_cast<std::uint32_t>(refPairs_.size()));
    if (fresh)
        refPairs_.push_back(it->first);
    return it->second;
}

void Scene::finishRefs(EntityRec& rec) {
    if (rec.refs.size() < 2)
        return;
    std::sort(rec.refs.begin(), rec.refs.end(),
              [this](std::uint32_t a, std::uint32_t b) { return refPairs_[a] < refPairs_[b]; });
    // The same pair has the same number, so equal numbers are the repeats.
    rec.refs.truncate(static_cast<std::uint32_t>(std::unique(rec.refs.begin(), rec.refs.end()) -
                                                 rec.refs.begin()));
}

// Masks drawn before something with this draw order.
std::uint32_t Scene::levelBelow(std::uint32_t drawOrder) const {
    return static_cast<std::uint32_t>(
        std::lower_bound(levelOrders_.begin(), levelOrders_.end(), drawOrder) -
        levelOrders_.begin());
}

std::vector<std::uint32_t> Scene::maskOrders() const {
    std::vector<std::uint32_t> orders;
    orders.reserve(masks_.size());
    for (const auto& [id, om] : masks_)
        orders.insert(orders.end(), om.second, om.first);
    std::sort(orders.begin(), orders.end());
    return orders;
}

bool Scene::needsRelevel() const {
    return !(masks_.empty() && levelOrders_.empty()) && maskOrders() != levelOrders_;
}

void Scene::adoptMaskOrders() {
    levelOrders_ = maskOrders();
}

std::uint32_t Scene::drawOrderOf(EntityId id) const {
    auto it = entities_.find(id);
    return it == entities_.end() ? 0 : it->second.drawOrder;
}

void Scene::eraseRefs(EntityId id, const EntityRec& rec) {
    for (std::uint32_t refId : rec.refs) {
        const auto& [tk, bk] = refPair(refId);
        auto tit = tiles_.find(tk);
        if (tit == tiles_.end())
            continue;
        Tile& tile = tit->second;
        auto bit = tile.batches.find(bk);
        if (bit == tile.batches.end())
            continue;
        Batch& batch = bit->second;
        for (std::size_t r = 0; r < batch.ranges.size();) {
            if (batch.ranges[r].entity != id) {
                ++r;
                continue;
            }
            const BatchRange gone = batch.ranges[r];
            batch.verts.erase(batch.verts.begin() + gone.begin,
                              batch.verts.begin() + gone.begin + gone.count);
            batch.ranges.erase(batch.ranges.begin() + static_cast<std::ptrdiff_t>(r));
            for (std::size_t k = r; k < batch.ranges.size(); ++k)
                batch.ranges[k].begin -= gone.count;
        }
        ++batch.version;
        if (batch.ranges.empty()) {
            deleted_.push_back(batch.id);
            tile.batches.erase(bit);
        } else {
            batch.minDrawOrder = batch.ranges[0].drawOrder;
            for (const BatchRange& r : batch.ranges)
                batch.minDrawOrder = std::min(batch.minDrawOrder, r.drawOrder);
        }
        if (tile.batches.empty())
            tiles_.erase(tit);
        else
            tile.boundsDirty = true;
    }
}

void Scene::remove(EntityId id) {
    auto it = entities_.find(id);
    if (it == entities_.end())
        return;
    eraseRefs(id, it->second);
    if (it->second.mask)
        masks_.erase(id);
    entities_.erase(it);
    ++changeCounter_;
}

bool Scene::removeAllOf(std::span<const EntityId> ids) {
    if (ids.size() < 9 || ids.size() != entities_.size())
        return false;
    for (EntityId id : ids)
        if (!entities_.count(id))
            return false;
    discardAll();
    ++changeCounter_;
    return true;
}

void Scene::removeMany(std::span<const EntityId> ids) {
    if (ids.size() <= 8) {
        for (EntityId id : ids)
            remove(id);
        return;
    }
    // Compacts every touched batch once, leaving out the ranges of dropped objects.
    auto compact = [&](const std::set<std::pair<TileKey, BatchKey>>& touched, auto&& dropped) {
        for (const auto& [tk, bk] : touched) {
            auto tit = tiles_.find(tk);
            if (tit == tiles_.end())
                continue;
            Tile& tile = tit->second;
            auto bit = tile.batches.find(bk);
            if (bit == tile.batches.end())
                continue;
            Batch& batch = bit->second;
            std::vector<BatchRange> ranges;
            std::vector<GpuVertex> verts;
            ranges.reserve(batch.ranges.size());
            verts.reserve(batch.verts.size());
            for (BatchRange r : batch.ranges) {
                if (dropped(r.entity))
                    continue;
                const std::uint32_t from = r.begin;
                r.begin = static_cast<std::uint32_t>(verts.size());
                verts.insert(verts.end(), batch.verts.begin() + from,
                             batch.verts.begin() + from + r.count);
                ranges.push_back(r);
            }
            batch.ranges = std::move(ranges);
            batch.verts = std::move(verts);
            ++batch.version;
            if (batch.ranges.empty()) {
                deleted_.push_back(batch.id);
                tile.batches.erase(bit);
            } else {
                batch.minDrawOrder = batch.ranges[0].drawOrder;
                for (const BatchRange& r : batch.ranges)
                    batch.minDrawOrder = std::min(batch.minDrawOrder, r.drawOrder);
            }
            if (tile.batches.empty())
                tiles_.erase(tit);
            else
                tile.boundsDirty = true;
        }
    };
    // Fewer ids than drawn objects: take them out of the scene's map one by one (what is not drawn
    // is skipped) and drop every range whose object is no longer there.
    if (ids.size() < entities_.size()) {
        std::set<std::pair<TileKey, BatchKey>> touched;
        for (EntityId id : ids) {
            auto it = entities_.find(id);
            if (it == entities_.end())
                continue;
            for (std::uint32_t ref : it->second.refs)
                touched.insert(refPair(ref));
            if (it->second.mask)
                masks_.erase(id);
            entities_.erase(it);
        }
        compact(touched, [&](EntityId e) { return entities_.count(e) == 0; });
        ++changeCounter_;
        return;
    }
    // Most ids of a big change are often not in the scene at all (objects that were only queued):
    // look at the scene's side first instead of hashing every id into a set.
    std::vector<EntityId> present;
    if (ids.size() > 4 * entities_.size()) {
        // Far more ids than drawn objects (an undo of EXPLODE drops the many pieces that were still
        // waiting in the queue): walk the scene once against a flat table of the ids instead of
        // looking every id up in the scene's map.
        std::size_t cap = 16;
        while (cap < ids.size() * 2)
            cap <<= 1;
        std::vector<std::uint64_t> table(cap, 0);
        const std::size_t mask = cap - 1;
        auto slot = [&](std::uint64_t v) {
            return static_cast<std::size_t>(v * 0x9E3779B97F4A7C15ull >> 20) & mask;
        };
        for (EntityId id : ids)
            if (id.value) {
                std::size_t i = slot(id.value);
                while (table[i] && table[i] != id.value)
                    i = (i + 1) & mask;
                table[i] = id.value;
            }
        for (const auto& [id, rec] : entities_) {
            std::size_t i = slot(id.value);
            while (table[i] && table[i] != id.value)
                i = (i + 1) & mask;
            if (table[i])
                present.push_back(id);
        }
    } else {
        for (EntityId id : ids)
            if (entities_.count(id))
                present.push_back(id);
    }
    if (present.size() <= 8) {
        for (EntityId id : present)
            remove(id);
        return;
    }
    if (present.size() != ids.size()) {
        removeMany(present);
        return;
    }
    // Erasing one object shifts every later range of its batches, so erasing many one by one is
    // quadratic. Here each touched batch is compacted once.
    const std::unordered_set<EntityId> drop(ids.begin(), ids.end());
    if (drop.size() >= entities_.size()) {
        bool all = true;
        for (const auto& [id, rec] : entities_)
            if (!drop.count(id)) {
                all = false;
                break;
            }
        if (all) {  // everything goes: no need to compact batch by batch
            discardAll();
            ++changeCounter_;
            return;
        }
    }
    std::set<std::pair<TileKey, BatchKey>> touched;
    for (EntityId id : drop) {
        auto it = entities_.find(id);
        if (it == entities_.end())
            continue;
        for (std::uint32_t ref : it->second.refs)
            touched.insert(refPair(ref));
        if (it->second.mask)
            masks_.erase(id);
        entities_.erase(it);
    }
    compact(touched, [&](EntityId e) { return drop.count(e) != 0; });
    ++changeCounter_;
}

std::size_t Scene::trimMemory() {
    std::size_t freed = 0;
    for (auto& [tk, tile] : tiles_)
        for (auto& [bk, batch] : tile.batches) {
            freed += (batch.verts.capacity() - batch.verts.size()) * sizeof(GpuVertex) +
                     (batch.ranges.capacity() - batch.ranges.size()) * sizeof(BatchRange);
            batch.verts.shrink_to_fit();
            batch.ranges.shrink_to_fit();
        }
    return freed;
}

void Scene::discardAll() {
    for (auto& [tk, tile] : tiles_)
        for (auto& [bk, batch] : tile.batches)
            deleted_.push_back(batch.id);
    // Freeing hundreds of thousands of small allocations takes as long as building them (most of
    // a second on Windows), and nothing needs the memory back at once: another thread does it.
    constexpr std::size_t kAsyncMin = 20000;
    if (graveyard_.valid())
        graveyard_.get();
    if (entities_.size() >= kAsyncMin) {
        graveyard_ = std::async(std::launch::async, [tiles = std::move(tiles_),
                                                     entities = std::move(entities_)]() mutable {
            tiles.clear();
            entities.clear();
        });
    }
    tiles_.clear();
    entities_.clear();
    refPairs_.clear();
    refIds_.clear();
    masks_.clear();
    levelOrders_.clear();
}

void Scene::clear() {
    discardAll();
    ++changeCounter_;
}

void Scene::clearAndRetile(double tileSizeMm) {
    clear();
    if (tileSizeMm > 0)
        tileSize_ = tileSizeMm;
}

void Scene::setLayerVisible(LayerId layer, bool visible) {
    bool changed = visible ? hiddenLayers_.erase(layer) != 0 : hiddenLayers_.insert(layer).second;
    if (changed)
        ++changeCounter_;
}

void Scene::setHighlighted(std::span<const EntityId> ids) {
    highlighted_.clear();
    highlighted_.insert(ids.begin(), ids.end());
    ++hl_.stamp;  // not changeCounter_: hovering must not rebuild the selection's geometry
}

void Scene::setSelected(std::span<const EntityId> ids) {
    // A big selection that is dropped (after ERASE, say) is freed on another thread.
    constexpr std::size_t kAsyncMin = 20000;
    if (selGraveyard_.valid())
        selGraveyard_.get();
    if (selected_.size() >= kAsyncMin)
        selGraveyard_ =
            std::async(std::launch::async, [old = std::move(selected_)]() mutable { old.clear(); });
    selected_.clear();
    selected_.reserve(ids.size());
    selected_.insert(ids.begin(), ids.end());
    ++sel_.stamp;
}

const SelectionGeometry& Scene::geometryFor(const std::unordered_set<EntityId>& set,
                                            SelCache& cache) const {
    if (cache.builtFor == changeCounter_ && cache.builtStamp == cache.stamp)
        return cache.geometry;
    cache.builtFor = changeCounter_;
    cache.builtStamp = cache.stamp;
    cache.geometry.version = ++cache.serial;
    cache.geometry.tiles.clear();
    std::map<std::pair<TileKey, std::int32_t>, std::size_t> index;
    auto tileFor = [&](const TileKey& tk, const Tile& tile, const BatchKey& bk) -> SelectionTile& {
        auto [iit, fresh] = index.try_emplace({tk, bk.lineweightUm}, cache.geometry.tiles.size());
        if (fresh) {
            SelectionTile st;
            st.origin = tile.origin;
            st.lineweightMm = bk.lineweightUm / 1000.0;
            st.bounds = tile.geometryBounds();
            st.halfWidthMm = tile.maxHalfWidthMm;
            cache.geometry.tiles.push_back(std::move(st));
        }
        return cache.geometry.tiles[iit->second];
    };
    constexpr std::size_t kWalkEntitiesBelow = 64;
    if (set.size() <= kWalkEntitiesBelow) {
        // A few objects: go straight to their batches.
        for (EntityId id : set) {
            auto eit = entities_.find(id);
            if (eit == entities_.end())
                continue;
            for (std::uint32_t refId : eit->second.refs) {
                const auto& [tk, bk] = refPair(refId);
                if (bk.kind != BatchKind::Stroke || !layerVisible(bk.layer))
                    continue;
                auto tit = tiles_.find(tk);
                if (tit == tiles_.end())
                    continue;
                auto bit = tit->second.batches.find(bk);
                if (bit == tit->second.batches.end())
                    continue;
                SelectionTile& st = tileFor(tk, tit->second, bk);
                const Batch& batch = bit->second;
                for (const BatchRange& r : batch.ranges) {
                    if (r.entity != id)
                        continue;
                    st.verts.insert(st.verts.end(), batch.verts.begin() + r.begin,
                                    batch.verts.begin() + r.begin + r.count);
                }
            }
        }
        return cache.geometry;
    }
    // Many objects (select all): look each range up in the set once. Walking the ranges of a
    // batch for every selected object is quadratic in the number of objects per tile.
    for (const auto& [tk, tile] : tiles_) {
        for (const auto& [bk, batch] : tile.batches) {
            if (bk.kind != BatchKind::Stroke || !layerVisible(bk.layer))
                continue;
            SelectionTile* st = nullptr;
            for (const BatchRange& r : batch.ranges) {
                if (!set.count(r.entity))
                    continue;
                if (!st)
                    st = &tileFor(tk, tile, bk);
                st->verts.insert(st->verts.end(), batch.verts.begin() + r.begin,
                                 batch.verts.begin() + r.begin + r.count);
            }
        }
    }
    return cache.geometry;
}

bool Scene::needsRetessellation(const Camera2D& oldCam, const Camera2D& newCam) {
    if (!(oldCam.pixelsPerMm > 0) || !(newCam.pixelsPerMm > 0))
        return false;
    double ratio = newCam.pixelsPerMm / oldCam.pixelsPerMm;
    return ratio > 4.0 || ratio < 0.25;
}

namespace {
BBox2 cullBox(const Camera2D& camera, const Tile& tile) {
    // Strokes extend half a lineweight beyond their centre line, plus a few pixels for caps,
    // antialiasing and points.
    return camera.visibleWorld().inflated(tile.maxHalfWidthMm + 8.0 * camera.pixelSizeMm());
}
}  // namespace

std::vector<EntityId> Scene::staleEntities(const Camera2D& camera) const {
    std::vector<EntityId> out;
    for (const auto& [tk, tile] : tiles_) {
        if (!tile.geometryBounds().intersects(cullBox(camera, tile)))
            continue;
        for (const auto& [bk, batch] : tile.batches) {
            for (const BatchRange& r : batch.ranges) {
                if (r.builtPixelsPerMm <= 0)
                    continue;
                double ratio = camera.pixelsPerMm / r.builtPixelsPerMm;
                if (ratio > 4.0 || ratio < 0.25)
                    out.push_back(r.entity);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<VisibleBatch> Scene::visibleBatches(const Camera2D& camera) const {
    std::vector<VisibleBatch> out;
    for (const auto& [tk, tile] : tiles_) {
        if (!tile.geometryBounds().intersects(cullBox(camera, tile)))
            continue;
        for (const auto& [bk, batch] : tile.batches) {
            if (layerVisible(bk.layer) && !batch.verts.empty())
                out.push_back({&tile, &batch});
        }
    }
    std::sort(out.begin(), out.end(), [](const VisibleBatch& a, const VisibleBatch& b) {
        return std::make_tuple(a.batch->key.level, a.batch->key.kind, a.batch->minDrawOrder,
                               a.batch->id) < std::make_tuple(b.batch->key.level, b.batch->key.kind,
                                                              b.batch->minDrawOrder, b.batch->id);
    });
    return out;
}

std::vector<std::uint64_t> Scene::takeDeletedBatches() {
    return std::exchange(deleted_, {});
}

BBox2 Scene::bounds() const {
    BBox2 b;
    for (const auto& [tk, tile] : tiles_)
        b.merge(tile.geometryBounds());
    return b;
}

}  // namespace tbcad::render

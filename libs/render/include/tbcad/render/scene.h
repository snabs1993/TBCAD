#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tbcad/core/display.h"
#include "tbcad/core/ids.h"
#include "tbcad/core/math.h"
#include "tbcad/render/camera.h"
#include "tbcad/render/pattern.h"
#include "tbcad/render/precision.h"

namespace tbcad::render {

// The scene is the CPU-side half of the renderer: it turns display lists into tile-relative
// vertex batches (no OpenGL involved). The Renderer uploads the batches. Not thread-safe.

struct TileKey {
    std::int64_t x = 0;
    std::int64_t y = 0;
    friend bool operator==(const TileKey&, const TileKey&) = default;
    friend auto operator<=>(const TileKey&, const TileKey&) = default;
};

// Mask: a fill in the background colour (a wipeout). It hides what was drawn before it and is
// drawn between the batches of `level`s: see BatchKey::level.
enum class BatchKind : std::uint8_t { Fill = 0, Stroke = 1, Point = 2, Mask = 3 };

// Everything that makes two pieces of geometry drawable with the same GL state.
struct BatchKey {
    BatchKind kind = BatchKind::Stroke;
    LayerId layer{};
    std::uint32_t rgba = 0;         // r | g<<8 | b<<16 | a<<24
    std::int32_t lineweightUm = 0;  // lineweight in micrometres
    std::uint32_t pattern = 0;      // index into Scene::pattern(); 0 = continuous
    // Number of masks drawn before this geometry (masks with a lower draw order). Everything of
    // level n is drawn before the n-th mask and after the (n-1)-th, so a mask hides only what is
    // below it. Always 0 in a drawing without masks.
    std::uint32_t level = 0;
    friend bool operator==(const BatchKey&, const BatchKey&) = default;
    friend auto operator<=>(const BatchKey&, const BatchKey&) = default;
};

}  // namespace tbcad::render

namespace std {
template <>
struct hash<tbcad::render::TileKey> {
    size_t operator()(const tbcad::render::TileKey& k) const noexcept {
        return std::hash<std::int64_t>{}(k.x) * 0x9E3779B97F4A7C15ull ^
               std::hash<std::int64_t>{}(k.y);
    }
};
template <>
struct hash<tbcad::render::BatchKey> {
    size_t operator()(const tbcad::render::BatchKey& k) const noexcept {
        size_t h = static_cast<size_t>(k.kind);
        auto mix = [&h](size_t v) { h = (h ^ v) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull; };
        mix(k.layer.value);
        mix(k.rgba);
        mix(static_cast<size_t>(k.lineweightUm));
        mix(k.pattern);
        mix(k.level);
        return h;
    }
};
}  // namespace std

namespace tbcad::render {

// A contiguous run of vertices in a batch that belongs to one entity.
struct BatchRange {
    EntityId entity{};
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
    std::uint32_t drawOrder = 0;
    float builtPixelsPerMm = 0;  // zoom the geometry was tessellated for; 0 = unknown
};

// Strokes: two vertices per segment (a, b). Fills: three per triangle. Points: one each.
struct Batch {
    std::uint64_t id = 0;       // unique across all scenes of the process
    std::uint64_t version = 0;  // bumped on every change; GPU copies compare against it
    BatchKey key{};
    std::vector<GpuVertex> verts{};
    std::vector<BatchRange> ranges{};
    std::uint32_t minDrawOrder = 0;
};

struct Tile {
    TileKey key{};
    Vec2 origin{};  // world position all vertices of the tile are relative to
    std::unordered_map<BatchKey, Batch> batches{};
    double maxHalfWidthMm = 0;  // largest lineweight/2 of any stroke in the tile
    mutable BBox2 bounds{};     // bounds of the geometry (not of the cell)
    mutable bool boundsDirty = false;
    const BBox2& geometryBounds() const;
};

struct VisibleBatch {
    const Tile* tile = nullptr;
    const Batch* batch = nullptr;
};

// Geometry of highlighted or selected entities, copied from the batches (strokes only).
struct SelectionTile {
    Vec2 origin{};
    double lineweightMm = 0;
    BBox2 bounds{};  // of the scene tile this came from: a superset of the segments, for culling
    double halfWidthMm = 0;
    std::vector<GpuVertex> verts{};  // two per segment
};
struct SelectionGeometry {
    std::uint64_t version = 0;
    std::vector<SelectionTile> tiles{};
};

class Scene {
public:
    struct Options {
        // Edge of a tile in mm. Tiles only affect culling and dash precision, not coordinate
        // precision (vertices carry a hi/lo float pair), so anything from ~1 m to ~1 km works.
        double tileSizeMm = 1024;
    };

    Scene() : Scene(Options{}) {}
    explicit Scene(Options options);

    // A power of two giving roughly 8x8 tiles over `extent`, between 2^4 and 2^24 mm. Few big tiles
    // keep the number of draw calls low when the whole drawing is in view; the GPU does not mind
    // drawing the content of a handful of tiles when zoomed in.
    static double suggestTileSize(const BBox2& extent);

    // Adds or replaces an entity. Items are drawn per style bucket; `drawOrder` orders buckets
    // (lower first) within fills, strokes and points. `builtPixelsPerMm` is the zoom the curves
    // were tessellated for (0 = unknown), see staleEntities().
    void upsert(EntityId id, const DisplayList& list, std::uint32_t drawOrder,
                double builtPixelsPerMm = 0);
    // Adds or replaces an entity drawn as `blockSpace` moved by `t`, the same result as upsert
    // of the transformed list without copying it per reference: the geometry of a shared list is
    // prepared once (grouped by style, triangulated) and each reference only transforms and
    // packs it. False, with the entity untouched, for a list this cannot take (dashed lines,
    // wipeouts): the caller then builds the transformed list and calls upsert.
    bool upsertInstance(EntityId id, const std::shared_ptr<const DisplayList>& blockSpace,
                        const Transform2& t, std::uint32_t drawOrder, double builtPixelsPerMm = 0);
    // The same for a reference drawn at several places (an array) and with `extra` geometry that
    // is not shared (attribute values), already in drawing space.
    bool upsertInstance(EntityId id, const std::shared_ptr<const DisplayList>& blockSpace,
                        std::span<const Transform2> placements, const DisplayList* extra,
                        std::uint32_t drawOrder, double builtPixelsPerMm = 0);
    void remove(EntityId id);
    // Same for many objects at once; much faster than calling remove for each.
    // Drops everything when `ids` (distinct) are exactly the entities of the scene; false and
    // no change otherwise. Costs one lookup per id, no allocation.
    bool removeAllOf(std::span<const EntityId> ids);
    void removeMany(std::span<const EntityId> ids);
    void clear();
    bool contains(EntityId id) const { return entities_.count(id) != 0; }
    std::size_t entityCount() const { return entities_.size(); }
    // Gives back the spare room that growing batches keep (up to double the size). The content
    // and the batch versions stay as they are. Returns the bytes released.
    std::size_t trimMemory();

    // Layers are visible unless switched off. Use syncLayerVisibility() to follow a Document.
    void setLayerVisible(LayerId layer, bool visible);
    bool layerVisible(LayerId layer) const { return hiddenLayers_.count(layer) == 0; }

    void setHighlighted(std::span<const EntityId> ids);
    void setSelected(std::span<const EntityId> ids);
    const SelectionGeometry& highlightGeometry() const { return geometryFor(highlighted_, hl_); }
    const SelectionGeometry& selectedGeometry() const { return geometryFor(selected_, sel_); }

    // True when the zoom changed by more than 4x, so curves tessellated for `oldCam` look
    // faceted (zoom in) or wasteful (zoom out) at `newCam`.
    static bool needsRetessellation(const Camera2D& oldCam, const Camera2D& newCam);
    // Entities with geometry in the visible tiles that was built for a zoom more than 4x away
    // from the camera's. Entities upserted with builtPixelsPerMm == 0 never appear.
    std::vector<EntityId> staleEntities(const Camera2D& camera) const;

    // Wipeouts (masks) are drawn in draw order between other geometry. The scene builds batches
    // for the masks it knew when adoptMaskOrders() was last called; when a mask is added, removed
    // or reordered this is true, and the owner must call adoptMaskOrders() and upsert every
    // entity again (keeping its drawOrderOf()).
    bool needsRelevel() const;
    // True when the scene holds any wipeout (mask fill).
    bool hasMasks() const { return !masks_.empty(); }
    void adoptMaskOrders();
    std::uint32_t drawOrderOf(EntityId id) const;

    // Batches to draw: visible tiles only, hidden layers removed, ordered fills, strokes, points
    // and by drawOrder inside each kind.
    std::vector<VisibleBatch> visibleBatches(const Camera2D& camera) const;
    // Ids of batches destroyed since the last call (so the renderer can free their GPU buffers).
    std::vector<std::uint64_t> takeDeletedBatches();

    // Bounds of all geometry (empty box when the scene is empty).
    BBox2 bounds() const;
    const DashPattern& pattern(std::uint32_t id) const { return patterns_[id]; }
    const std::unordered_map<TileKey, Tile>& tiles() const { return tiles_; }
    TileKey tileOf(const Vec2& p) const;
    double tileSize() const { return tileSize_; }
    // Drops all geometry and re-tiles with the given tile edge (mm); <= 0 keeps the current one.
    void clearAndRetile(double tileSizeMm);

private:
    using RefPair = std::pair<TileKey, BatchKey>;
    // The batches an entity has geometry in, as numbers from the scene's table of (tile, batch)
    // pairs: a pair is 48 bytes and a drawing of 700,000 objects would otherwise hold that for
    // each. One or two fit in the record itself.
    class RefList {
    public:
        RefList() = default;
        RefList(const RefList& o) { assign(o.begin(), o.size()); }
        RefList(RefList&& o) noexcept { steal(o); }
        RefList& operator=(const RefList& o) {
            if (this != &o) {
                release();
                assign(o.begin(), o.size());
            }
            return *this;
        }
        RefList& operator=(RefList&& o) noexcept {
            if (this != &o) {
                release();
                steal(o);
            }
            return *this;
        }
        ~RefList() { release(); }
        std::uint32_t size() const { return n_; }
        bool empty() const { return n_ == 0; }
        std::uint32_t* begin() { return cap_ == kInline ? u_.in : u_.heap; }
        std::uint32_t* end() { return begin() + n_; }
        const std::uint32_t* begin() const { return cap_ == kInline ? u_.in : u_.heap; }
        const std::uint32_t* end() const { return begin() + n_; }
        void push_back(std::uint32_t v) {
            if (n_ == cap_) {
                const std::uint32_t cap = cap_ * 2;
                auto* p = new std::uint32_t[cap];
                std::copy(begin(), end(), p);
                if (cap_ != kInline)
                    delete[] u_.heap;
                u_.heap = p;
                cap_ = cap;
            }
            begin()[n_++] = v;
        }
        void truncate(std::uint32_t n) { n_ = n; }

    private:
        static constexpr std::uint32_t kInline = 2;
        void release() {
            if (cap_ != kInline)
                delete[] u_.heap;
            cap_ = kInline;
            n_ = 0;
        }
        void assign(const std::uint32_t* from, std::uint32_t n) {
            for (std::uint32_t i = 0; i < n; ++i)
                push_back(from[i]);
        }
        void steal(RefList& o) {
            n_ = o.n_;
            cap_ = o.cap_;
            if (o.cap_ == kInline)
                std::copy(o.u_.in, o.u_.in + kInline, u_.in);
            else
                u_.heap = o.u_.heap;
            o.cap_ = kInline;
            o.n_ = 0;
        }
        std::uint32_t n_ = 0, cap_ = kInline;
        union {
            std::uint32_t in[kInline];
            std::uint32_t* heap;
        } u_{};
    };
    struct RefPairHash {
        std::size_t operator()(const RefPair& r) const noexcept {
            return std::hash<TileKey>{}(r.first) * 31 ^ std::hash<BatchKey>{}(r.second);
        }
    };
    struct EntityRec {
        std::uint32_t drawOrder = 0;
        bool mask = false;
        std::uint32_t maskCount = 0;
        RefList refs{};
    };
    std::uint32_t internRef(const TileKey& tile, const BatchKey& key);
    const RefPair& refPair(std::uint32_t id) const { return refPairs_[id]; }
    // Puts an entity's refs in order of (tile, batch) and drops repeats.
    void finishRefs(EntityRec& rec);
    std::vector<RefPair> refPairs_{};
    std::unordered_map<RefPair, std::uint32_t, RefPairHash> refIds_{};
    struct SelCache {
        std::uint64_t builtFor = ~0ull;    // scene changeCounter_ the geometry was built for
        std::uint64_t builtStamp = ~0ull;  // this set's own stamp at that time
        std::uint64_t stamp = 0;           // bumped when the set's members change
        std::uint64_t serial = 0;          // unique version handed to the renderer
        SelectionGeometry geometry{};
    };

    struct Cursor;
    Batch& batchFor(const TileKey& tile, const BatchKey& key, double halfWidthMm);
    void addPolyline(Cursor& c, const DisplayPolyline& pl);
    void addFill(Cursor& c, const DisplayFill& fill);
    void addPoints(Cursor& c, const DisplayPoints& pts);
    BatchKey styleKey(BatchKind kind, const DisplayStyle& style, std::uint32_t level);
    std::vector<std::uint32_t> maskOrders() const;
    std::uint32_t levelBelow(std::uint32_t drawOrder) const;
    std::uint32_t internPattern(const DisplayStyle& style);
    void eraseRefs(EntityId id, const EntityRec& rec);
    const SelectionGeometry& geometryFor(const std::unordered_set<EntityId>& set,
                                         SelCache& cache) const;

    struct InstanceTemplate;
    struct InstanceTemplateDeleter {
        void operator()(InstanceTemplate*) const;
    };
    const InstanceTemplate* templateFor(const std::shared_ptr<const DisplayList>& list);
    // Prepared shared lists, kept alive with the list so the address stays unique.
    std::unordered_map<const DisplayList*,
                       std::unique_ptr<InstanceTemplate, InstanceTemplateDeleter>>
        templates_{};
    std::size_t templateSweepAt_ = 64;  // template count at which unused ones are dropped

    double tileSize_;
    // Frees a scene's worth of geometry on another thread (see discardAll).
    std::future<void> graveyard_{};
    std::future<void> selGraveyard_{};
    void discardAll();
    std::unordered_map<TileKey, Tile> tiles_{};
    std::unordered_map<EntityId, EntityRec> entities_{};
    std::unordered_set<LayerId> hiddenLayers_{};
    std::unordered_set<EntityId> highlighted_{};
    std::unordered_set<EntityId> selected_{};
    // Entities with mask fills -> draw order and the number of separate masks (runs of mask
    // fills with other geometry between them) the entity has.
    std::unordered_map<EntityId, std::pair<std::uint32_t, std::uint32_t>> masks_{};
    std::vector<std::uint32_t> levelOrders_{};  // sorted draw orders of the masks batches use
    std::vector<DashPattern> patterns_;
    std::map<std::vector<double>, std::uint32_t> patternIds_{};
    std::vector<std::uint64_t> deleted_{};
    std::uint64_t changeCounter_ = 0;  // bumped by anything the selection geometry depends on
    mutable SelCache hl_{};
    mutable SelCache sel_{};
};

}  // namespace tbcad::render

#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "tbcad/blocks/display.h"
#include "tbcad/core/core.h"
#include "tbcad/render/scene.h"

namespace tbcad::ui {

// Keeps a render::Scene equal to the model space of a Document. Every commit, undo and redo is
// mirrored: changed entities get new display lists (geom::buildDisplay), erased ones are removed,
// and layer visibility is synchronised through Document::effective(). Entities on a locked layer
// are drawn faded. No OpenGL is involved, so it runs headless.
//
// Registers itself as a DocumentObserver on construction; it must not outlive the document or the
// scene. Register it after the DocumentIndexer so that observers see a consistent index.
class SceneSync final : public DocumentObserver {
public:
    SceneSync(Document& doc, render::Scene& scene);
    ~SceneSync() override;
    SceneSync(const SceneSync&) = delete;
    SceneSync& operator=(const SceneSync&) = delete;

    void onChanged(const ChangeSet& changes) override;

    // Size of a screen pixel in mm that curves are tessellated for. Changing it does not rebuild
    // anything by itself; see refreshStale().
    void setPixelSize(double mm) { pixelSizeMm_ = mm; }
    double pixelSize() const { return pixelSizeMm_; }
    // Transparency on or off (the TPY switch); changing it needs a rebuildAll().
    void setShowTransparency(bool on) { showTransparency_ = on; }
    bool showTransparency() const { return showTransparency_; }

    // Threads used to build the display of many objects at once: 0 = one per core (at most 16),
    // 1 = none. The scene is the same for any number.
    void setMaxThreads(unsigned n) { maxThreads_ = n; }

    // Rebuilds the whole scene from the document (after loading a file).
    void rebuildAll(double tileSizeMm = 0);
    // Rebuilds up to `limit` entities in view whose curves were tessellated for a zoom more than
    // 4x away from the camera's. Returns how many were rebuilt.
    std::size_t refreshStale(const render::Camera2D& camera, std::size_t limit = 20000);
    // True when the last refreshStale() left stale objects alone because they are out of view:
    // the camera must be checked again after it moves.
    bool staleOffscreen() const { return skippedOffscreen_; }

    // A change of a very large number of objects (a MOVE of the whole drawing, its undo) takes
    // them out of the scene at once but builds their geometry later: first what the camera sees,
    // a slice per call, then the rest. pumpPending() does one slice; flushPending() builds all.
    // Nothing else changes: the scene lacks those objects until they are built.
    std::size_t pendingCount() const { return pending_.size() - pendingDead_; }
    // With budgetMs, stops early once that much time went by (checked after each chunk).
    std::size_t pumpPending(const render::Camera2D& camera, std::size_t limit = 8000,
                            double budgetMs = 0);
    void flushPending();

    // Bounds of the rendered geometry, for "zoom extents" on scene content.
    BBox2 bounds() const { return scene_.bounds(); }

    // Layers hidden on top of their own state: those frozen in the layout viewport being edited
    // from model space. Applied at once and kept through later layer changes.
    void setExtraHiddenLayers(std::unordered_set<LayerId> layers);
    const std::unordered_set<LayerId>& extraHiddenLayers() const { return extraHidden_; }

    // Block-space lists built since construction (for tests).
    std::size_t blockBuilds() const { return blockCache_.builds(); }

private:
    bool hiddenByLevel(const Entity& e) const;
    void upsertEntity(const Entity& e, std::optional<std::uint32_t> drawOrder = {});
    // The part of upsertEntity that only reads the document (safe on several threads once the
    // layers' effective settings are cached), and the part that changes the scene.
    struct Prepared {
        enum class What : std::uint8_t { Remove, Defer, List } what = What::Remove;
        const Entity* entity = nullptr;
        std::uint32_t order = 0;
        double builtZoom = 0;
        bool fields = false;
        DisplayList list{};
        // A reference to a block whose shared geometry was packed ahead (see packInserts).
        bool packed = false;
        blocks::InsertInstance inst{};
        render::Scene::InstancePack pack{};
    };
    void prepare(const Entity& e, std::uint32_t order, Prepared& out) const;
    // The references to blocks among `chunk`: their shared geometry is looked up on this thread
    // (the block cache is single threaded), then moved and packed on all cores.
    void packInserts(std::vector<Prepared>& chunk, std::size_t lanes);
    void commit(Prepared& p);
    // Builds the display of many entities on all cores and puts them in the scene in the given
    // order, so the scene is the same as when they are added one by one.
    // Lists shorter than minParallel are built on this thread.
    void upsertParallel(const std::vector<const Entity*>& list,
                        const std::vector<std::uint32_t>& orders, std::size_t minParallel = 16384);
    // Rebuilds many entities, each keeping its place in the draw order. Taking them out of the
    // scene one by one costs a pass over their batches each (quadratic for a whole drawing), so
    // they are taken out in chunks first.
    void upsertMany(const std::vector<const Entity*>& list);
    void relevelMasks();
    // Lazy building (see pendingCount): queue instead of build, and the pieces around it.
    struct PendingRec {
        EntityId id{};
        std::uint32_t order = 0;
        BBox2 box{};
        bool dead = false;  // built or erased meanwhile
    };
    bool lazyOk(std::size_t count) const;
    // `known` (optional, same length as `list`) holds boxes already measured for entities of
    // `list`, or null where there is none.
    void queueLazy(const std::vector<const Entity*>& list,
                   const std::vector<const BBox2*>* known = nullptr);
    std::optional<std::uint32_t> takePending(EntityId id);
    bool pendingHas(EntityId id) const;  // waiting to be built (counts as drawn)
    void dropPending(const std::vector<EntityId>& ids);
    std::uint32_t orderFor(EntityId id);
    void buildPending(PendingRec& rec);
    void compactPending();
    void syncLayers();
    void syncLayerVisibility();  // the document's layer states, then extraHidden_
    void rebuildDrawOrder();
    std::unordered_set<LayerId> extraHidden_{};

    Document& doc_;
    render::Scene& scene_;
    std::vector<PendingRec> pending_{};  // sorted by id
    std::size_t pendingDead_ = 0;
    double pixelSizeMm_ = 0.1;
    unsigned maxThreads_ = 0;
    bool skippedOffscreen_ = false;
    std::unordered_set<LayerId> lockedLayers_{};
    std::unordered_map<LayerId, std::uint8_t> layerTransparency_{};
    bool showTransparency_ = true;
    bool fillMode_ = true;    // FILL, from the drawing header
    int attDisplay_ = 0;      // ATTDISP, from the drawing header
    bool quickText_ = false;  // QTEXT, from the drawing header
    // Reads FILL and ATTDISP from the header; true when either changed.
    bool readDisplayModes();
    // Texts holding fields (%<...>%): they show other objects' values, so they are redrawn after
    // every change.
    std::unordered_set<EntityId> fieldTexts_{};
    // Block-space display lists shared by the Inserts of one block (dropped on every revision).
    blocks::BlockDisplayCache blockCache_{};
    std::uint64_t seenRevision_ = ~std::uint64_t{0};  // document revision at the last change seen
};

}  // namespace tbcad::ui

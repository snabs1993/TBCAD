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
    std::size_t pumpPending(const render::Camera2D& camera, std::size_t limit = 8000);
    void flushPending();

    // Bounds of the rendered geometry, for "zoom extents" on scene content.
    BBox2 bounds() const { return scene_.bounds(); }

private:
    bool hiddenByLevel(const Entity& e) const;
    void upsertEntity(const Entity& e, std::optional<std::uint32_t> drawOrder = {});
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
    void queueLazy(const std::vector<const Entity*>& list);
    std::optional<std::uint32_t> takePending(EntityId id);
    void dropPending(const std::vector<EntityId>& ids);
    std::uint32_t orderFor(EntityId id);
    void buildPending(PendingRec& rec);
    void compactPending();
    void syncLayers();
    void rebuildDrawOrder();

    Document& doc_;
    render::Scene& scene_;
    std::vector<PendingRec> pending_{};  // sorted by id
    std::size_t pendingDead_ = 0;
    double pixelSizeMm_ = 0.1;
    bool skippedOffscreen_ = false;
    std::unordered_set<LayerId> lockedLayers_{};
    std::unordered_map<LayerId, std::uint8_t> layerTransparency_{};
    bool showTransparency_ = true;
    bool fillMode_ = true;  // FILL, from the drawing header
    int attDisplay_ = 0;    // ATTDISP, from the drawing header
    // Reads FILL and ATTDISP from the header; true when either changed.
    bool readDisplayModes();
    // Texts holding fields (%<...>%): they show other objects' values, so they are redrawn after
    // every change.
    std::unordered_set<EntityId> fieldTexts_{};
    // Block-space display lists shared by the Inserts of one block (dropped on every revision).
    blocks::BlockDisplayCache blockCache_{};
};

}  // namespace tbcad::ui

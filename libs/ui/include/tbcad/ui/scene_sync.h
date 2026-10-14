#pragma once

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

    // Rebuilds the whole scene from the document (after loading a file).
    void rebuildAll(double tileSizeMm = 0);
    // Rebuilds up to `limit` entities in view whose curves were tessellated for a zoom more than
    // 4x away from the camera's. Returns how many were rebuilt.
    std::size_t refreshStale(const render::Camera2D& camera, std::size_t limit = 20000);

    // Bounds of the rendered geometry, for "zoom extents" on scene content.
    BBox2 bounds() const { return scene_.bounds(); }

private:
    bool hiddenByLevel(const Entity& e) const;
    void upsertEntity(const Entity& e);
    void syncLayers();
    void rebuildDrawOrder();

    Document& doc_;
    render::Scene& scene_;
    double pixelSizeMm_ = 0.1;
    std::unordered_set<LayerId> lockedLayers_{};
    // Texts holding fields (%<...>%): they show other objects' values, so they are redrawn after
    // every change.
    std::unordered_set<EntityId> fieldTexts_{};
    // Block-space display lists shared by the Inserts of one block (dropped on every revision).
    blocks::BlockDisplayCache blockCache_{};
};

}  // namespace tbcad::ui

#pragma once

#include <functional>

#include "tbcad/core/document.h"
#include "tbcad/index/spatial_index.h"

namespace tbcad::index {

// Keeps a SpatialIndex in step with the model-space entities of a Document: every commit, undo
// and redo is mirrored into the index. Entities inside block definitions are not indexed.
//
// The bounding box comes from a callback so that this library does not depend on the geometry
// module. An empty box means "not pickable/drawable" and the entity is left out of the index.
// The callback sees only the entity: when a block definition changes, the boxes of its Inserts
// change too and the owner of the callback must call reindex() / rebuild().
//
// The indexer registers itself as an observer on construction and unregisters on destruction;
// it must not outlive the document or the index.
class DocumentIndexer final : public DocumentObserver {
public:
    using BoundsFn = std::function<BBox2(const Entity&)>;

    // Registers with `doc` and loads every model-space entity into `index` (replacing its content).
    DocumentIndexer(Document& doc, SpatialIndex& index, BoundsFn bounds);
    ~DocumentIndexer() override;
    DocumentIndexer(const DocumentIndexer&) = delete;
    DocumentIndexer& operator=(const DocumentIndexer&) = delete;

    void onChanged(const ChangeSet& changes) override;

    // Reloads the whole index from the document.
    void rebuild();
    // Recomputes one entity's box.
    void reindex(EntityId id);

private:
    Document& doc_;
    SpatialIndex& index_;
    BoundsFn bounds_;
};

}  // namespace tbcad::index

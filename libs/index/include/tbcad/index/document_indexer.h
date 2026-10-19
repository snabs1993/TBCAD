#pragma once

#include <functional>
#include <memory>

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
    // `batch`, when set, is called before a run of many bounds calls (loading, a big change) and
    // the object it returns is kept until the run ends: a hook for caches that help a run.
    using BatchFn = std::function<std::shared_ptr<void>()>;
    DocumentIndexer(Document& doc, SpatialIndex& index, BoundsFn bounds, BatchFn batch = {},
                    BatchFn workerScope = {}, std::size_t maxThreads = 0);
    ~DocumentIndexer() override;
    DocumentIndexer(const DocumentIndexer&) = delete;
    DocumentIndexer& operator=(const DocumentIndexer&) = delete;

    // With `workerScope` set a big load measures on several threads. `workerScope` is called once
    // on each thread before it measures and the object it returns is kept until the thread is done
    // (per-thread caches, like the one `batch` gives the loading thread). The bounds callback must
    // then be safe to call from several threads at once.

    void onChanged(const ChangeSet& changes) override;

    // Reloads the whole index from the document.
    void rebuild();
    // Recomputes one entity's box.
    void reindex(EntityId id);

private:
    void rebuildKeeping(const ChangeSet* changes, bool keepOld = false);
    // The index as it was on the other side of a big change. When that same
    // change comes back the other way (undo of an erase, redo of it ...), the two trees are swapped
    // instead of built again. Any other change to the index drops it.
    struct Other {
        SpatialIndex tree;
        std::shared_ptr<const void> key;  // the change's shared box memo identifies it
        std::size_t count = 0;            // entities in the change
        std::size_t indexSize = 0;        // what the live index must hold for the swap to be valid
        bool valid = false;
    } other_;

    Document& doc_;
    SpatialIndex& index_;
    BoundsFn bounds_;
    BatchFn batch_;
    BatchFn workerScope_;
    std::size_t maxThreads_ = 0;  // 0 = the machine's
};

}  // namespace tbcad::index

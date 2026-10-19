#include "tbcad/index/document_indexer.h"

#include <unordered_set>
#include <utility>
#include <vector>

namespace tbcad::index {

namespace {

bool usable(const BBox2& b) {
    return !b.empty();  // non-finite boxes are rejected by the index itself
}

}  // namespace

DocumentIndexer::DocumentIndexer(Document& doc, SpatialIndex& index, BoundsFn bounds, BatchFn batch)
    : doc_(doc), index_(index), bounds_(std::move(bounds)), batch_(std::move(batch)) {
    rebuild();
    doc_.addObserver(this);
}

DocumentIndexer::~DocumentIndexer() {
    doc_.removeObserver(this);
}

void DocumentIndexer::rebuild() {
    rebuildKeeping(nullptr);
}

// Loads every entity again. With `changes`, entities it does not name keep the box they have in
// the index: laying out every text again just to rebuild after a big edit is most of the cost.
void DocumentIndexer::rebuildKeeping(const ChangeSet* changes) {
    std::shared_ptr<void> run = batch_ ? batch_() : nullptr;
    std::unordered_set<EntityId> touched;
    if (changes)
        for (const EntityChange& c : changes->entities)
            touched.insert(changes->idOf(c));
    std::vector<IdBox> items;
    items.reserve(doc_.modelSpaceCount());
    doc_.forEachEntity([&](const Entity& e) {
        if (doc_.ownerBlock(e.common.id))
            return;
        BBox2 b;
        if (changes && !touched.count(e.common.id) && index_.contains(e.common.id))
            b = index_.boxOf(e.common.id);
        else
            b = bounds_(e);
        if (usable(b))
            items.emplace_back(e.common.id, b);
    });
    index_.bulkLoad(items);
}

void DocumentIndexer::reindex(EntityId id) {
    const Entity* e = doc_.find(id);
    if (!e || doc_.ownerBlock(id)) {
        index_.remove(id);
        return;
    }
    BBox2 b = bounds_(*e);
    if (usable(b))
        index_.update(id, b);
    else
        index_.remove(id);
}

void DocumentIndexer::onChanged(const ChangeSet& changes) {
    std::size_t n = changes.entities.size();
    // A large edit (an import, a paste of thousands) is cheaper as one bulk load.
    if (n > 1000 && n * 2 > index_.size()) {
        rebuildKeeping(&changes);
        return;
    }
    // Taking out many entries one by one condenses the tree again and again (several times the
    // cost of putting them in), so a quarter of the index going away is also a rebuild.
    if (n > 1000 && n * 4 > index_.size()) {
        std::size_t gone = 0;
        for (const EntityChange& c : changes.entities)
            if (!changes.after(c))
                ++gone;
        if (gone * 4 > index_.size()) {
            rebuildKeeping(&changes);
            return;
        }
    }
    std::shared_ptr<void> run = (batch_ && n > 8) ? batch_() : nullptr;
    for (const EntityChange& c : changes.entities)
        reindex(changes.idOf(c));
}

}  // namespace tbcad::index

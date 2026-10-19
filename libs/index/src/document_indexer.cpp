#include "tbcad/index/document_indexer.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
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
    // Entities the change names: the box it remembers for them, or none (measure them again).
    struct Touched {
        std::optional<BBox2> known{};
        std::uint32_t addedAs = kNoSnapshot;  // snapshot to remember a measured box with
    };
    // Ids are small and dense, so a table indexed by id beats hashing 400,000 of them.
    constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    std::vector<std::uint32_t> slot;
    std::vector<Touched> touched;
    auto touchedOf = [&](EntityId id) -> Touched* {
        return id.value < slot.size() && slot[id.value] != kNone ? &touched[slot[id.value]]
                                                                 : nullptr;
    };
    if (changes) {
        std::uint64_t top = 0;
        for (const EntityChange& c : changes->entities)
            top = std::max(top, changes->idOf(c).value);
        slot.assign(top + 1, kNone);
        touched.reserve(changes->entities.size());
        for (const EntityChange& c : changes->entities) {
            const std::uint64_t v = changes->idOf(c).value;
            if (slot[v] == kNone) {
                slot[v] = static_cast<std::uint32_t>(touched.size());
                touched.emplace_back();
            }
            Touched& t = touched[slot[v]];
            if (c.before == kNoSnapshot && c.after != kNoSnapshot) {
                t.addedAs = c.after;
                if (const BBox2* known = changes->memoBox(c.after))
                    t.known = *known;
            }
        }
    }
    std::vector<IdBox> items;
    items.reserve(doc_.modelSpaceCount());
    doc_.forEachEntity([&](const Entity& e) {
        if (doc_.ownerBlock(e.common.id))
            return;
        BBox2 b;
        Touched* t = touchedOf(e.common.id);
        if (changes && !t && index_.contains(e.common.id))
            b = index_.boxOf(e.common.id);
        else if (t && t->known)
            b = *t->known;
        else {
            b = bounds_(e);
            if (t)
                changes->memoizeBox(t->addedAs, b);
        }
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
    // Entities that go away still have their box in the index: keep it with the snapshot, so an
    // undo that brings them back does not have to measure them again.
    for (const EntityChange& c : changes.entities)
        if (c.before != kNoSnapshot && c.after == kNoSnapshot) {
            const EntityId id = changes.before(c)->common.id;
            if (index_.contains(id))
                changes.memoizeBox(c.before, index_.boxOf(id));
        }
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

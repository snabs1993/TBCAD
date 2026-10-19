#include "tbcad/index/document_indexer.h"

#include <algorithm>
#include <future>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace tbcad::index {

namespace {

bool usable(const BBox2& b) {
    return !b.empty();  // non-finite boxes are rejected by the index itself
}

}  // namespace

DocumentIndexer::DocumentIndexer(Document& doc, SpatialIndex& index, BoundsFn bounds, BatchFn batch,
                                 BatchFn workerScope, std::size_t maxThreads)
    : doc_(doc),
      index_(index),
      bounds_(std::move(bounds)),
      batch_(std::move(batch)),
      workerScope_(std::move(workerScope)),
      maxThreads_(maxThreads) {
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
    // What the change names, sorted by id: binary search beats hashing a million ids. An entity
    // that comes back may have its box remembered with the change (see onChanged); one that has
    // to be measured is remembered after the measuring.
    struct Touched {
        EntityId id{};
        std::optional<BBox2> known{};
        std::uint32_t addedAs = kNoSnapshot;
    };
    std::vector<Touched> touched;
    std::size_t unmeasured = 0;  // entities that come back without a remembered box
    auto byId = [](const Touched& a, const Touched& b) { return a.id < b.id; };
    auto touchedOf = [&](EntityId id) -> const Touched* {
        auto it = std::lower_bound(touched.begin(), touched.end(), Touched{id}, byId);
        return it != touched.end() && it->id == id ? &*it : nullptr;
    };
    if (changes) {
        touched.reserve(changes->entities.size());
        for (const EntityChange& c : changes->entities) {
            Touched t;
            t.id = changes->idOf(c);
            if (c.before == kNoSnapshot && c.after != kNoSnapshot) {
                t.addedAs = c.after;
                if (const BBox2* known = changes->memoBox(c.after))
                    t.known = *known;
                else
                    ++unmeasured;
            }
            touched.push_back(t);
        }
        if (!std::is_sorted(touched.begin(), touched.end(), byId))
            std::sort(touched.begin(), touched.end(), byId);
    }
    std::vector<const Entity*> todo;
    todo.reserve(doc_.modelSpaceCount());
    doc_.forEachEntity([&](const Entity& e) {
        if (!doc_.ownerBlock(e.common.id))
            todo.push_back(&e);
    });
    std::vector<BBox2> boxes(todo.size());
    auto measure = [&](std::size_t a, std::size_t b) {
        // Entities usually come in id order, so a cursor along the sorted list beats a search each.
        std::size_t cur = 0;
        EntityId last{};
        for (std::size_t i = a; i < b; ++i) {
            const Entity& e = *todo[i];
            const Touched* t = nullptr;
            if (changes) {
                const EntityId id = e.common.id;
                if (i == a || id < last)
                    cur = std::lower_bound(touched.begin(), touched.end(), Touched{id}, byId) -
                          touched.begin();
                else
                    while (cur < touched.size() && touched[cur].id < id)
                        ++cur;
                last = id;
                if (cur < touched.size() && touched[cur].id == id)
                    t = &touched[cur];
            }
            if (changes && !t && index_.contains(e.common.id))
                boxes[i] = index_.boxOf(e.common.id);
            else if (t && t->known)
                boxes[i] = *t->known;
            else
                boxes[i] = bounds_(e);
        }
    };
    constexpr std::size_t kMinPerLane = 8192;
    const std::size_t hw = maxThreads_ ? maxThreads_ : std::thread::hardware_concurrency();
    const std::size_t lanes =
        std::clamp<std::size_t>(std::min(hw, todo.size() / kMinPerLane), 1, 16);
    if (lanes < 2 || !workerScope_) {
        measure(0, todo.size());
    } else {
        // The lookups that fill a cache on first use are done before the threads start.
        for (const auto& [id, layer] : doc_.layers())
            (void)doc_.effective(id);
        auto lane = [&](std::size_t a, std::size_t b) {
            std::shared_ptr<void> scope = workerScope_();
            measure(a, b);
        };
        std::vector<std::future<void>> jobs;
        for (std::size_t k = 1; k < lanes; ++k)
            jobs.push_back(std::async(std::launch::async, lane, todo.size() * k / lanes,
                                      todo.size() * (k + 1) / lanes));
        lane(0, todo.size() / lanes);
        for (auto& j : jobs)
            j.get();
    }
    if (unmeasured)  // (here, not in the lanes: the memo is not safe to fill from several threads)
        for (std::size_t i = 0; i < todo.size(); ++i)
            if (const Touched* t = touchedOf(todo[i]->common.id); t && !t->known)
                changes->memoizeBox(t->addedAs, boxes[i]);
    std::vector<IdBox> items;
    items.reserve(todo.size());
    for (std::size_t i = 0; i < todo.size(); ++i)
        if (usable(boxes[i]))
            items.emplace_back(todo[i]->common.id, boxes[i]);
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

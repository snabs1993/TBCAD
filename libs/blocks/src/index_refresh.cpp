#include "tbcad/blocks/index_refresh.h"

#include <unordered_set>

#include "tbcad/blocks/blocks.h"

namespace tbcad::blocks {

BlockIndexRefresher::BlockIndexRefresher(Document& doc, index::DocumentIndexer& indexer)
    : doc_(doc), indexer_(indexer) {
    doc_.addObserver(this);
}

BlockIndexRefresher::~BlockIndexRefresher() {
    doc_.removeObserver(this);
}

void BlockIndexRefresher::onChanged(const ChangeSet& cs) {
    std::vector<BlockId> touched;
    for (const auto& row : cs.blocks) {
        const BlockDef* d = row.after ? &*row.after : (row.before ? &*row.before : nullptr);
        if (d)
            touched.push_back(d->id);
    }
    for (const EntityChange& c : cs.entities) {
        // An entity that is (or was) inside a block definition changes what that block draws.
        const Entity* e = cs.after(c) ? cs.after(c) : cs.before(c);
        if (!e)
            continue;
        if (BlockId owner = doc_.ownerBlock(e->common.id))
            touched.push_back(owner);
    }
    if (touched.empty())
        return;
    const auto dirty = dependentBlocks(doc_, touched);
    std::unordered_set<BlockId> set(dirty.begin(), dirty.end());
    std::vector<EntityId> inserts;
    doc_.forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e))
            if (set.count(ins->block) && !doc_.ownerBlock(e.common.id))
                inserts.push_back(e.common.id);
    });
    for (EntityId id : inserts)
        indexer_.reindex(id);
}

}  // namespace tbcad::blocks

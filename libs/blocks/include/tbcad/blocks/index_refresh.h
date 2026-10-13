#pragma once

#include "tbcad/core/core.h"
#include "tbcad/index/document_indexer.h"

namespace tbcad::blocks {

// Keeps the spatial index right when a block definition changes. The box of an Insert depends on
// its block's contents, which DocumentIndexer cannot see, so this observer reindexes every
// model-space Insert whose block (or a block nested in it) was touched by a commit, undo or redo.
//
// Create it after the DocumentIndexer it feeds (the indexer must handle a change first) and
// destroy it before the indexer and the document.
class BlockIndexRefresher final : public DocumentObserver {
public:
    BlockIndexRefresher(Document& doc, index::DocumentIndexer& indexer);
    ~BlockIndexRefresher() override;
    BlockIndexRefresher(const BlockIndexRefresher&) = delete;
    BlockIndexRefresher& operator=(const BlockIndexRefresher&) = delete;

    void onChanged(const ChangeSet& changes) override;

private:
    Document& doc_;
    index::DocumentIndexer& indexer_;
};

}  // namespace tbcad::blocks

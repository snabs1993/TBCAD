#include "tbcad/collab/ids.h"

#include <algorithm>

namespace tbcad::collab {

IdRange IdRanges::grant(PeerId peer) {
    NextIds next = doc_->nextIds();
    IdRange r;
    r.first = next.entity;
    r.limit = r.first + blockSize_;
    // The host's own ids now start above the block.
    NextIds raised;
    raised.entity = r.limit;
    (void)doc_->reserveIds(raised);
    granted_[peer].push_back(r);
    return r;
}

bool IdRanges::owns(PeerId peer, EntityId id) const {
    auto it = granted_.find(peer);
    if (it == granted_.end())
        return false;
    return std::any_of(it->second.begin(), it->second.end(),
                       [&](const IdRange& r) { return r.contains(id.value); });
}

const std::vector<IdRange>& IdRanges::rangesOf(PeerId peer) const {
    static const std::vector<IdRange> none;
    auto it = granted_.find(peer);
    return it == granted_.end() ? none : it->second;
}

}  // namespace tbcad::collab

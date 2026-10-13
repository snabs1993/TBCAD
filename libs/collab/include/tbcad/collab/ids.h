#pragma once

// Entity id ranges. Ids are plain integers handed out by the document; two peers that draw at the
// same time would mint the same id. The host therefore gives every guest its own block of ids
// to draw from and keeps the rest for itself. Ids stay unique and `.tbcad` needs no new field:
// the saved "next id" of the host is above every block it handed out.
//
// Table rows (layers, styles, blocks ...) are created rarely and are not ranged. Two peers that
// create a row with the same id at the same time are told apart by the existence check of the
// operation: the second is rejected like any other conflict.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "tbcad/core/document.h"

namespace tbcad::collab {

using PeerId = std::uint32_t;
inline constexpr PeerId kHostPeer = 0;

struct IdRange {
    std::uint64_t first = 0;
    std::uint64_t limit = 0;  // one past the last id
    std::uint64_t size() const { return limit > first ? limit - first : 0; }
    bool contains(std::uint64_t id) const { return id >= first && id < limit; }
    friend constexpr bool operator==(const IdRange&, const IdRange&) = default;
};

class IdRanges {
public:
    // Hands out blocks of `blockSize` ids from `doc`'s counter.
    explicit IdRanges(Document& doc, std::uint64_t blockSize = std::uint64_t{1} << 16)
        : doc_(&doc), blockSize_(blockSize ? blockSize : 1) {}

    // A fresh block for the peer (it may hold several: ask again when one runs low).
    IdRange grant(PeerId peer);
    bool owns(PeerId peer, EntityId id) const;
    // The peer left. Its unused ids are not given to anyone else (ids are never reused).
    void release(PeerId peer) { granted_.erase(peer); }
    const std::vector<IdRange>& rangesOf(PeerId peer) const;

private:
    Document* doc_;
    std::uint64_t blockSize_;
    std::unordered_map<PeerId, std::vector<IdRange>> granted_{};
};

}  // namespace tbcad::collab

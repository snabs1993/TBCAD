#pragma once

// The wire form of an edit.
//
// A guest sends an Op: its edit (an io::Delta, the "after" half of a ChangeSet) plus, for every
// thing the edit touches, the version the guest saw and whether the thing existed. The host
// answers every accepted Op by broadcasting an Accepted, which carries the same delta and the
// sequence number that becomes the new version of everything it touched.

#include <cstdint>
#include <span>
#include <vector>

#include "tbcad/collab/ids.h"
#include "tbcad/collab/versions.h"
#include "tbcad/core/document.h"
#include "tbcad/io/delta.h"

namespace tbcad::collab {

struct Base {
    Key key{};
    std::uint64_t version = 0;
    bool existed = false;  // the author's copy had the thing before the edit
    friend constexpr bool operator==(const Base&, const Base&) = default;
};

struct Op {
    PeerId author = 0;
    std::uint64_t clientSeq = 0;  // the author's own counter, echoed back in Accepted
    std::vector<Base> bases{};    // one per key of `delta`, sorted by key
    io::Delta delta{};
};

struct Accepted {
    std::uint64_t seq = 0;  // global order; the new version of everything the delta touches
    PeerId author = 0;
    std::uint64_t clientSeq = 0;
    io::Delta delta{};
};

// Builds the Op for a change set the author just committed. `versions` are the versions the
// author's replica holds (see Replica).
Op makeOp(PeerId author, std::uint64_t clientSeq, const ChangeSet& committed,
          const VersionMap& versions);

std::vector<std::uint8_t> encodeOp(const Op& op);
Result<Op> decodeOp(std::span<const std::uint8_t> bytes, const io::DeltaLimits& limits = {});
std::vector<std::uint8_t> encodeAccepted(const Accepted& accepted);
Result<Accepted> decodeAccepted(std::span<const std::uint8_t> bytes,
                                const io::DeltaLimits& limits = {});

// Patches the pending ops of a guest after one of its own earlier ops was accepted, so that they
// are not judged against versions the earlier op has already replaced.
void rebasePending(std::vector<Op>& pending, const Accepted& own);

}  // namespace tbcad::collab

#pragma once

// What a guest keeps: its copy of the document, the versions it has been told about, and the
// edits it made that the host has not answered yet.
//
// The guest edits optimistically: a local commit is in the document at once and an Op goes to
// the host. The document is always "what the host has confirmed, plus the pending edits on top".
// When the host broadcasts someone else's edit, the Replica takes its own pending edits off,
// applies the broadcast and puts the pending edits back (they are after-images, so putting them
// back is a replay). When the host answers one of its edits with "rejected", that edit is taken
// out and the ones after it are replayed. Nothing here sends or receives bytes: the session
// engine (Q62) does that, and shows the user what happened.

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>

#include "tbcad/collab/ids.h"
#include "tbcad/collab/ops.h"
#include "tbcad/collab/versions.h"
#include "tbcad/core/document.h"

namespace tbcad::collab {

class Replica : private DocumentObserver {
public:
    // Watches `doc`: every commit, undo or redo the user makes becomes an Op passed to onLocalOp.
    Replica(Document& doc, PeerId self);
    ~Replica() override;
    Replica(const Replica&) = delete;
    Replica& operator=(const Replica&) = delete;

    Document& document() { return *doc_; }
    PeerId self() const { return self_; }
    const VersionMap& versions() const { return versions_; }
    std::uint64_t sequence() const { return seq_; }
    // Starts from a snapshot of the host (the document must already be that snapshot).
    void restore(VersionMap versions, std::uint64_t sequence);

    // The host gave this peer a block of ids. New entities take their ids from the blocks, in
    // the order they were granted.
    Result<void> addRange(IdRange range);
    // Ids still free to draw with, over all blocks. Ask the host for more when this runs low.
    std::uint64_t idsLeft() const;

    // Called with the Op for a local edit when it is its turn: one edit at a time is in flight,
    // the next is released when the host has answered the previous. Send it to the host.
    std::function<void(const Op&)> onLocalOp{};

    // The host's answer to a local edit that was accepted, or a broadcast of anybody else's edit.
    // Returns the change applied to the document (empty for the replica's own accepted edits).
    Result<ChangeSet> apply(const Accepted& accepted);
    // The host refused the local edit numbered `clientSeq`. Its effect leaves the document.
    // Returns the change that took it out (empty when it was not in the document any more).
    Result<ChangeSet> reject(std::uint64_t clientSeq);

    std::size_t pendingCount() const { return pending_.size(); }
    // True while the Replica itself changes the document (replays); observers that mirror the
    // document for another purpose can use it to tell these changes from the user's.
    bool busy() const { return busy_; }

private:
    struct Pending {
        Op op{};
        ChangeSet applied{};  // what the document holds for it right now (before/after images)
        bool live = true;     // false: could not be replayed; the host will refuse it
    };

    void onChanged(const ChangeSet& changes) override;
    void useNextRange();
    void takeOffPending(std::size_t from);
    void putBackPending(std::size_t from);
    void sendNext();

    Document* doc_;
    PeerId self_;
    VersionMap versions_{};
    std::uint64_t seq_ = 0;
    std::uint64_t clientSeq_ = 0;
    std::deque<IdRange> ranges_{};
    std::deque<Pending> pending_{};
    bool busy_ = false;
    bool inFlight_ = false;  // the oldest pending edit has been handed to onLocalOp
};

}  // namespace tbcad::collab

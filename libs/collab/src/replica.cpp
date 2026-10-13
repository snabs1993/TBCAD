#include "tbcad/collab/replica.h"

#include <algorithm>

namespace tbcad::collab {

namespace {
// Marks the replica as changing the document; can be ended early.
struct BusyGuard {
    bool* flag;
    explicit BusyGuard(bool& f) : flag(&f) { *flag = true; }
    void release() {
        if (flag)
            *flag = false;
        flag = nullptr;
    }
    ~BusyGuard() { release(); }
};
}  // namespace

Replica::Replica(Document& doc, PeerId self) : doc_(&doc), self_(self) {
    doc_->addObserver(this);
}

Replica::~Replica() {
    doc_->removeObserver(this);
}

void Replica::restore(VersionMap versions, std::uint64_t sequence) {
    versions_ = std::move(versions);
    seq_ = sequence;
}

Result<void> Replica::addRange(IdRange range) {
    if (range.size() == 0)
        return makeError(ErrorCode::InvalidArgument, "empty id range");
    ranges_.push_back(range);
    constexpr auto kNoWindow = std::numeric_limits<std::uint64_t>::max();
    if (doc_->entityIdsLeft() == kNoWindow || doc_->entityIdsLeft() == 0)
        useNextRange();
    return {};
}

void Replica::useNextRange() {
    if (ranges_.empty())
        return;
    IdRange r = ranges_.front();
    ranges_.pop_front();
    (void)doc_->setEntityIdWindow(r.first, r.limit);
}

std::uint64_t Replica::idsLeft() const {
    std::uint64_t n = doc_->entityIdsLeft();
    if (n == std::numeric_limits<std::uint64_t>::max())
        n = 0;
    for (const IdRange& r : ranges_)
        n += r.size();
    return n;
}

void Replica::onChanged(const ChangeSet& changes) {
    if (busy_ || changes.empty())
        return;
    Pending p;
    p.op = collab::makeOp(self_, ++clientSeq_, changes, versions_);
    p.applied = changes;
    // A window that ran dry moves on to the next block before the next edit.
    if (doc_->entityIdsLeft() == 0)
        useNextRange();
    pending_.push_back(std::move(p));
    sendNext();
}

// One edit at a time is in flight: the next one is built on top of the previous one, so its
// versions can only be judged once the host has answered the previous one.
void Replica::sendNext() {
    if (inFlight_ || pending_.empty())
        return;
    inFlight_ = true;
    if (onLocalOp)
        onLocalOp(pending_.front().op);
}

// Reverses the pending edits from the newest back to `from`.
void Replica::takeOffPending(std::size_t from) {
    for (std::size_t i = pending_.size(); i-- > from;) {
        if (pending_[i].live)
            doc_->apply(pending_[i].applied, Direction::Undo);
    }
}

// Applies the pending edits from `from` again, on top of whatever the document is now. An edit
// that no longer fits is kept as dead: the host will refuse it as well.
void Replica::putBackPending(std::size_t from) {
    for (std::size_t i = from; i < pending_.size(); ++i) {
        Pending& p = pending_[i];
        p.live = false;
        Result<ChangeSet> cs = io::materialize(p.op.delta, *doc_);
        if (!cs)
            continue;
        if (!doc_->applyChecked(cs.value()))
            continue;
        p.applied = std::move(cs.value());
        p.live = true;
    }
}

Result<ChangeSet> Replica::apply(const Accepted& a) {
    if (a.seq <= seq_)
        return makeError(ErrorCode::Conflict, "broadcast is out of order");
    BusyGuard busyOff(busy_);

    ChangeSet applied;
    const bool own = a.author == self_;
    if (own) {
        if (pending_.empty() || pending_.front().op.clientSeq != a.clientSeq)
            return makeError(ErrorCode::Conflict, "answer for an edit that is not pending");
        Pending done = std::move(pending_.front());
        pending_.pop_front();
        inFlight_ = false;
        if (!done.live) {
            // The host took an edit the replica could not replay: apply it the ordinary way.
            takeOffPending(0);
            Result<ChangeSet> cs = io::materialize(a.delta, *doc_);
            if (!cs)
                return cs.error();
            if (Result<void> r = doc_->applyChecked(cs.value()); !r)
                return r.error();
            applied = std::move(cs.value());
            putBackPending(0);
        }
        std::vector<Op> rest;
        for (Pending& p : pending_)
            rest.push_back(p.op);
        rebasePending(rest, a);
        for (std::size_t i = 0; i < pending_.size(); ++i)
            pending_[i].op = std::move(rest[i]);
    } else {
        takeOffPending(0);
        Result<ChangeSet> cs = io::materialize(a.delta, *doc_);
        if (!cs) {
            putBackPending(0);
            return cs.error();
        }
        if (Result<void> r = doc_->applyChecked(cs.value()); !r) {
            putBackPending(0);
            return r.error();
        }
        applied = std::move(cs.value());
        putBackPending(0);
    }
    seq_ = a.seq;
    versions_.setAll(keysOf(a.delta), a.seq);
    busyOff.release();
    if (own)
        sendNext();
    return applied;
}

Result<ChangeSet> Replica::reject(std::uint64_t clientSeq) {
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [&](const Pending& p) { return p.op.clientSeq == clientSeq; });
    if (it == pending_.end())
        return makeError(ErrorCode::NotFound, "no such pending edit");
    BusyGuard busyOff(busy_);

    const std::size_t at = static_cast<std::size_t>(it - pending_.begin());
    takeOffPending(at);
    ChangeSet undone = it->live ? it->applied.inverted() : ChangeSet{};
    pending_.erase(it);
    if (at == 0)
        inFlight_ = false;
    putBackPending(at);
    busyOff.release();
    sendNext();
    return undone;
}

}  // namespace tbcad::collab

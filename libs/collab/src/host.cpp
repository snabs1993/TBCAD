#include "tbcad/collab/host.h"

#include <algorithm>

namespace tbcad::collab {

Host::Host(Document& doc, HostConfig config)
    : doc_(&doc), cfg_(std::move(config)), ranges_(doc, cfg_.idBlockSize) {
    doc_->addObserver(this);
}

Host::~Host() {
    doc_->removeObserver(this);
}

void Host::restore(VersionMap versions, std::uint64_t sequence) {
    versions_ = std::move(versions);
    seq_ = sequence;
}

PeerId Host::addPeer(Role role) {
    PeerId id = nextPeer_++;
    peers_[id].role = role;
    return id;
}

void Host::removePeer(PeerId peer) {
    peers_.erase(peer);
    ranges_.release(peer);
}

bool Host::setRole(PeerId peer, Role role) {
    auto it = peers_.find(peer);
    if (it == peers_.end())
        return false;
    it->second.role = role;
    return true;
}

std::optional<Role> Host::roleOf(PeerId peer) const {
    auto it = peers_.find(peer);
    if (it == peers_.end())
        return std::nullopt;
    return it->second.role;
}

IdRange Host::grantRange(PeerId peer) {
    return ranges_.grant(peer);
}

Submitted Host::reject(RejectReason reason, std::string message, std::optional<Key> key,
                       std::uint64_t current) {
    Submitted s;
    s.rejection = Rejection{reason, std::move(message), key, current};
    return s;
}

void Host::onChanged(const ChangeSet& changes) {
    if (applying_)
        return;  // a guest's edit that submit() is applying; it is recorded there
    Accepted a;
    a.delta = io::deltaOf(changes);
    a.author = kHostPeer;
    a.seq = ++seq_;
    versions_.setAll(keysOf(a.delta), a.seq);
    if (onAccepted)
        onAccepted(a);
}

Submitted Host::submitEncoded(PeerId from, std::span<const std::uint8_t> bytes,
                              std::uint64_t nowMs) {
    if (bytes.size() > cfg_.limits.maxBytes)
        return reject(RejectReason::TooLarge, "operation is too large");
    Result<Op> op = decodeOp(bytes, cfg_.limits);
    if (!op)
        return reject(RejectReason::Malformed, op.error().message);
    return submit(from, op.value(), nowMs);
}

Submitted Host::submit(PeerId from, const Op& op, std::uint64_t nowMs) {
    auto pit = peers_.find(from);
    if (pit == peers_.end())
        return reject(RejectReason::UnknownPeer, "not a member of this session");
    Peer& peer = pit->second;
    if (peer.role != Role::Editor)
        return reject(RejectReason::NotAllowed, "viewers cannot edit");
    if (op.author != from)
        return reject(RejectReason::Malformed, "operation names another author");

    // Rate limit: a sliding window of timestamps.
    auto& recent = peer.recent;
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [&](std::uint64_t t) { return t + cfg_.rateWindowMs <= nowMs; }),
                 recent.end());
    if (recent.size() >= cfg_.maxOpsPerWindow)
        return reject(RejectReason::RateLimited, "too many edits too quickly");
    recent.push_back(nowMs);

    if (op.delta.empty())
        return reject(RejectReason::Malformed, "empty operation");
    if (op.delta.entities.size() > cfg_.limits.maxEntities)
        return reject(RejectReason::TooLarge, "too many entities in one operation");

    // The bases must describe exactly the things the edit touches.
    const std::vector<Key> keys = keysOf(op.delta);
    if (op.bases.size() != keys.size())
        return reject(RejectReason::Malformed, "bases do not match the edit");
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (op.bases[i].key != keys[i])
            return reject(RejectReason::Malformed, "bases do not match the edit");
    }

    // Anything changed, created or erased by someone else since the author last heard?
    for (const Base& b : op.bases) {
        std::uint64_t now = versions_.get(b.key);
        if (now != b.version || exists(*doc_, b.key) != b.existed) {
            return reject(RejectReason::Stale,
                          std::string("the ") + spaceName(b.key.space) + " was changed first",
                          b.key, now);
        }
    }

    // New entities must use ids this peer was given.
    for (const io::EntityDelta& e : op.delta.entities) {
        if (e.after && !doc_->contains(e.id) && !ranges_.owns(from, e.id))
            return reject(RejectReason::OutsideRange, "entity id is not in this peer's range",
                          Key{Space::Entity, e.id.value});
    }

    if (policy) {
        if (std::string why = policy(from, op, *doc_); !why.empty())
            return reject(RejectReason::NotAllowed, std::move(why));
    }

    Result<ChangeSet> cs = io::materialize(op.delta, *doc_);
    if (!cs)
        return reject(RejectReason::Invalid, cs.error().message);
    applying_ = true;
    Result<void> done = doc_->applyChecked(cs.value());
    applying_ = false;
    if (!done)
        return reject(RejectReason::Invalid, done.error().message);

    Accepted a;
    a.seq = ++seq_;
    a.author = from;
    a.clientSeq = op.clientSeq;
    a.delta = op.delta;
    versions_.setAll(keys, a.seq);
    Submitted s;
    s.accepted = a;
    if (onAccepted)
        onAccepted(a);
    return s;
}

}  // namespace tbcad::collab

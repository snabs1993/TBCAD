#include "tbcad/collab/guest_session.h"

#include <algorithm>

#include "tbcad/collab/snapshot.h"

namespace tbcad::collab {

GuestSession::GuestSession(std::string name, SendFn send)
    : name_(std::move(name)), send_(std::move(send)) {}

GuestSession::~GuestSession() = default;

void GuestSession::send(const Message& m) {
    if (send_ && state_ != GuestState::Ended && state_ != GuestState::Denied)
        send_(encodeMessage(m));
}

void GuestSession::start() {
    send(msg::Hello{kProtocolVersion, name_});
}

void GuestSession::leave() {
    if (state_ == GuestState::Joined)
        send(msg::Bye{"left"});
    end(GuestState::Ended, "you left the session");
}

void GuestSession::connectionLost() {
    if (state_ == GuestState::Joined || state_ == GuestState::Connecting)
        end(GuestState::Ended, "the connection to the host was lost");
}

void GuestSession::end(GuestState s, std::string reason) {
    if (state_ == GuestState::Ended || state_ == GuestState::Denied)
        return;
    state_ = s;
    endReason_ = std::move(reason);
    // The copy stays readable (and savable as a copy) but no longer sends anything.
    if (replica_)
        replica_->onLocalOp = {};
    if (onEnded)
        onEnded();
}

void GuestSession::receive(std::span<const std::uint8_t> frame) {
    if (state_ == GuestState::Ended || state_ == GuestState::Denied)
        return;
    Result<Message> m = decodeMessage(frame);
    if (!m) {
        end(GuestState::Ended, "the host sent something this program could not understand");
        return;
    }
    handle(m.value());
}

void GuestSession::handle(const Message& m) {
    if (const auto* w = std::get_if<msg::Welcome>(&m)) {
        if (state_ == GuestState::Connecting)
            joined(*w);
        return;
    }
    if (const auto* d = std::get_if<msg::Denied>(&m)) {
        end(GuestState::Denied, d->reason);
        return;
    }
    if (const auto* b = std::get_if<msg::Bye>(&m)) {
        end(GuestState::Ended, b->reason);
        return;
    }
    if (state_ != GuestState::Joined)
        return;  // nothing else is meaningful before the drawing arrived
    if (const auto* a = std::get_if<msg::AcceptedMsg>(&m)) {
        Result<ChangeSet> r = replica_->apply(a->accepted);
        if (!r) {
            end(GuestState::Ended, "the copy fell out of step with the host: " + r.error().message);
            return;
        }
        if (a->accepted.author != self_ && !r.value().empty() && onRemoteChange)
            onRemoteChange(r.value());
        askForIds();
    } else if (const auto* r = std::get_if<msg::Refused>(&m)) {
        refused(*r);
    } else if (const auto* g = std::get_if<msg::RangeGrant>(&m)) {
        idsRequested_ = false;
        (void)replica_->addRange(g->range);
    } else if (const auto* p = std::get_if<msg::PeerJoined>(&m)) {
        peers_.push_back(p->peer);
        if (onPeerJoined)
            onPeerJoined(p->peer);
    } else if (const auto* p = std::get_if<msg::PeerLeft>(&m)) {
        auto it = std::find_if(peers_.begin(), peers_.end(),
                               [&](const PeerInfo& i) { return i.peer == p->peer; });
        if (it != peers_.end()) {
            PeerInfo gone = *it;
            peers_.erase(it);
            if (onPeerLeft)
                onPeerLeft(gone);
        }
    } else if (const auto* c = std::get_if<msg::RoleChanged>(&m)) {
        if (c->peer == self_) {
            role_ = c->role;
            if (onRoleChanged)
                onRoleChanged(role_);
        }
        for (PeerInfo& i : peers_)
            if (i.peer == c->peer)
                i.role = c->role;
    }
}

void GuestSession::joined(const msg::Welcome& w) {
    Result<Snapshot> snap = loadSnapshot(w.snapshot);
    if (!snap) {
        end(GuestState::Ended, "could not open the host's drawing: " + snap.error().message);
        return;
    }
    self_ = w.peer;
    role_ = w.role;
    hostName_ = w.hostName;
    peers_ = w.peers;
    doc_ = std::move(snap.value().document);
    replica_ = std::make_unique<Replica>(*doc_, self_);
    replica_->restore(std::move(snap.value().versions), snap.value().sequence);
    (void)replica_->addRange(w.range);
    replica_->onLocalOp = [this](const Op& op) { send(msg::OpMsg{op}); };
    replica_->onOwnAccepted = [this](const ChangeSet& cs, std::uint64_t seq, std::uint64_t cseq) {
        ownAccepted(cs, seq, cseq);
    };
    state_ = GuestState::Joined;
    if (onJoined)
        onJoined();
}

void GuestSession::askForIds() {
    if (!idsRequested_ && replica_->idsLeft() < lowIds_) {
        idsRequested_ = true;
        send(msg::RangeRequest{});
    }
}

void GuestSession::ownAccepted(const ChangeSet& applied, std::uint64_t seq,
                               std::uint64_t clientSeq) {
    Entry e;
    e.name = applied.name;
    e.applied = applied;
    e.seq = seq;
    e.keys = keysOf(io::deltaOf(applied));
    Origin origin = Origin::User;
    if (auto it = sent_.find(clientSeq); it != sent_.end()) {
        origin = it->second.origin;
        sent_.erase(it);
    }
    switch (origin) {
        case Origin::User:
            undo_.push_back(std::move(e));
            redo_.clear();
            break;
        case Origin::Undo:
            redo_.push_back(std::move(e));
            break;
        case Origin::Redo:
            undo_.push_back(std::move(e));
            break;
    }
}

void GuestSession::refused(const msg::Refused& m) {
    Result<ChangeSet> r = replica_->reject(m.clientSeq);
    (void)r;
    Conflict c;
    c.reason = m.reason;
    c.key = m.key;
    c.byName = m.byName;
    if (m.reason == RejectReason::Stale && !m.byName.empty())
        c.message = m.byName + " changed this first, so your change was undone.";
    else if (m.reason == RejectReason::Stale)
        c.message = "Someone changed this first, so your change was undone.";
    else if (m.reason == RejectReason::NotAllowed)
        c.message = "You can only look at this drawing; your change was undone.";
    else
        c.message = "The host did not accept your change: " + m.message;
    if (auto it = sent_.find(m.clientSeq); it != sent_.end()) {
        c.wasUndo = true;
        // The undo or redo did not happen: the entry can be tried again.
        (it->second.origin == Origin::Undo ? undo_ : redo_).push_back(std::move(it->second.taken));
        sent_.erase(it);
    }
    if (onConflict)
        onConflict(c);
}

Result<void> GuestSession::move(std::vector<Entry>& from, std::vector<Entry>& to, Origin origin) {
    (void)to;
    if (state_ != GuestState::Joined)
        return makeError(ErrorCode::Conflict, "not connected to a session");
    if (role_ == Role::Viewer)
        return makeError(ErrorCode::Conflict, "you can only look at this drawing");
    if (from.empty())
        return makeError(ErrorCode::NotFound, "nothing to undo");
    if (replica_->pendingCount() != 0)
        return makeError(ErrorCode::Conflict, "wait until your last change has been saved");
    Entry entry = std::move(from.back());
    from.pop_back();
    for (const Key& k : entry.keys) {
        if (replica_->versions().get(k) != entry.seq) {
            return makeError(ErrorCode::Conflict, std::string("someone else changed the ") +
                                                      spaceName(k.space) + " since, so \"" +
                                                      entry.name + "\" can not be taken back");
        }
    }
    if (Result<void> r = doc_->applyChecked(entry.applied.inverted()); !r) {
        return makeError(ErrorCode::Conflict,
                         "\"" + entry.name + "\" can not be taken back: " + r.error().message);
    }
    Sent s;
    s.origin = origin;
    s.taken = std::move(entry);
    sent_[replica_->lastClientSeq()] = std::move(s);
    return {};
}

Result<void> GuestSession::undo() {
    return move(undo_, redo_, Origin::Undo);
}

Result<void> GuestSession::redo() {
    if (redo_.empty() && state_ == GuestState::Joined)
        return makeError(ErrorCode::NotFound, "nothing to redo");
    return move(redo_, undo_, Origin::Redo);
}

}  // namespace tbcad::collab

#include "tbcad/collab/session.h"

#include <algorithm>

#include "bytes.h"
#include "tbcad/collab/snapshot.h"

namespace tbcad::collab {

namespace {

using detail::FormatError;
using detail::Reader;
using detail::Writer;

enum Msg : std::uint8_t {
    kHello = 1,
    kWelcome = 2,
    kOp = 3,
    kAccepted = 4,
    kRejected = 5,
    kGrantRequest = 6,
    kGrant = 7,
    kPeerJoined = 8,
    kPeerLeft = 9,
    kKicked = 10,
    kError = 11,
    kBye = 12,
    kRoleChanged = 13,
};

constexpr std::size_t kMaxName = 200;
constexpr std::size_t kMaxMessage = 2000;
constexpr std::size_t kMaxPeers = 4096;
constexpr std::size_t kAuthorMemory = 4096;

void putString(Writer& w, const std::string& s, std::size_t limit) {
    std::size_t n = std::min(s.size(), limit);
    w.varint(n);
    for (std::size_t i = 0; i < n; ++i)
        w.u8(static_cast<std::uint8_t>(s[i]));
}

std::string getString(Reader& r, std::size_t limit) {
    std::uint64_t n = r.varint();
    if (n > limit || n > r.remaining())
        throw FormatError("string too long");
    std::string s;
    s.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i)
        s.push_back(static_cast<char>(r.u8()));
    return s;
}

void putPeer(Writer& w, const PeerInfo& p) {
    w.varint(p.id);
    w.u8(static_cast<std::uint8_t>(p.role));
    putString(w, p.name, kMaxName);
}

PeerInfo getPeer(Reader& r) {
    PeerInfo p;
    p.id = static_cast<PeerId>(r.varint());
    std::uint8_t role = r.u8();
    p.role = role == static_cast<std::uint8_t>(Role::Viewer) ? Role::Viewer : Role::Editor;
    p.name = getString(r, kMaxName);
    return p;
}

Frame frameOf(Msg type) {
    return Frame{type};
}

Frame textFrame(Msg type, const std::string& text) {
    Writer w;
    w.u8(type);
    putString(w, text, kMaxMessage);
    return std::move(w.bytes());
}

Frame rejectedFrame(std::uint64_t clientSeq, const Rejection& rej) {
    Writer w;
    w.u8(kRejected);
    w.varint(clientSeq);
    w.u8(static_cast<std::uint8_t>(rej.reason));
    putString(w, rej.message, kMaxMessage);
    w.u8(rej.key ? 1 : 0);
    if (rej.key) {
        w.u8(static_cast<std::uint8_t>(rej.key->space));
        w.varint(rej.key->id);
    }
    w.varint(rej.currentVersion);
    return std::move(w.bytes());
}

}  // namespace

// ---- HostSession -------------------------------------------------------------------------------

HostSession::HostSession(Host& host, std::string sessionName)
    : host_(&host), name_(std::move(sessionName)) {
    host_->onAccepted = [this](const Accepted& a) {
        Writer w;
        w.u8(kAccepted);
        w.raw(encodeAccepted(a));
        broadcast(w.bytes());
    };
}

HostSession::~HostSession() {
    host_->onAccepted = {};
}

ConnId HostSession::accept(SendFn send, std::function<void()> close) {
    ConnId id = nextConn_++;
    conns_[id] = Conn{std::move(send), std::move(close), {}};
    return id;
}

void HostSession::sendTo(Conn& c, Frame frame) {
    if (c.send)
        c.send(frame);
}

void HostSession::broadcast(const Frame& frame, std::optional<PeerId> except) {
    for (auto& [peer, conn] : connOf_) {
        if (except && *except == peer)
            continue;
        auto it = conns_.find(conn);
        if (it != conns_.end())
            sendTo(it->second, frame);
    }
}

void HostSession::fail(ConnId id, const std::string& message) {
    auto it = conns_.find(id);
    if (it == conns_.end())
        return;
    sendTo(it->second, textFrame(kError, message));
    leave(id, true);
}

void HostSession::leave(ConnId id, bool notifyClose) {
    auto it = conns_.find(id);
    if (it == conns_.end())
        return;
    Conn conn = std::move(it->second);
    conns_.erase(it);
    std::optional<PeerInfo> gone;
    if (conn.peer) {
        auto p = peers_.find(*conn.peer);
        if (p != peers_.end()) {
            gone = p->second;
            peers_.erase(p);
        }
        connOf_.erase(*conn.peer);
        host_->removePeer(*conn.peer);
    }
    if (gone) {
        Writer w;
        w.u8(kPeerLeft);
        w.varint(gone->id);
        broadcast(w.bytes());
        if (onPeer)
            onPeer(*gone, false);
    }
    if (notifyClose && conn.close)
        conn.close();
}

void HostSession::disconnect(ConnId conn) {
    leave(conn, false);
}

void HostSession::receive(ConnId id, std::span<const std::uint8_t> frame, std::uint64_t nowMs) {
    auto it = conns_.find(id);
    if (it == conns_.end())
        return;
    if (frame.empty()) {
        fail(id, "empty message");
        return;
    }
    const std::uint8_t type = frame[0];
    std::span<const std::uint8_t> body = frame.subspan(1);
    if (type == kHello) {
        if (it->second.peer) {
            fail(id, "already joined");
            return;
        }
        handleHello(id, body);
        return;
    }
    if (!it->second.peer) {
        fail(id, "send Hello first");
        return;
    }
    const PeerId peer = *it->second.peer;
    switch (type) {
        case kOp: {
            Submitted s = host_->submitEncoded(peer, body, nowMs);
            if (s.ok())
                return;  // the broadcast (onAccepted) already went to everybody
            std::uint64_t clientSeq = 0;
            bool decoded = false;
            if (Result<Op> op = decodeOp(body, host_->limits()); op.ok()) {
                clientSeq = op.value().clientSeq;
                decoded = true;
            }
            if (onRefused) {
                if (auto p = peers_.find(peer); p != peers_.end())
                    onRefused(p->second, *s.rejection);
            }
            if (!decoded) {
                fail(id, "damaged edit: " + s.rejection->message);
                return;
            }
            sendTo(conns_[id], rejectedFrame(clientSeq, *s.rejection));
            return;
        }
        case kGrantRequest: {
            IdRange r = host_->grantRange(peer);
            Writer w;
            w.u8(kGrant);
            w.varint(r.first);
            w.varint(r.limit);
            sendTo(conns_[id], std::move(w.bytes()));
            return;
        }
        case kBye:
            leave(id, true);
            return;
        default:
            fail(id, "unknown message");
            return;
    }
}

void HostSession::handleHello(ConnId id, std::span<const std::uint8_t> body) {
    std::string name;
    try {
        Reader r(body);
        if (r.varint() != kProtocolVersion) {
            fail(id, "this program speaks a different collaboration version");
            return;
        }
        name = getString(r, kMaxName);
    } catch (const FormatError&) {
        fail(id, "damaged hello");
        return;
    }
    Role role = Role::Editor;
    if (approve && !approve(name, role)) {
        fail(id, "the host did not let you in");
        return;
    }
    const PeerId peer = host_->addPeer(role);
    IdRange first = host_->grantRange(peer);
    Result<std::vector<std::uint8_t>> snap =
        saveSnapshot(host_->document(), host_->versions(), host_->sequence());
    if (!snap) {
        host_->removePeer(peer);
        fail(id, "could not prepare the drawing: " + snap.error().message);
        return;
    }
    PeerInfo info{peer, name, role};

    Writer w;
    w.u8(kWelcome);
    w.varint(peer);
    w.u8(static_cast<std::uint8_t>(role));
    putString(w, name_, kMaxName);
    w.varint(peers_.size());
    for (const auto& [pid, p] : peers_)
        putPeer(w, p);
    w.varint(first.first);
    w.varint(first.limit);
    w.raw(snap.value());

    Conn& c = conns_[id];
    c.peer = peer;
    peers_[peer] = info;
    connOf_[peer] = id;
    sendTo(c, std::move(w.bytes()));

    Writer j;
    j.u8(kPeerJoined);
    putPeer(j, info);
    broadcast(j.bytes(), peer);
    if (onPeer)
        onPeer(info, true);
}

std::vector<PeerInfo> HostSession::peers() const {
    std::vector<PeerInfo> out;
    for (const auto& [id, p] : peers_)
        out.push_back(p);
    return out;
}

bool HostSession::kick(PeerId peer, const std::string& why) {
    auto c = connOf_.find(peer);
    if (c == connOf_.end())
        return false;
    ConnId id = c->second;
    sendTo(conns_[id], textFrame(kKicked, why));
    leave(id, true);
    return true;
}

bool HostSession::setRole(PeerId peer, Role role) {
    auto c = connOf_.find(peer);
    if (c == connOf_.end() || !host_->setRole(peer, role))
        return false;
    peers_[peer].role = role;
    Writer w;
    w.u8(kRoleChanged);
    w.u8(static_cast<std::uint8_t>(role));
    sendTo(conns_[c->second], std::move(w.bytes()));
    return true;
}

// ---- GuestSession ------------------------------------------------------------------------------

GuestSession::GuestSession(SendFn send) : send_(std::move(send)) {}

GuestSession::~GuestSession() {
    if (replica_)
        replica_->onLocalOp = {};
}

void GuestSession::join(const std::string& name) {
    if (state_ != State::Idle)
        return;
    state_ = State::Joining;
    Writer w;
    w.u8(kHello);
    w.varint(kProtocolVersion);
    putString(w, name, kMaxName);
    send_(w.bytes());
}

void GuestSession::leave() {
    if (state_ == State::Joined || state_ == State::Joining)
        send_(frameOf(kBye));
    state_ = State::Closed;
    closeReason_ = "left the session";
}

void GuestSession::disconnected() {
    if (state_ != State::Closed)
        close("the connection to the host was lost");
}

void GuestSession::close(const std::string& why) {
    if (state_ == State::Closed)
        return;
    state_ = State::Closed;
    closeReason_ = why;
    if (replica_)
        replica_->onLocalOp = {};
    if (onClosed)
        onClosed(why);
}

void GuestSession::topUpIds() {
    if (!replica_ || grantAsked_ || firstGrant_ == 0)
        return;
    if (replica_->idsLeft() < firstGrant_ / 2) {
        grantAsked_ = true;
        send_(frameOf(kGrantRequest));
    }
}

void GuestSession::handleWelcome(std::span<const std::uint8_t> body) {
    try {
        Reader r(body);
        self_ = static_cast<PeerId>(r.varint());
        role_ = r.u8() == static_cast<std::uint8_t>(Role::Viewer) ? Role::Viewer : Role::Editor;
        (void)getString(r, kMaxName);  // the session's name; the UI may show it later
        std::size_t n = r.count(3);
        if (n > kMaxPeers)
            throw FormatError("too many peers");
        peers_.clear();
        for (std::size_t i = 0; i < n; ++i)
            peers_.push_back(getPeer(r));
        IdRange first;
        first.first = r.varint();
        first.limit = r.varint();
        Result<Snapshot> snap = loadSnapshot(r.rest());
        if (!snap) {
            close("the drawing could not be read: " + snap.error().message);
            return;
        }
        doc_ = std::move(snap.value().document);
        if (onDocument)
            onDocument(*doc_);
        replica_ = std::make_unique<Replica>(*doc_, self_);
        replica_->restore(std::move(snap.value().versions), snap.value().sequence);
        if (Result<void> added = replica_->addRange(first); !added) {
            close("bad id block from the host");
            return;
        }
        firstGrant_ = first.size();
        replica_->onLocalOp = [this](const Op& op) {
            Writer w;
            w.u8(kOp);
            w.raw(encodeOp(op));
            send_(w.bytes());
            topUpIds();
        };
    } catch (const FormatError& e) {
        close(std::string("damaged welcome: ") + e.what());
        return;
    }
    state_ = State::Joined;
    if (onJoined)
        onJoined();
}

void GuestSession::receive(std::span<const std::uint8_t> frame) {
    if (state_ == State::Closed || frame.empty())
        return;
    const std::uint8_t type = frame[0];
    std::span<const std::uint8_t> body = frame.subspan(1);
    try {
        if (type == kError || type == kKicked) {
            Reader r(body);
            close(getString(r, kMaxMessage));
            return;
        }
        if (state_ == State::Joining) {
            if (type == kWelcome)
                handleWelcome(body);
            else
                close("unexpected message while joining");
            return;
        }
        if (state_ != State::Joined)
            return;
        switch (type) {
            case kAccepted: {
                Result<Accepted> a = decodeAccepted(body);
                if (!a) {
                    close("damaged broadcast: " + a.error().message);
                    return;
                }
                authors_[a.value().seq] = a.value().author;
                while (authors_.size() > kAuthorMemory)
                    authors_.erase(authors_.begin());
                Result<ChangeSet> applied = replica_->apply(a.value());
                if (!applied) {
                    close("out of step with the host: " + applied.error().message);
                    return;
                }
                topUpIds();
                break;
            }
            case kRejected: {
                Reader r(body);
                Conflict c;
                c.clientSeq = r.varint();
                std::uint8_t reason = r.u8();
                c.reason = reason <= static_cast<std::uint8_t>(RejectReason::TooLarge)
                               ? static_cast<RejectReason>(reason)
                               : RejectReason::Invalid;
                c.message = getString(r, kMaxMessage);
                if (r.u8()) {
                    Key k;
                    std::uint8_t space = r.u8();
                    if (space > kLastSpace)
                        throw FormatError("bad key");
                    k.space = static_cast<Space>(space);
                    k.id = r.varint();
                    c.key = k;
                }
                std::uint64_t version = r.varint();
                if (auto who = authors_.find(version); who != authors_.end()) {
                    for (const PeerInfo& p : peers_)
                        if (p.id == who->second)
                            c.by = p;
                }
                Result<ChangeSet> undone = replica_->reject(c.clientSeq);
                if (undone)
                    c.undone = std::move(undone.value());
                if (onConflict)
                    onConflict(c);
                break;
            }
            case kGrant: {
                Reader r(body);
                IdRange g;
                g.first = r.varint();
                g.limit = r.varint();
                grantAsked_ = false;
                if (Result<void> added = replica_->addRange(g); !added)
                    close("bad id block from the host");
                break;
            }
            case kPeerJoined: {
                Reader r(body);
                PeerInfo p = getPeer(r);
                if (peers_.size() < kMaxPeers)
                    peers_.push_back(p);
                if (onPeer)
                    onPeer(p, true);
                break;
            }
            case kPeerLeft: {
                Reader r(body);
                PeerId id = static_cast<PeerId>(r.varint());
                auto it = std::find_if(peers_.begin(), peers_.end(),
                                       [&](const PeerInfo& p) { return p.id == id; });
                if (it != peers_.end()) {
                    PeerInfo gone = *it;
                    peers_.erase(it);
                    if (onPeer)
                        onPeer(gone, false);
                }
                break;
            }
            case kRoleChanged: {
                Reader r(body);
                role_ =
                    r.u8() == static_cast<std::uint8_t>(Role::Viewer) ? Role::Viewer : Role::Editor;
                break;
            }
            default:
                close("unknown message from the host");
                break;
        }
    } catch (const FormatError& e) {
        close(std::string("damaged message: ") + e.what());
    }
}

}  // namespace tbcad::collab

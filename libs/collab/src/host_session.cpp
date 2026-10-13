#include "tbcad/collab/host_session.h"

#include <algorithm>

#include "tbcad/collab/snapshot.h"

namespace tbcad::collab {

HostSession::HostSession(Host& host, HostSessionConfig config)
    : host_(&host), cfg_(std::move(config)) {
    host_->onAccepted = [this](const Accepted& a) { accepted(a); };
}

HostSession::~HostSession() {
    host_->onAccepted = {};
}

ConnectionId HostSession::open(SendFn send) {
    ConnectionId id = nextConnection_++;
    connections_[id].send = std::move(send);
    return id;
}

HostSession::Connection* HostSession::byPeer(PeerId peer) {
    for (auto& [id, c] : connections_)
        if (c.joined && c.peer == peer)
            return &c;
    return nullptr;
}

void HostSession::sendTo(Connection& c, const Message& m) {
    if (c.send)
        c.send(encodeMessage(m));
}

void HostSession::broadcast(const Message& m, PeerId except) {
    const std::vector<std::uint8_t> bytes = encodeMessage(m);
    for (auto& [id, c] : connections_)
        if (c.joined && c.peer != except && c.send)
            c.send(bytes);
}

void HostSession::accepted(const Accepted& a) {
    authors_[a.seq] = a.author;
    while (authors_.size() > cfg_.authorMemory)
        authors_.erase(authors_.begin());
    broadcast(msg::AcceptedMsg{a});
}

std::vector<PeerInfo> HostSession::peers() const {
    std::vector<PeerInfo> out;
    for (const auto& [id, c] : connections_)
        if (c.joined)
            out.push_back({c.peer, c.name, host_->roleOf(c.peer).value_or(Role::Editor)});
    return out;
}

std::optional<PeerInfo> HostSession::peer(PeerId id) const {
    for (const PeerInfo& p : peers())
        if (p.peer == id)
            return p;
    return std::nullopt;
}

void HostSession::receive(ConnectionId id, std::span<const std::uint8_t> frame,
                          std::uint64_t nowMs) {
    auto it = connections_.find(id);
    if (it == connections_.end())
        return;
    Result<Message> m = decodeMessage(frame, cfg_.limits);
    if (!m) {
        sendTo(it->second, msg::Bye{"the host could not understand a message"});
        drop(id, true);
        return;
    }
    handle(it->second, id, m.value(), nowMs);
}

void HostSession::handle(Connection& c, ConnectionId id, const Message& m, std::uint64_t nowMs) {
    if (const auto* h = std::get_if<msg::Hello>(&m)) {
        if (c.joined) {
            drop(id, true);
            return;
        }
        hello(c, *h);
        if (!c.joined)
            drop(id, false);
        return;
    }
    if (!c.joined) {  // nothing but Hello is allowed first
        drop(id, false);
        return;
    }
    if (const auto* o = std::get_if<msg::OpMsg>(&m)) {
        edit(c, o->op, nowMs);
    } else if (std::get_if<msg::RangeRequest>(&m)) {
        if (host_->ranges().rangesOf(c.peer).size() < cfg_.maxBlocksPerPeer)
            sendTo(c, msg::RangeGrant{host_->grantRange(c.peer)});
    } else if (std::get_if<msg::Bye>(&m)) {
        drop(id, true);
    } else {
        drop(id, true);  // a guest never sends anything else
    }
}

void HostSession::hello(Connection& c, const msg::Hello& m) {
    if (m.protocol != kProtocolVersion) {
        sendTo(c, msg::Denied{"this host speaks a different version of the sharing protocol"});
        return;
    }
    std::string name = m.name.empty() ? "Guest" : m.name;
    if (cfg_.approve && !cfg_.approve(name)) {
        sendTo(c, msg::Denied{"the host did not let you in"});
        return;
    }
    Result<std::vector<std::uint8_t>> snap =
        saveSnapshot(host_->document(), host_->versions(), host_->sequence());
    if (!snap) {
        sendTo(c, msg::Denied{"the host could not prepare the drawing"});
        return;
    }
    msg::Welcome w;
    w.peer = host_->addPeer(cfg_.defaultRole);
    w.role = cfg_.defaultRole;
    w.sequence = host_->sequence();
    w.range = host_->grantRange(w.peer);
    w.hostName = cfg_.hostName;
    w.peers.push_back({kHostPeer, cfg_.hostName, Role::Editor});
    for (const PeerInfo& p : peers())
        w.peers.push_back(p);
    w.snapshot = std::move(snap.value());
    c.joined = true;
    c.peer = w.peer;
    c.name = name;
    sendTo(c, w);
    PeerInfo info{c.peer, c.name, w.role};
    broadcast(msg::PeerJoined{info}, c.peer);
    if (onJoined)
        onJoined(info);
}

void HostSession::edit(Connection& c, const Op& op, std::uint64_t nowMs) {
    Submitted s = host_->submit(c.peer, op, nowMs);
    if (s.ok())
        return;  // already broadcast through onAccepted
    const Rejection& r = *s.rejection;
    msg::Refused m;
    m.clientSeq = op.clientSeq;
    m.reason = r.reason;
    m.message = r.message;
    m.key = r.key;
    m.currentVersion = r.currentVersion;
    if (r.key && r.currentVersion) {
        if (auto it = authors_.find(r.currentVersion); it != authors_.end()) {
            m.byPeer = it->second;
            if (it->second == kHostPeer)
                m.byName = cfg_.hostName;
            else if (auto p = peer(it->second))
                m.byName = p->name;
        }
    }
    sendTo(c, m);
    if (onRejected)
        onRejected(PeerInfo{c.peer, c.name, host_->roleOf(c.peer).value_or(Role::Editor)}, r);
}

void HostSession::close(ConnectionId id) {
    drop(id, true);
}

void HostSession::drop(ConnectionId id, bool notify) {
    auto it = connections_.find(id);
    if (it == connections_.end())
        return;
    Connection c = std::move(it->second);
    connections_.erase(it);
    if (!c.joined)
        return;
    PeerInfo info{c.peer, c.name, host_->roleOf(c.peer).value_or(Role::Editor)};
    host_->removePeer(c.peer);
    broadcast(msg::PeerLeft{c.peer});
    if (notify && onLeft)
        onLeft(info);
}

bool HostSession::kick(PeerId peer, const std::string& reason) {
    for (auto& [id, c] : connections_) {
        if (c.joined && c.peer == peer) {
            sendTo(c, msg::Bye{reason});
            ConnectionId cid = id;
            drop(cid, true);
            return true;
        }
    }
    return false;
}

bool HostSession::setRole(PeerId peer, Role role) {
    if (!host_->setRole(peer, role))
        return false;
    broadcast(msg::RoleChanged{peer, role});
    return true;
}

void HostSession::shutdown(const std::string& reason) {
    broadcast(msg::Bye{reason});
    std::vector<ConnectionId> ids;
    for (auto& [id, c] : connections_)
        ids.push_back(id);
    for (ConnectionId id : ids)
        drop(id, true);
}

}  // namespace tbcad::collab

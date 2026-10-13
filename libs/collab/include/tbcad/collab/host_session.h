#pragma once

// The host side of a session, above the wire: connections, the join handshake, answers and
// broadcasts. It works on bytes only; the owner connects it to sockets (or, in tests, queues).
//
//   HostSession session(host, {.hostName = "Nok"});
//   ConnectionId c = session.open([&](bytes) { send to that guest });
//   session.receive(c, frame, nowMs);   // for every frame from that guest
//   session.close(c);                   // when the connection ends
//
// Everything runs on the thread that owns the host's document.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "tbcad/collab/host.h"
#include "tbcad/collab/protocol.h"

namespace tbcad::collab {

using ConnectionId = std::uint32_t;
using SendFn = std::function<void(const std::vector<std::uint8_t>&)>;

struct HostSessionConfig {
    std::string hostName = "Host";
    Role defaultRole = Role::Editor;
    // Asked for every guest that says hello. Empty = everybody is let in. Return false to deny.
    std::function<bool(const std::string& name)> approve{};
    // A guest asking for ids gets another block only while it holds fewer than this many blocks.
    std::size_t maxBlocksPerPeer = 64;
    // The host remembers who made the latest change of this many recent edits, to tell the loser
    // of a conflict who won.
    std::size_t authorMemory = 4096;
    io::DeltaLimits limits{};  // bounds on what a guest may send
};

class HostSession {
public:
    HostSession(Host& host, HostSessionConfig config = {});
    ~HostSession();
    HostSession(const HostSession&) = delete;
    HostSession& operator=(const HostSession&) = delete;

    Host& host() { return *host_; }

    // A new connection that has not said hello yet. `send` delivers one frame to the guest.
    ConnectionId open(SendFn send);
    // One frame from the guest. A frame that can not be understood ends the connection.
    void receive(ConnectionId c, std::span<const std::uint8_t> frame, std::uint64_t nowMs);
    // The connection ended (or was cut). Tells the others when the guest had joined.
    void close(ConnectionId c);

    // Who is in the session (guests that finished the handshake; the host is not listed).
    std::vector<PeerInfo> peers() const;
    std::optional<PeerInfo> peer(PeerId id) const;
    // Sends Bye with the reason and drops the guest.
    bool kick(PeerId peer, const std::string& reason = "removed by the host");
    bool setRole(PeerId peer, Role role);
    // The session ends: everybody gets Bye.
    void shutdown(const std::string& reason = "the host ended the session");

    // Events for the host's own user interface.
    std::function<void(const PeerInfo&)> onJoined{};
    std::function<void(const PeerInfo&)> onLeft{};
    std::function<void(const PeerInfo&, const Rejection&)> onRejected{};

private:
    struct Connection {
        SendFn send{};
        bool joined = false;
        PeerId peer = 0;
        std::string name{};
    };

    void handle(Connection& c, ConnectionId id, const Message& m, std::uint64_t nowMs);
    void hello(Connection& c, const msg::Hello& m);
    void edit(Connection& c, const Op& op, std::uint64_t nowMs);
    void broadcast(const Message& m, PeerId except = 0);
    void sendTo(Connection& c, const Message& m);
    void drop(ConnectionId id, bool notify);
    Connection* byPeer(PeerId peer);
    void accepted(const Accepted& a);

    Host* host_;
    HostSessionConfig cfg_;
    ConnectionId nextConnection_ = 1;
    std::map<ConnectionId, Connection> connections_{};
    // seq -> author for the most recent edits.
    std::map<std::uint64_t, PeerId> authors_{};
};

}  // namespace tbcad::collab

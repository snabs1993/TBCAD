#pragma once

// The session engine: the messages two programs exchange and the state each side keeps. It works
// on whole frames (byte vectors) and knows nothing about sockets; session_tcp.h moves the frames
// over TCP/TLS and the tests move them through queues. Everything runs on one thread (the one
// that owns the document); the transport calls receive() from that thread.
//
//   host:   HostSession turns frames from connections into Host::submit calls, broadcasts what the
//           Host accepts, answers refusals to the author, hands out id blocks, tracks who is in.
//   guest:  GuestSession joins (receives the drawing as a snapshot), keeps a Replica, sends the
//           user's edits, applies the broadcasts, asks for more ids when they run low, and tells
//           the user when an edit was refused and who got there first.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "tbcad/collab/host.h"
#include "tbcad/collab/replica.h"

namespace tbcad::collab {

inline constexpr std::uint32_t kProtocolVersion = 1;

struct PeerInfo {
    PeerId id = 0;
    std::string name{};
    Role role = Role::Editor;
    friend bool operator==(const PeerInfo&, const PeerInfo&) = default;
};

using Frame = std::vector<std::uint8_t>;
using SendFn = std::function<void(const Frame&)>;

using ConnId = std::uint32_t;

class HostSession {
public:
    // `host` stays the caller's; its onAccepted is taken over by the session.
    explicit HostSession(Host& host, std::string sessionName = {});
    ~HostSession();
    HostSession(const HostSession&) = delete;
    HostSession& operator=(const HostSession&) = delete;

    // A new connection (not yet a participant until its Hello is approved). `send` delivers a
    // frame to it; `close` is called when the session ends the connection (kick, bad message).
    ConnId accept(SendFn send, std::function<void()> close = {});
    // A frame arrived on the connection. `nowMs` is a monotonic clock for rate limiting.
    void receive(ConnId conn, std::span<const std::uint8_t> frame, std::uint64_t nowMs);
    // The transport lost the connection.
    void disconnect(ConnId conn);

    std::vector<PeerInfo> peers() const;
    bool kick(PeerId peer, const std::string& why = "removed by the host");
    bool setRole(PeerId peer, Role role);

    // Decides about a join request. Default: allow as editor. Return false to refuse; `role` may
    // be changed to admit the person as a viewer.
    std::function<bool(const std::string& name, Role& role)> approve{};
    // Somebody joined (true) or left (false).
    std::function<void(const PeerInfo&, bool joined)> onPeer{};
    // An edit from a guest was refused (for a log or a toast on the host).
    std::function<void(const PeerInfo&, const Rejection&)> onRefused{};

    Host& host() { return *host_; }

private:
    struct Conn {
        SendFn send{};
        std::function<void()> close{};
        std::optional<PeerId> peer{};
    };
    void sendTo(Conn& c, Frame frame);
    void broadcast(const Frame& frame, std::optional<PeerId> except = {});
    void fail(ConnId id, const std::string& message);
    void handleHello(ConnId id, std::span<const std::uint8_t> body);
    void leave(ConnId id, bool notifyClose);

    Host* host_;
    std::string name_;
    ConnId nextConn_ = 1;
    std::unordered_map<ConnId, Conn> conns_{};
    std::map<PeerId, PeerInfo> peers_{};
    std::map<PeerId, ConnId> connOf_{};
};

// What the guest tells the user when the host refused one of its edits.
struct Conflict {
    std::uint64_t clientSeq = 0;
    RejectReason reason = RejectReason::Stale;
    std::string message{};         // the host's wording
    std::optional<Key> key{};      // what was changed first
    std::optional<PeerInfo> by{};  // who changed it, when known
    ChangeSet undone{};            // what left the document again
};

class GuestSession {
public:
    enum class State { Idle, Joining, Joined, Closed };

    explicit GuestSession(SendFn send);
    ~GuestSession();
    GuestSession(const GuestSession&) = delete;
    GuestSession& operator=(const GuestSession&) = delete;

    // Asks to join; the answer arrives through receive().
    void join(const std::string& name);
    void receive(std::span<const std::uint8_t> frame);
    // Leaves politely.
    void leave();
    // The transport lost the connection.
    void disconnected();

    State state() const { return state_; }
    // Valid once Joined (the document and the replica exist until the session is destroyed).
    Document* document() { return doc_.get(); }
    Replica* replica() { return replica_.get(); }
    PeerId self() const { return self_; }
    Role role() const { return role_; }
    const std::vector<PeerInfo>& peers() const { return peers_; }
    const std::string& closeReason() const { return closeReason_; }

    std::function<void()> onJoined{};
    std::function<void(const Conflict&)> onConflict{};
    std::function<void(const PeerInfo&, bool joined)> onPeer{};
    // The session ended: kicked, protocol error, or the connection broke.
    std::function<void(const std::string& why)> onClosed{};
    // The Document is replaced when the guest joins; the application hooks it up here (observers,
    // the undo stack guard ...) before the first edit is possible.
    std::function<void(Document&)> onDocument{};

private:
    void handleWelcome(std::span<const std::uint8_t> body);
    void topUpIds();
    void close(const std::string& why);

    SendFn send_;
    State state_ = State::Idle;
    PeerId self_ = 0;
    Role role_ = Role::Editor;
    std::unique_ptr<Document> doc_{};
    std::unique_ptr<Replica> replica_{};
    std::vector<PeerInfo> peers_{};
    std::map<std::uint64_t, PeerId> authors_{};  // sequence number -> who made it (recent ones)
    std::uint64_t firstGrant_ = 0;
    bool grantAsked_ = false;
    std::string closeReason_{};
};

}  // namespace tbcad::collab

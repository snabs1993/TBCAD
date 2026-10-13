#pragma once

// The messages of a collaboration session. One message is one frame on the connection (see
// net::sendFrame); the first byte tells which. Nothing here knows about sockets.
//
// Join:   guest -> Hello;  host -> Welcome (who you are, the drawing, a block of ids, who is here)
//         or Denied.
// Edit:   guest -> Op;     host -> Accepted to everybody (the author included), or Refused to
//         the author only.
// Other:  RangeRequest/RangeGrant (more ids), PeerJoined/PeerLeft/RoleChanged, Bye (kicked or
//         the session ended).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "tbcad/collab/host.h"
#include "tbcad/collab/ops.h"

namespace tbcad::collab {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxNameBytes = 128;
inline constexpr std::size_t kMaxMessageBytes = 192;

struct PeerInfo {
    PeerId peer = 0;
    std::string name{};
    Role role = Role::Editor;
    friend bool operator==(const PeerInfo&, const PeerInfo&) = default;
};

namespace msg {
struct Hello {
    std::uint32_t protocol = kProtocolVersion;
    std::string name{};
};
struct Welcome {
    PeerId peer = 0;
    Role role = Role::Editor;
    std::uint64_t sequence = 0;
    IdRange range{};
    std::string hostName{};
    std::vector<PeerInfo> peers{};         // everybody else in the session, the host included
    std::vector<std::uint8_t> snapshot{};  // .tbcad image with the versions chunk
};
struct Denied {
    std::string reason{};
};
struct OpMsg {
    Op op{};
};
struct AcceptedMsg {
    Accepted accepted{};
};
struct Refused {
    std::uint64_t clientSeq = 0;
    RejectReason reason = RejectReason::Malformed;
    std::string message{};
    // For Stale: what was changed first, its version, and who changed it (name is empty when
    // that peer has left or the version is older than the host remembers).
    std::optional<Key> key{};
    std::uint64_t currentVersion = 0;
    PeerId byPeer = 0;
    std::string byName{};
};
struct RangeRequest {};
struct RangeGrant {
    IdRange range{};
};
struct PeerJoined {
    PeerInfo peer{};
};
struct PeerLeft {
    PeerId peer = 0;
};
struct RoleChanged {
    PeerId peer = 0;
    Role role = Role::Editor;
};
struct Bye {
    std::string reason{};
};
}  // namespace msg

using Message = std::variant<msg::Hello, msg::Welcome, msg::Denied, msg::OpMsg, msg::AcceptedMsg,
                             msg::Refused, msg::RangeRequest, msg::RangeGrant, msg::PeerJoined,
                             msg::PeerLeft, msg::RoleChanged, msg::Bye>;

std::vector<std::uint8_t> encodeMessage(const Message& m);
// `limits` bound the edit payloads; a message that does not fit its format is an error.
Result<Message> decodeMessage(std::span<const std::uint8_t> bytes,
                              const io::DeltaLimits& limits = {});

}  // namespace tbcad::collab

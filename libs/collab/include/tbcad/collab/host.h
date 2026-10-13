#pragma once

// The host side of a session: the one process that orders every edit.
//
// The Host owns no document; it works on the one it is given, which stays the host's own
// working document. Edits the host makes itself are noticed through the document observer and
// recorded like any other; edits that arrive from guests go through submit().

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "tbcad/collab/ids.h"
#include "tbcad/collab/ops.h"
#include "tbcad/collab/versions.h"
#include "tbcad/core/document.h"

namespace tbcad::collab {

enum class Role : std::uint8_t { Editor, Viewer };

struct HostConfig {
    std::uint64_t idBlockSize = std::uint64_t{1} << 16;
    // A guest may send this many ops per `rateWindowMs`.
    std::size_t maxOpsPerWindow = 200;
    std::uint64_t rateWindowMs = 1000;
    io::DeltaLimits limits{};
};

enum class RejectReason : std::uint8_t {
    Malformed,  // undecodable, or bases that do not match the edit
    UnknownPeer,
    NotAllowed,    // viewer
    Stale,         // someone else changed (or erased, or created) one of the things first
    OutsideRange,  // creates an entity with an id that is not the guest's to use
    Invalid,       // breaks a rule of the drawing (unknown layer, duplicate name ...)
    RateLimited,
    TooLarge,
};

struct Rejection {
    RejectReason reason = RejectReason::Malformed;
    std::string message{};
    // For Stale: the thing that was changed first and who changed it.
    std::optional<Key> key{};
    std::uint64_t currentVersion = 0;
};

struct Submitted {
    std::optional<Accepted> accepted{};
    std::optional<Rejection> rejection{};
    bool ok() const { return accepted.has_value(); }
};

class Host : private DocumentObserver {
public:
    explicit Host(Document& doc, HostConfig config = {});
    ~Host() override;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    Document& document() { return *doc_; }
    const VersionMap& versions() const { return versions_; }
    std::uint64_t sequence() const { return seq_; }
    // Restores versions saved with the file (see snapshot.h).
    void restore(VersionMap versions, std::uint64_t sequence);

    PeerId addPeer(Role role);
    void removePeer(PeerId peer);
    bool setRole(PeerId peer, Role role);
    std::optional<Role> roleOf(PeerId peer) const;
    // A block of entity ids for the peer to draw from; call again when it runs low.
    IdRange grantRange(PeerId peer);
    const IdRanges& ranges() const { return ranges_; }

    // A guest's edit. On success it is already applied to the host's document.
    Submitted submit(PeerId from, const Op& op, std::uint64_t nowMs);
    Submitted submitEncoded(PeerId from, std::span<const std::uint8_t> bytes, std::uint64_t nowMs);

    // Called for every accepted edit, the host's own included, in sequence order. This is where
    // a session sends it to the guests.
    std::function<void(const Accepted&)> onAccepted{};

private:
    void onChanged(const ChangeSet& changes) override;
    Submitted reject(RejectReason reason, std::string message, std::optional<Key> key = {},
                     std::uint64_t current = 0);

    struct Peer {
        Role role = Role::Editor;
        std::vector<std::uint64_t> recent{};  // times of the ops inside the rate window
    };

    Document* doc_;
    HostConfig cfg_;
    VersionMap versions_{};
    std::uint64_t seq_ = 0;
    PeerId nextPeer_ = 1;
    std::unordered_map<PeerId, Peer> peers_{};
    IdRanges ranges_;
    bool applying_ = false;
};

}  // namespace tbcad::collab

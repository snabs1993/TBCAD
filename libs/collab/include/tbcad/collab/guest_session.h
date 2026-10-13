#pragma once

// The guest side of a session, above the wire. Like HostSession it handles bytes only.
//
//   GuestSession guest("Somchai", [&](bytes) { send to the host });
//   guest.start();                       // sends Hello
//   guest.receive(frame);                // for every frame from the host
//   guest.document()                     // the guest's copy, once state() == Joined
//
// The user edits `document()` as usual (commands, undo of the *document* is not used): every
// commit is sent to the host optimistically. Use undo()/redo() of this class instead of the
// document's: they take back only this user's own edits.

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tbcad/collab/host_session.h"
#include "tbcad/collab/replica.h"

namespace tbcad::collab {

enum class GuestState : std::uint8_t { Connecting, Joined, Denied, Ended };

// What the user is told when the host refused one of their edits.
struct Conflict {
    RejectReason reason = RejectReason::Stale;
    std::string message{};  // "Somchai changed this first." style text, ready to show
    std::optional<Key> key{};
    std::string byName{};
    bool wasUndo = false;  // the refused edit was an undo or redo
};

class GuestSession {
public:
    GuestSession(std::string name, SendFn send);
    ~GuestSession();
    GuestSession(const GuestSession&) = delete;
    GuestSession& operator=(const GuestSession&) = delete;

    GuestState state() const { return state_; }
    const std::string& endReason() const { return endReason_; }  // why Denied / Ended
    PeerId self() const { return self_; }
    Role role() const { return role_; }
    const std::string& hostName() const { return hostName_; }
    const std::vector<PeerInfo>& peers() const { return peers_; }  // everybody else
    // Null until the host has sent the drawing.
    Document* document() { return doc_.get(); }
    Replica* replica() { return replica_.get(); }

    void start();  // sends Hello
    void receive(std::span<const std::uint8_t> frame);
    // The connection ended without a Bye.
    void connectionLost();
    // Leaves politely.
    void leave();

    // ---- this user's own undo ---------------------------------------------------------------
    // Takes back the user's latest accepted edit by sending its inverse as a new edit. Fails
    // (with text for the user) when someone else has changed any of the things it touched since,
    // or when the edit was not accepted by the host yet.
    Result<void> undo();
    Result<void> redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoLabel() const { return undo_.empty() ? "" : undo_.back().name; }

    // ---- events for the user interface -------------------------------------------------------
    std::function<void()> onJoined{};
    std::function<void(const PeerInfo&)> onPeerJoined{};
    std::function<void(const PeerInfo&)> onPeerLeft{};
    std::function<void(Role)> onRoleChanged{};  // own role
    std::function<void(const Conflict&)> onConflict{};
    // Something remote changed the drawing (not the user's own edits).
    std::function<void(const ChangeSet&)> onRemoteChange{};
    std::function<void()> onEnded{};

private:
    struct Entry {
        std::string name{};
        ChangeSet applied{};
        std::uint64_t seq = 0;
        std::vector<Key> keys{};
    };
    enum class Origin : std::uint8_t { User, Undo, Redo };
    struct Sent {
        Origin origin = Origin::User;
        Entry taken{};  // for Undo/Redo: the entry that was moved between the stacks
    };

    void handle(const Message& m);
    void joined(const msg::Welcome& w);
    void ownAccepted(const ChangeSet& applied, std::uint64_t seq, std::uint64_t clientSeq);
    void refused(const msg::Refused& m);
    void end(GuestState s, std::string reason);
    Result<void> move(std::vector<Entry>& from, std::vector<Entry>& to, Origin origin);
    void send(const Message& m);
    void askForIds();

    std::string name_;
    SendFn send_;
    GuestState state_ = GuestState::Connecting;
    std::string endReason_{};
    PeerId self_ = 0;
    Role role_ = Role::Editor;
    std::string hostName_{};
    std::vector<PeerInfo> peers_{};
    std::unique_ptr<Document> doc_{};
    std::unique_ptr<Replica> replica_{};
    std::vector<Entry> undo_{};
    std::vector<Entry> redo_{};
    std::map<std::uint64_t, Sent> sent_{};  // by clientSeq, for undo/redo edits
    bool idsRequested_ = false;
    std::uint64_t lowIds_ = 1000;  // ask for more below this many free ids
};

}  // namespace tbcad::collab

#pragma once

// Live editing of one drawing by several people: what the Share / Join commands and the panel
// drive. It glues the open-source session engine (libs/collab) to the encrypted links (link.h)
// and to the program's tabs. Everything here runs on the main thread; the links own the threads.

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "discovery.h"
#include "link.h"
#include "tbcad/collab/session.h"
#include "tbcad/ui/app_core.h"
#include "team.h"

namespace tbcad::collabapp {

class CollabController {
public:
    enum class Mode { Off, Hosting, Joining, Joined, Offline };

    CollabController(ui::AppCore& core, std::function<void()> wake);
    ~CollabController();
    CollabController(const CollabController&) = delete;
    CollabController& operator=(const CollabController&) = delete;

    // Shares the drawing that is open now. `port` 0 picks a free one.
    bool share(int port, const std::string& password, const std::string& name, bool guestsMayEdit,
               std::string& error);
    // Joins a shared drawing; it opens as a new tab when the host has sent it.
    bool join(const std::string& host, int port, const std::string& password,
              const std::string& name, std::string& error);
    // Ends the session (guests are told, the drawing stays open on this side).
    void stop(const std::string& why = {});
    // Moves frames between the links and the engine. Once per loop turn.
    void pump();

    Mode mode() const { return mode_; }
    int port() const { return listener_ ? listener_->port() : 0; }
    std::string status() const;
    std::vector<collab::PeerInfo> peers() const;
    // Newest last; kept to a few hundred lines.
    const std::vector<std::string>& log() const { return log_; }
    // Lets a guest in as an editor (true) or as a viewer (false) from now on.
    void setGuestsMayEdit(bool on) { guestsMayEdit_ = on; }
    bool guestsMayEdit() const { return guestsMayEdit_; }
    bool kick(collab::PeerId peer);
    // Other people's cursors (drawing units), for the overlay and the people list. The caller's own
    // cursor is passed in to be shared; it is sent at most ten times a second.
    struct Cursor {
        collab::PeerId peer = 0;
        Vec2 at{};
        std::uint64_t seenMs = 0;
    };
    void shareCursor(Vec2 at);
    std::vector<Cursor> cursors() const;
    static Rgba colorOf(collab::PeerId peer);
    // People who asked to join and wait for the host's answer.
    struct Request {
        collab::ConnId conn = 0;
        std::string name{};
        std::string computer{};  // the asking computer's id (empty from an older program)
    };
    const std::vector<Request>& requests() const { return requests_; }
    // `remember`: let this computer in without asking next time (rememberedComputers()).
    void answer(collab::ConnId conn, bool allow, collab::Role role, bool remember = false);
    void setAskBeforeJoin(bool on) { askBeforeJoin_ = on; }
    bool askBeforeJoin() const { return askBeforeJoin_; }
    // One click: shares on the local network with no code, on port 47800 (any free port when that
    // one is taken), under the computer's user name. The host is asked before anyone joins, except
    // computers it let in before (rememberedComputers()).
    bool shareSimple(std::string& error);
    // One click on a host found on the network (nearbyHosts()) or typed in: no code needed.
    bool joinSimple(const std::string& host, int port, const std::string& name, std::string& error);
    // Computers the host let in before: they join without asking. Forget removes one.
    struct Computer {
        std::string id{};
        std::string name{};
    };
    std::vector<Computer> rememberedComputers() const;
    void forgetComputer(const std::string& id);
    // This computer's id (made once, kept in the settings), sent to the host when joining.
    std::string computerId() const;
    // Address of this computer on the local network (for typing on the other computer).
    std::string localAddress() const;
    static std::string defaultName();
    // Hosts announcing themselves on the local network (while not in a session).
    std::vector<Announcement> nearbyHosts();
    // Chat and layer claims (team.h).
    void sendChat(const std::string& text);
    const std::vector<ChatLine>& chat() const { return chat_; }
    void setClaim(LayerId layer, bool claim);
    const Claims& claims() const { return claims_; }
    collab::PeerId self() const;
    std::string nameOf(collab::PeerId peer) const;
    // Offline: the connection to the host was lost. The drawing stays editable; reconnect() joins
    // again and sends what was done meanwhile. An edit to something another person changed in the
    // meantime is not sent on its own: it waits in conflicts() for keepMine / keepTheirs.
    bool reconnect(std::string& error);
    std::size_t offlineEdits() const;
    struct OfflineConflict {
        std::string what{};
        collab::Op op{};
        ChangeSet applied{};
    };
    const std::vector<OfflineConflict>& conflicts() const { return conflicts_; }
    void keepMine(std::size_t index);
    void keepTheirs(std::size_t index);
    // Host only: hands the host role to a guest. Everybody (this program included) then joins
    // the new host; the drawings stay as they are.
    bool handOver(collab::PeerId to, std::string& error);
    // The drawing being shared (null when not in a session).
    const Document* sharedDocument() const { return doc_; }
    // The program is about to close or replace `doc`: let go of it if it is the shared drawing.
    void drawingClosing(Document& doc);

private:
    struct Parked {
        collab::ConnId conn = 0;
        Frame hello{};
    };
    struct HostConn {
        std::shared_ptr<Link> link{};
        collab::ConnId id = 0;
        bool admitted = false;
        std::string computer{};   // id the guest sent before its Hello
        collab::PeerId peer = 0;  // known once the engine let the person in
    };

    void say(std::string line);
    bool remembered(const std::string& computer) const;
    Frame computerFrame() const;
    void rememberComputer(const std::string& computer, const std::string& name);
    void pumpHost(std::uint64_t ms);
    bool startHosting(std::unique_ptr<Listener> listener);
    void becomeHost();
    void afterPump();
    void replayOffline();
    bool replay(const collab::Op& op);
    void feedHello(HostConn& c, const Frame& hello, std::uint64_t ms);
    void broadcastTeam(const Frame& f, collab::ConnId except = 0);
    void teamFrameFromGuest(HostConn& c, const Frame& f, std::uint64_t ms);
    void pumpGuest();
    void teardown();
    void sendPresence(Vec2 at, std::uint64_t nowMs);
    void notePresence(const Frame& f, std::uint64_t nowMs);
    std::uint64_t nowMs() const;

    ui::AppCore& core_;
    std::function<void()> wake_;
    Mode mode_ = Mode::Off;
    Document* doc_ = nullptr;  // the shared drawing (the host's, or the one the guest opened)
    std::string hostName_{};
    bool guestsMayEdit_ = true;
    std::vector<std::string> log_{};
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
    bool askBeforeJoin_ = false;  // on for shareSimple (no code: the host is the gate)
    collab::Role admitAs_ = collab::Role::Editor;
    std::vector<Request> requests_{};
    std::vector<Parked> parked_{};
    std::vector<Cursor> cursors_{};
    Claims claims_{};
    std::vector<ChatLine> chat_{};
    std::uint64_t lastPresenceMs_ = 0;
    Vec2 lastShared_{};
    std::string password_{};
    // hand-over in progress
    collab::PeerId handingTo_ = 0;
    bool becomeHostAsked_ = false;
    std::optional<PortMessage> moveTo_{};
    std::uint64_t closeAfterJoin_ = 0;  // documentId of the old copy to close once joined
    std::uint64_t autoAdmitUntil_ = 0;
    std::string lastAddress_{};
    int lastPort_ = 0;
    std::vector<collab::Replica::PendingEdit>
        offlineOps_{};  // carried from the old connection to the new one
    std::vector<OfflineConflict> conflicts_{};
    bool stopRequested_ = false;
    std::string stopWhy_{};

    // host
    std::unique_ptr<collab::Host> host_{};
    std::unique_ptr<collab::HostSession> hostSession_{};
    std::unique_ptr<Listener> listener_{};
    std::unique_ptr<Announcer> announcer_{};
    std::unique_ptr<Finder> finder_{};
    std::vector<HostConn> conns_{};
    // guest
    std::unique_ptr<collab::GuestSession> guest_{};
    std::shared_ptr<Link> guestLink_{};
};

}  // namespace tbcad::collabapp

#include "collab_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>

#include "tbcad/cmd/undo.h"
#include "tbcad/collab/undo_guard.h"
#include "team.h"

namespace tbcad::collabapp {

namespace {
constexpr std::uint8_t kPresence = 0xF0;  // not an engine message: cursor of one person
constexpr std::uint8_t kHello = 1;        // the engine's first message (name follows the version)
constexpr std::size_t kPresenceSize = 1 + 4 + 8 + 8;

Frame presenceFrame(collab::PeerId id, Vec2 at) {
    Frame f(kPresenceSize);
    f[0] = kPresence;
    const std::uint32_t i = static_cast<std::uint32_t>(id);
    std::memcpy(&f[1], &i, 4);
    std::memcpy(&f[5], &at.x, 8);
    std::memcpy(&f[13], &at.y, 8);
    return f;
}
}  // namespace

Rgba CollabController::colorOf(collab::PeerId peer) {
    static const Rgba palette[] = {{230, 80, 80, 255},  {70, 160, 235, 255},  {90, 200, 110, 255},
                                   {235, 170, 50, 255}, {180, 100, 220, 255}, {60, 200, 200, 255}};
    return palette[peer % 6];
}

std::uint64_t CollabController::nowMs() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - t0_)
                                          .count());
}

void CollabController::notePresence(const Frame& f, std::uint64_t now) {
    if (f.size() != kPresenceSize)
        return;
    std::uint32_t id = 0;
    Vec2 at{};
    std::memcpy(&id, &f[1], 4);
    std::memcpy(&at.x, &f[5], 8);
    std::memcpy(&at.y, &f[13], 8);
    if (!std::isfinite(at.x) || !std::isfinite(at.y))
        return;
    for (Cursor& c : cursors_)
        if (c.peer == id) {
            c.at = at;
            c.seenMs = now;
            return;
        }
    if (cursors_.size() < 64)
        cursors_.push_back({id, at, now});
}

std::vector<CollabController::Cursor> CollabController::cursors() const {
    std::vector<Cursor> out;
    const std::uint64_t now = nowMs();
    for (const Cursor& c : cursors_)
        if (now - c.seenMs < 5000)
            out.push_back(c);
    return out;
}

void CollabController::sendPresence(Vec2 at, std::uint64_t now) {
    if (now - lastPresenceMs_ < 100 || (at.x == lastShared_.x && at.y == lastShared_.y))
        return;
    lastPresenceMs_ = now;
    lastShared_ = at;
    if (hostSession_) {
        const Frame f = presenceFrame(0, at);
        for (HostConn& c : conns_)
            c.link->send(f);
    } else if (guestLink_ && mode_ == Mode::Joined) {
        guestLink_->send(presenceFrame(guest_->self(), at));
    }
}

void CollabController::shareCursor(Vec2 at) {
    if (mode_ == Mode::Hosting || mode_ == Mode::Joined)
        sendPresence(at, nowMs());
}

CollabController::CollabController(ui::AppCore& core, std::function<void()> wake)
    : core_(core), wake_(std::move(wake)) {}

CollabController::~CollabController() {
    teardown();
}

void CollabController::say(std::string line) {
    log_.push_back(std::move(line));
    if (log_.size() > 300)
        log_.erase(log_.begin(), log_.begin() + 100);
}

std::string CollabController::status() const {
    switch (mode_) {
        case Mode::Off:
            return "Not sharing";
        case Mode::Hosting:
            return "Sharing on port " + std::to_string(port()) + " (" +
                   std::to_string(peers().size()) + " guest(s))";
        case Mode::Joining:
            return password_ == kLanPassword ? "Waiting for the host to let you in..."
                                             : "Joining...";
        case Mode::Joined:
            return "Connected as " + hostName_;
        case Mode::Offline:
            return "Offline: " + std::to_string(offlineEdits()) + " edit(s) wait to be sent";
    }
    return {};
}

std::vector<collab::PeerInfo> CollabController::peers() const {
    if (hostSession_)
        return hostSession_->peers();
    if (guest_)
        return guest_->peers();
    return {};
}

bool CollabController::share(int port, const std::string& password, const std::string& name,
                             bool guestsMayEdit, std::string& error) {
    if (mode_ != Mode::Off) {
        error = "Already in a session";
        return false;
    }
    if (password.size() < 4) {
        error = "Choose a password of at least 4 characters";
        return false;
    }
    std::unique_ptr<Listener> listener = Listener::start(port, password, wake_, error);
    if (!listener)
        return false;
    doc_ = &core_.document();
    host_ = std::make_unique<collab::Host>(*doc_);
    hostName_ = name.empty() ? "Host" : name;
    guestsMayEdit_ = guestsMayEdit;
    password_ = password;
    core_.undoStack().setGuard(collab::checkUndoRedo);
    return startHosting(std::move(listener));
}

bool CollabController::startHosting(std::unique_ptr<Listener> listener) {
    hostSession_ = std::make_unique<collab::HostSession>(*host_, hostName_);
    hostSession_->approve = [this](const std::string&, collab::Role& role) {
        role = admitAs_;  // decided by the host (answer()) or by the default
        return true;
    };
    hostSession_->onPeer = [this](const collab::PeerInfo& p, bool joined) {
        say(p.name + (joined ? " joined" : " left"));
        if (!joined && claims_.releaseAll(p.id))
            broadcastTeam(claims_.tableFrame());
    };
    host_->policy = [this](collab::PeerId from, const collab::Op& op, const Document& d) {
        return claims_.check(from, op, d, [this](collab::PeerId p) { return nameOf(p); });
    };
    hostSession_->onRefused = [this](const collab::PeerInfo& p, const collab::Rejection& r) {
        say("Refused an edit from " + p.name + ": " + r.message);
    };
    listener_ = std::move(listener);
    announcer_ = std::make_unique<Announcer>(hostName_, listener_->port());
    mode_ = Mode::Hosting;
    say("Sharing this drawing on port " + std::to_string(listener_->port()));
    return true;
}

bool CollabController::join(const std::string& host, int port, const std::string& password,
                            const std::string& name, std::string& error) {
    if (mode_ != Mode::Off) {
        error = "Already in a session";
        return false;
    }
    if (host.empty() || port <= 0 || port > 65535) {
        error = "Enter the address and port of the host";
        return false;
    }
    if (password.empty()) {
        error = "Enter the session password";
        return false;
    }
    hostName_ = name.empty() ? "Guest" : name;
    password_ = password;
    lastAddress_ = host;
    lastPort_ = port;
    guestLink_ = Link::dial(host, port, password, wake_);
    std::shared_ptr<Link> link = guestLink_;
    guest_ =
        std::make_unique<collab::GuestSession>([link](const collab::Frame& f) { link->send(f); });
    guest_->onJoined = [this] {
        std::unique_ptr<Document> doc = guest_->releaseDocument();
        doc_ = doc.get();
        core_.adoptDocument(std::move(doc));
        core_.undoStack().setGuard(collab::checkUndoRedo);
        mode_ = Mode::Joined;
        say("Joined: the shared drawing is open in a new tab");
        replayOffline();
        if (const std::uint64_t old = std::exchange(closeAfterJoin_, 0))
            for (std::size_t i = 0; i < core_.documentCount(); ++i)
                if (core_.documentId(i) == old) {
                    core_.closeDocument(i);  // the copy from before the hand-over
                    break;
                }
    };
    guest_->onConflict = [this](const collab::Conflict& c) {
        std::string line = c.message;
        if (c.by)
            line += " (" + c.by->name + " got there first)";
        say("Your edit was undone: " + line);
        core_.print("Shared drawing: your last edit was undone. " + line);
    };
    guest_->onPeer = [this](const collab::PeerInfo& p, bool joined) {
        say(p.name + (joined ? " joined" : " left"));
    };
    guest_->onClosed = [this](const std::string& why) {
        stopRequested_ = true;
        stopWhy_ = why;
    };
    mode_ = Mode::Joining;
    say("Connecting to " + host + ":" + std::to_string(port));
    link->send(computerFrame());
    guest_->join(hostName_);
    return true;
}

void CollabController::answer(collab::ConnId conn, bool allow, collab::Role role, bool remember) {
    for (const Request& r : requests_)
        if (r.conn == conn && allow && remember)
            rememberComputer(r.computer, r.name);
    std::erase_if(requests_, [&](const Request& r) { return r.conn == conn; });
    for (std::size_t i = 0; i < parked_.size(); ++i) {
        if (parked_[i].conn != conn)
            continue;
        Frame hello = std::move(parked_[i].hello);
        parked_.erase(parked_.begin() + static_cast<std::ptrdiff_t>(i));
        for (HostConn& c : conns_)
            if (c.id == conn) {
                if (allow && hostSession_) {
                    admitAs_ = role;
                    feedHello(c, hello, nowMs());
                } else {
                    c.link->close();
                }
            }
        return;
    }
}

std::vector<Announcement> CollabController::nearbyHosts() {
    if (mode_ != Mode::Off)
        return {};
    if (!finder_)
        finder_ = std::make_unique<Finder>();
    return finder_->hosts();
}

bool CollabController::kick(collab::PeerId peer) {
    return hostSession_ && hostSession_->kick(peer);
}

void CollabController::teardown() {
    // Engine objects first: they watch the drawing and talk through the links.
    hostSession_.reset();
    host_.reset();
    guest_.reset();
    for (HostConn& c : conns_)
        c.link->close();
    conns_.clear();
    if (guestLink_)
        guestLink_->close();
    guestLink_.reset();
    listener_.reset();
    announcer_.reset();
    cursors_.clear();
    claims_ = {};
    chat_.clear();
    doc_ = nullptr;
    mode_ = Mode::Off;
}

void CollabController::stop(const std::string& why) {
    if (mode_ == Mode::Off && !guest_)
        return;
    // Links flush what is queued (the goodbye) on close(), but the engine is gone by then.
    if (guest_ && guest_->state() == collab::GuestSession::State::Joined)
        guest_->leave();
    teardown();
    conflicts_.clear();
    offlineOps_.clear();
    stopRequested_ = false;
    say(why.empty() ? "Session ended" : "Session ended: " + why);
}

void CollabController::drawingClosing(Document& doc) {
    if (&doc == doc_)
        stop("the drawing was closed");
}

void CollabController::feedHello(HostConn& c, const Frame& hello, std::uint64_t ms) {
    c.admitted = true;
    const auto before = hostSession_->peers();
    hostSession_->receive(c.id, hello, ms);
    // The engine numbers the newcomer; remember which connection speaks for whom.
    for (const collab::PeerInfo& p : hostSession_->peers()) {
        bool known = false;
        for (const collab::PeerInfo& b : before)
            known = known || b.id == p.id;
        if (!known)
            c.peer = p.id;
    }
    if (c.peer != 0)
        c.link->send(claims_.tableFrame());
}

void CollabController::broadcastTeam(const Frame& f, collab::ConnId except) {
    for (HostConn& o : conns_)
        if (o.peer != 0 && o.id != except)
            o.link->send(f);
}

void CollabController::teamFrameFromGuest(HostConn& c, const Frame& f, std::uint64_t ms) {
    if (f[0] == kPresence) {
        if (f.size() != kPresenceSize)
            return;
        Frame stamped = f;
        const std::uint32_t id = c.peer;
        std::memcpy(&stamped[1], &id, 4);
        notePresence(stamped, ms);
        broadcastTeam(stamped, c.id);
    } else if (f[0] == kChat) {
        if (!readChat(f))
            return;
        const Frame stamped = restampChat(f, c.peer);
        chat_.push_back(*readChat(stamped));
        broadcastTeam(stamped, c.id);
    } else if (auto req = readClaim(f)) {
        if (!hostSession_->host().roleOf(c.peer) ||
            *hostSession_->host().roleOf(c.peer) != collab::Role::Editor)
            return;
        const bool changed =
            req->claim ? claims_.claim(c.peer, req->layer) : claims_.release(c.peer, req->layer);
        if (changed)
            broadcastTeam(claims_.tableFrame());
    }
}

std::string CollabController::nameOf(collab::PeerId peer) const {
    if (peer == 0)
        return mode_ == Mode::Hosting ? hostName_ : std::string("Host");
    if (guest_ && peer == guest_->self())
        return hostName_;
    for (const collab::PeerInfo& p : peers())
        if (p.id == peer)
            return p.name;
    return "someone";
}

void CollabController::sendChat(const std::string& text) {
    if (text.empty())
        return;
    if (hostSession_) {
        const Frame f = chatFrame(0, text);
        chat_.push_back(*readChat(f));
        broadcastTeam(f);
    } else if (guestLink_ && mode_ == Mode::Joined) {
        const Frame f = chatFrame(guest_->self(), text);
        chat_.push_back(*readChat(f));
        guestLink_->send(f);
    }
    if (chat_.size() > 300)
        chat_.erase(chat_.begin(), chat_.begin() + 100);
}

void CollabController::setClaim(LayerId layer, bool claim) {
    if (hostSession_) {
        if (claim ? claims_.claim(0, layer) : claims_.release(0, layer))
            broadcastTeam(claims_.tableFrame());
    } else if (guestLink_ && mode_ == Mode::Joined) {
        guestLink_->send(claimFrame(claim, layer));
    }
}

collab::PeerId CollabController::self() const {
    return guest_ ? guest_->self() : 0;
}

void CollabController::pumpHost(std::uint64_t ms) {
    for (std::shared_ptr<Link>& link : listener_->takeNew()) {
        HostConn c;
        c.link = link;
        c.id = hostSession_->accept([link](const collab::Frame& f) { link->send(f); },
                                    [link] { link->close(); });
        conns_.push_back(std::move(c));
    }
    for (std::size_t i = 0; i < conns_.size();) {
        HostConn& c = conns_[i];
        Frame f;
        while (c.link->poll(f)) {
            if (f.empty())
                continue;
            if (f[0] == kHostReady && c.peer != 0 && c.peer == handingTo_) {
                auto ready = readPort(f, kHostReady);
                handingTo_ = 0;
                if (!ready || ready->port == 0 || c.link->peerAddress().empty()) {
                    say("The hand-over did not work; you are still the host");
                } else {
                    moveTo_ = PortMessage{ready->port, c.link->peerAddress()};
                    broadcastTeam(portFrame(kMoveTo, ready->port, moveTo_->address), c.id);
                }
                continue;
            }
            if (f[0] == kPresence || f[0] == kChat || f[0] == kClaim) {
                if (c.peer != 0)
                    teamFrameFromGuest(c, f, ms);
                continue;
            }
            if (f[0] == kComputer) {
                if (!c.admitted && f.size() <= 1 + kMaxComputerId)
                    c.computer.assign(f.begin() + 1, f.end());
                continue;
            }
            if (askBeforeJoin_ && ms >= autoAdmitUntil_ && f[0] == kHello && !c.admitted &&
                !remembered(c.computer)) {
                // Keep the request until the host answers; nothing is sent to the guest meanwhile.
                std::string name;
                if (f.size() >= 3 && f[1] < 0x80 && f[2] < 0x80 && f.size() >= 3u + f[2])
                    name.assign(f.begin() + 3, f.begin() + 3 + f[2]);
                if (name.empty() || requests_.size() >= 8) {
                    c.link->close();
                    continue;
                }
                requests_.push_back({c.id, name, c.computer});
                parked_.push_back({c.id, f});
                say(name + " asks to join");
                core_.print(name + " asks to join your shared drawing (File > Work Together).");
                core_.openPanel("collab");
                continue;
            }
            if (f[0] == kHello && !c.admitted) {
                admitAs_ = guestsMayEdit_ ? collab::Role::Editor : collab::Role::Viewer;
                feedHello(c, f, ms);
                continue;
            }
            hostSession_->receive(c.id, f, ms);
        }
        if (c.link->finished() && !c.link->poll(f)) {
            std::erase_if(requests_, [&](const Request& r) { return r.conn == c.id; });
            std::erase_if(parked_, [&](const Parked& r) { return r.conn == c.id; });
            hostSession_->disconnect(c.id);
            conns_.erase(conns_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void CollabController::pumpGuest() {
    Frame f;
    while (guestLink_ && guestLink_->poll(f) && !stopRequested_) {
        if (!f.empty() && f[0] == kPresence) {
            notePresence(f, nowMs());
        } else if (!f.empty() && f[0] == kChat) {
            if (auto line = readChat(f))
                chat_.push_back(*line);
        } else if (!f.empty() && f[0] == kClaimTable) {
            (void)claims_.readTable(f);
        } else if (!f.empty() && f[0] == kBecomeHost) {
            becomeHostAsked_ = true;
            break;
        } else if (!f.empty() && f[0] == kMoveTo) {
            if (auto m = readPort(f, kMoveTo); m && m->port > 0 && !m->address.empty()) {
                moveTo_ = m;
                break;
            }
        } else {
            guest_->receive(f);
        }
    }
    if (!stopRequested_ && guestLink_ && guestLink_->finished() &&
        guest_->state() != collab::GuestSession::State::Closed) {
        const std::string why = guestLink_->error();
        const bool wasJoined = mode_ == Mode::Joined;
        guest_->disconnected();
        if (wasJoined) {
            // Keep the replica: it goes on collecting the edits made from now on.
            stopRequested_ = false;
            mode_ = Mode::Offline;
            guestLink_.reset();
            say("Connection lost (" + (why.empty() ? std::string("no reason given") : why) +
                "). Keep working; press Reconnect when the network is back.");
        } else if (!stopRequested_) {
            stopRequested_ = true;
            stopWhy_ = why.empty() ? "connection lost" : why;
        }
    }
}

void CollabController::pump() {
    if (mode_ == Mode::Off)
        return;
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - t0_).count());
    if (hostSession_)
        pumpHost(ms);
    else if (guest_ && mode_ != Mode::Offline)
        pumpGuest();
    afterPump();
}

std::size_t CollabController::offlineEdits() const {
    if (mode_ == Mode::Offline && guest_ && guest_->replica())
        return guest_->replica()->pendingCount();
    return 0;
}

bool CollabController::reconnect(std::string& error) {
    if (mode_ != Mode::Offline || !guest_ || !guest_->replica()) {
        error = "Not offline";
        return false;
    }
    offlineOps_ = guest_->replica()->pendingEdits();
    std::uint64_t oldCopy = 0;
    for (std::size_t i = 0; i < core_.documentCount(); ++i)
        if (core_.activeDocument() == i && doc_ == &core_.document())
            oldCopy = core_.documentId(i);
    const std::string address = lastAddress_, password = password_, name = hostName_;
    const int port = lastPort_;
    teardown();
    if (!join(address, port, password, name, error)) {
        offlineOps_.clear();
        return false;
    }
    closeAfterJoin_ = oldCopy;
    return true;
}

// Puts an edit made on the old copy into the drawing just received. Entities only: a new object
// gets a new id from this connection's block; table rows changed offline are not carried over.
bool CollabController::replay(const collab::Op& op) {
    if (!doc_)
        return false;
    Transaction tx = doc_->begin("offline edit");
    for (const io::EntityDelta& e : op.delta.entities) {
        Result<void> r = Result<void>{};
        if (!e.after) {
            if (doc_->contains(e.id))
                r = tx.erase(e.id);
        } else if (doc_->contains(e.id)) {
            Entity after = *e.after;
            r = tx.modify(e.id, [&](Entity& now) { now = after; });
        } else {
            Result<EntityId> added = tx.add(*e.after);
            if (!added)
                r = Result<void>(added.error());
        }
        if (!r) {
            say("An offline edit could not be put back: " + r.error().message);
            return false;
        }
    }
    tx.commit();
    return true;
}

void CollabController::replayOffline() {
    if (offlineOps_.empty() || !doc_)
        return;
    std::size_t sent = 0;
    for (collab::Replica::PendingEdit& e : std::exchange(offlineOps_, {})) {
        // Still as it was before my edit? Otherwise someone else changed it meanwhile.
        const std::vector<ChangeSet> sets{e.applied};
        if (!collab::checkUndoRedo(*doc_, sets, Direction::Redo).empty()) {
            conflicts_.push_back({std::to_string(e.op.delta.entities.size()) +
                                      " object(s) that someone else changed meanwhile",
                                  std::move(e.op), std::move(e.applied)});
        } else if (replay(e.op)) {
            ++sent;
        }
    }
    say(std::to_string(sent) + " offline edit(s) sent" +
        (conflicts_.empty() ? std::string()
                            : ", " + std::to_string(conflicts_.size()) + " need your choice"));
}

void CollabController::keepMine(std::size_t index) {
    if (index >= conflicts_.size())
        return;
    collab::Op op = std::move(conflicts_[index].op);
    conflicts_.erase(conflicts_.begin() + static_cast<std::ptrdiff_t>(index));
    (void)replay(op);
}

void CollabController::keepTheirs(std::size_t index) {
    if (index < conflicts_.size())
        conflicts_.erase(conflicts_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::string CollabController::defaultName() {
    for (const char* var : {"USERNAME", "USER"})
        if (const char* v = std::getenv(var); v && *v)
            return v;
    return "Me";
}

bool CollabController::shareSimple(std::string& error) {
    bool ok = share(47800, kLanPassword, defaultName(), true, error);
    if (!ok && mode_ == Mode::Off) {
        error.clear();
        ok = share(0, kLanPassword, defaultName(), true, error);
    }
    if (ok)
        askBeforeJoin_ = true;
    return ok;
}

bool CollabController::joinSimple(const std::string& host, int port, const std::string& name,
                                  std::string& error) {
    return join(host, port, kLanPassword, name.empty() ? defaultName() : name, error);
}

std::string CollabController::computerId() const {
    std::string id = core_.settings().getString(kSettingComputerId);
    if (id.size() >= 16 && id.size() <= kMaxComputerId)
        return id;
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    id.clear();
    for (int i = 0; i < 32; ++i)
        id += hex[rd() % 16];
    core_.settings().setString(kSettingComputerId, id);
    return id;
}

Frame CollabController::computerFrame() const {
    const std::string id = computerId();
    Frame f{kComputer};
    f.insert(f.end(), id.begin(), id.end());
    return f;
}

std::vector<CollabController::Computer> CollabController::rememberedComputers() const {
    std::vector<Computer> out;
    for (const std::string& line : core_.settings().getStrings(kSettingRemembered)) {
        const std::size_t tab = line.find('\t');
        if (tab == 0 || tab == std::string::npos)
            continue;
        out.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
    return out;
}

bool CollabController::remembered(const std::string& computer) const {
    if (computer.empty())
        return false;
    for (const Computer& c : rememberedComputers())
        if (c.id == computer)
            return true;
    return false;
}

void CollabController::rememberComputer(const std::string& computer, const std::string& name) {
    if (computer.empty() || computer.find('\t') != std::string::npos)
        return;
    std::vector<std::string> lines;
    for (const Computer& c : rememberedComputers())
        if (c.id != computer)
            lines.push_back(c.id + "\t" + c.name);
    lines.push_back(computer + "\t" + name);
    while (lines.size() > 100)
        lines.erase(lines.begin());
    core_.settings().setStrings(kSettingRemembered, lines);
}

void CollabController::forgetComputer(const std::string& id) {
    std::vector<std::string> lines;
    for (const Computer& c : rememberedComputers())
        if (c.id != id)
            lines.push_back(c.id + "\t" + c.name);
    core_.settings().setStrings(kSettingRemembered, lines);
}

std::string CollabController::localAddress() const {
    return localLanAddress();
}

bool CollabController::handOver(collab::PeerId to, std::string& error) {
    if (mode_ != Mode::Hosting || !hostSession_) {
        error = "Only the host can hand over";
        return false;
    }
    for (HostConn& c : conns_)
        if (c.peer == to) {
            handingTo_ = to;
            c.link->send(Frame{kBecomeHost});
            say("Asking " + nameOf(to) + " to take over as host");
            return true;
        }
    error = "That person is not connected";
    return false;
}

void CollabController::becomeHost() {
    collab::Replica* rep = guest_ ? guest_->replica() : nullptr;
    std::string err;
    std::unique_ptr<Listener> listener;
    if (rep && doc_ && rep->pendingCount() == 0)
        listener = Listener::start(0, password_, wake_, err);
    if (!listener) {
        if (guestLink_)
            guestLink_->send(portFrame(kHostReady, 0));
        say("Could not take over as host" + (err.empty() ? std::string() : ": " + err));
        return;
    }
    collab::VersionMap versions = rep->versions();
    const std::uint64_t seq = rep->sequence();
    guestLink_->send(portFrame(kHostReady, listener->port()));
    guestLink_->close();
    guestLink_.reset();
    guest_.reset();  // the replica lets go of the drawing; the tab keeps it
    doc_->clearEntityIdWindow();
    host_ = std::make_unique<collab::Host>(*doc_);
    host_->restore(std::move(versions), seq);
    autoAdmitUntil_ = nowMs() + 60000;           // the others are on their way: let them in
    askBeforeJoin_ = password_ == kLanPassword;  // no code: the new host decides who joins
    (void)startHosting(std::move(listener));
    say("You are the host now");
}

void CollabController::afterPump() {
    if (becomeHostAsked_) {
        becomeHostAsked_ = false;
        becomeHost();
    }
    if (moveTo_) {
        const PortMessage to = *std::exchange(moveTo_, std::nullopt);
        const bool wasGuest = mode_ != Mode::Hosting;
        const std::string name = hostName_;
        const std::string password = password_;
        std::uint64_t oldCopy = 0;
        if (wasGuest)
            for (std::size_t i = 0; i < core_.documentCount(); ++i)
                if (core_.activeDocument() == i && doc_ == &core_.document())
                    oldCopy = core_.documentId(i);
        stop("the host role moved to " + to.address);
        std::string err;
        if (!join(to.address, to.port, password, name, err))
            say("Could not follow the new host: " + err);
        else
            closeAfterJoin_ = oldCopy;
    }
    if (stopRequested_)
        stop(stopWhy_);
}

}  // namespace tbcad::collabapp

#include <random>

#include "helpers.h"
#include "tbcad/collab/session.h"
#include "tbcad/collab/undo_guard.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collab::testing;

namespace {

// A host and its guests in one process, joined by queues. Frames wait in the queues until the
// test delivers them, which is how the tests make people edit "at the same time".
struct Net {
    struct Remote {
        std::unique_ptr<GuestSession> guest{};
        std::deque<Frame> toHost{};
        std::deque<Frame> toGuest{};
        ConnId conn = 0;
        bool closedByHost = false;
        std::vector<Conflict> conflicts{};
        std::string closedWhy{};
        std::vector<std::pair<PeerInfo, bool>> peerEvents{};
    };

    Document hostDoc;
    Host host;
    HostSession hs;
    std::vector<std::unique_ptr<Remote>> remotes;
    std::uint64_t now = 0;
    std::vector<std::pair<PeerInfo, RejectReason>> refused{};

    explicit Net(HostConfig cfg = {}) : host(hostDoc, cfg), hs(host, "test session") {
        hs.onRefused = [this](const PeerInfo& p, const Rejection& r) {
            refused.emplace_back(p, r.reason);
        };
    }

    Remote& add(const std::string& name, bool join = true) {
        auto r = std::make_unique<Remote>();
        Remote* raw = r.get();
        r->conn = hs.accept([raw](const Frame& f) { raw->toGuest.push_back(f); },
                            [raw] { raw->closedByHost = true; });
        r->guest =
            std::make_unique<GuestSession>([raw](const Frame& f) { raw->toHost.push_back(f); });
        r->guest->onConflict = [raw](const Conflict& c) { raw->conflicts.push_back(c); };
        r->guest->onClosed = [raw](const std::string& why) { raw->closedWhy = why; };
        r->guest->onPeer = [raw](const PeerInfo& p, bool in) {
            raw->peerEvents.emplace_back(p, in);
        };
        remotes.push_back(std::move(r));
        if (join) {
            raw->guest->join(name);
            settle();
        }
        return *raw;
    }

    bool deliverToHost(Remote& r) {
        if (r.toHost.empty())
            return false;
        Frame f = std::move(r.toHost.front());
        r.toHost.pop_front();
        hs.receive(r.conn, f, now += 10);
        return true;
    }
    bool deliverToGuest(Remote& r) {
        if (r.toGuest.empty())
            return false;
        Frame f = std::move(r.toGuest.front());
        r.toGuest.pop_front();
        r.guest->receive(f);
        return true;
    }
    void settle() {
        for (int round = 0; round < 10000; ++round) {
            bool any = false;
            for (auto& r : remotes) {
                any = deliverToHost(*r) || any;
                any = deliverToGuest(*r) || any;
            }
            if (!any)
                return;
        }
        FAIL("session did not settle");
    }
};

Document& docOf(Net::Remote& r) {
    return *r.guest->document();
}

EntityId drawLine(Document& d, Vec2 a, Vec2 b) {
    Transaction tx = d.begin("line");
    EntityId id = must(tx.add(line(kLayerZero, a, b)));
    tx.commit();
    return id;
}

void moveEnd(Document& d, EntityId id, Vec2 to) {
    Transaction tx = d.begin("move");
    (void)tx.modify(id, [&](Entity& e) {
        if (auto* l = std::get_if<LineData>(&e.data))
            l->b = to;
    });
    tx.commit();
}

}  // namespace

TEST_CASE("session: a guest joins and receives the drawing, the peers and its own ids") {
    Net net;
    drawLine(net.hostDoc, {0, 0}, {5, 5});
    drawLine(net.hostDoc, {1, 0}, {5, 6});
    std::vector<std::pair<PeerInfo, bool>> hostEvents;
    net.hs.onPeer = [&](const PeerInfo& p, bool in) { hostEvents.emplace_back(p, in); };

    auto& a = net.add("Somchai");
    REQUIRE(a.guest->state() == GuestSession::State::Joined);
    CHECK(canonicalDump(docOf(a)) == canonicalDump(net.hostDoc));
    CHECK(a.guest->self() != kHostPeer);
    CHECK(a.guest->role() == Role::Editor);
    CHECK(a.guest->replica()->idsLeft() > 0);

    auto& b = net.add("Nok");
    REQUIRE(b.guest->state() == GuestSession::State::Joined);
    REQUIRE(b.guest->peers().size() == 1);  // Somchai was already there
    CHECK(b.guest->peers()[0].name == "Somchai");
    REQUIRE(a.peerEvents.size() == 1);  // and Somchai was told about Nok
    CHECK(a.peerEvents[0].first.name == "Nok");
    CHECK(a.peerEvents[0].second);
    CHECK(hostEvents.size() == 2);
    CHECK(net.hs.peers().size() == 2);
}

TEST_CASE("session: edits travel both ways and everyone ends with the same drawing") {
    Net net;
    auto& a = net.add("A");
    auto& b = net.add("B");
    drawLine(docOf(a), {0, 0}, {1, 1});
    drawLine(docOf(b), {2, 2}, {3, 3});
    drawLine(net.hostDoc, {4, 4}, {5, 5});
    net.settle();
    CHECK(net.hostDoc.modelSpaceCount() == 3);
    CHECK(canonicalDump(docOf(a)) == canonicalDump(net.hostDoc));
    CHECK(canonicalDump(docOf(b)) == canonicalDump(net.hostDoc));
    CHECK(a.conflicts.empty());
    CHECK(b.conflicts.empty());
    CHECK(a.guest->replica()->pendingCount() == 0);
}

TEST_CASE("session: random edits by three guests and the host, frames delivered in random order") {
    for (unsigned seed = 1; seed <= 20; ++seed) {
        CAPTURE(seed);
        std::mt19937_64 rng(seed);
        Net net;
        drawLine(net.hostDoc, {0, 0}, {10, 10});
        auto& a = net.add("A");
        auto& b = net.add("B");
        auto& c = net.add("C");
        std::vector<Net::Remote*> all{&a, &b, &c};
        for (int step = 0; step < 120; ++step) {
            switch (rng() % 6) {
                case 0:
                case 1:
                case 2: {
                    Net::Remote& who = *all[rng() % all.size()];
                    Document& d = docOf(who);
                    auto ids = d.modelSpace();
                    if (ids.empty() || rng() % 3 == 0) {
                        drawLine(d, {double(rng() % 50), double(rng() % 50)},
                                 {double(rng() % 50), double(rng() % 50)});
                    } else if (rng() % 4 == 0) {
                        Transaction tx = d.begin("erase");
                        (void)tx.erase(ids[rng() % ids.size()]);
                        tx.commit();
                    } else {
                        moveEnd(d, ids[rng() % ids.size()],
                                {double(rng() % 50), double(rng() % 50)});
                    }
                    break;
                }
                case 3: {
                    auto ids = net.hostDoc.modelSpace();
                    if (!ids.empty())
                        moveEnd(net.hostDoc, ids[rng() % ids.size()],
                                {double(rng() % 50), double(rng() % 50)});
                    break;
                }
                default: {
                    Net::Remote& who = *all[rng() % all.size()];
                    if (rng() % 2)
                        net.deliverToHost(who);
                    else
                        net.deliverToGuest(who);
                    break;
                }
            }
        }
        net.settle();
        for (auto* r : all) {
            CHECK(r->guest->state() == GuestSession::State::Joined);
            CHECK(r->guest->replica()->pendingCount() == 0);
            CHECK(canonicalDump(docOf(*r)) == canonicalDump(net.hostDoc));
            CHECK(r->guest->replica()->versions() == net.host.versions());
        }
    }
}

TEST_CASE("session: a guest that uses up its ids asks for another block") {
    HostConfig cfg;
    cfg.idBlockSize = 8;
    Net net(cfg);
    auto& a = net.add("A");
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 3; ++i)
            drawLine(docOf(a), {double(i), double(round)}, {double(i), double(round) + 1});
        net.settle();
    }
    CHECK(net.hostDoc.modelSpaceCount() == 30);
    CHECK(a.conflicts.empty());
    CHECK(net.host.ranges().rangesOf(a.guest->self()).size() > 1);
    CHECK(canonicalDump(docOf(a)) == canonicalDump(net.hostDoc));
}

TEST_CASE("session: two people edit one line - the second is told who got there first") {
    Net net;
    EntityId id = drawLine(net.hostDoc, {0, 0}, {10, 0});
    auto& a = net.add("Somchai");
    auto& b = net.add("Nok");
    moveEnd(docOf(a), id, {10, 5});
    moveEnd(docOf(b), id, {10, -5});
    net.deliverToHost(a);  // Somchai's edit reaches the host first
    net.deliverToHost(b);
    net.settle();
    REQUIRE(b.conflicts.size() == 1);
    const Conflict& c = b.conflicts[0];
    CHECK(c.reason == RejectReason::Stale);
    REQUIRE(c.key.has_value());
    CHECK(c.key->space == Space::Entity);
    REQUIRE(c.by.has_value());
    CHECK(c.by->name == "Somchai");
    CHECK(a.conflicts.empty());
    CHECK(canonicalDump(docOf(a)) == canonicalDump(net.hostDoc));
    CHECK(canonicalDump(docOf(b)) == canonicalDump(net.hostDoc));
    REQUIRE(net.refused.size() == 1);
    CHECK(net.refused[0].first.name == "Nok");
}

TEST_CASE("session: undo of an edit someone else has changed since is refused, not applied") {
    Net net;
    auto& a = net.add("A");
    auto& b = net.add("B");
    Transaction tx = docOf(a).begin("line");
    EntityId id = must(tx.add(line(kLayerZero, {0, 0}, {10, 0})));
    ChangeSet add = tx.commit();
    net.settle();
    std::vector<ChangeSet> entry{add};
    CHECK(checkUndoRedo(docOf(a), entry, Direction::Undo).empty());
    moveEnd(docOf(b), id, {10, 9});
    net.settle();
    std::string why = checkUndoRedo(docOf(a), entry, Direction::Undo);
    CHECK(why.find("Cannot undo") == 0);
    // The refused step never touches the drawing, so nothing is sent and everything agrees.
    net.settle();
    CHECK(a.conflicts.empty());
    CHECK(canonicalDump(docOf(a)) == canonicalDump(net.hostDoc));
    CHECK(net.hostDoc.contains(id));
}

TEST_CASE("session: undoing your own untouched edit goes through for everybody") {
    Net net;
    auto& a = net.add("A");
    auto& b = net.add("B");
    Transaction tx = docOf(a).begin("line");
    EntityId id = must(tx.add(line(kLayerZero, {0, 0}, {10, 0})));
    ChangeSet add = tx.commit();
    net.settle();
    CHECK(net.hostDoc.contains(id));
    std::vector<ChangeSet> entry{add};
    REQUIRE(checkUndoRedo(docOf(a), entry, Direction::Undo).empty());
    docOf(a).apply(add, Direction::Undo);
    net.settle();
    CHECK_FALSE(net.hostDoc.contains(id));
    CHECK_FALSE(docOf(b).contains(id));
    CHECK(a.conflicts.empty());
    CHECK(canonicalDump(docOf(b)) == canonicalDump(net.hostDoc));
}

TEST_CASE("session: kicking a guest ends its session and tells the others") {
    Net net;
    auto& a = net.add("A");
    auto& b = net.add("B");
    REQUIRE(net.hs.kick(a.guest->self(), "bye"));
    net.settle();
    CHECK(a.guest->state() == GuestSession::State::Closed);
    CHECK(a.closedWhy == "bye");
    CHECK(a.closedByHost);
    CHECK(net.hs.peers().size() == 1);
    REQUIRE(!b.peerEvents.empty());
    CHECK(b.peerEvents.back().first.name == "A");
    CHECK_FALSE(b.peerEvents.back().second);
    // What the kicked guest still sends is ignored.
    drawLine(net.hostDoc, {0, 0}, {1, 1});
    net.settle();
    CHECK(canonicalDump(docOf(b)) == canonicalDump(net.hostDoc));
    CHECK_FALSE(net.hs.kick(a.guest->self()));
}

TEST_CASE("session: a viewer's edits are refused and rolled back") {
    Net net;
    net.hs.approve = [](const std::string& name, Role& role) {
        if (name == "Visitor")
            role = Role::Viewer;
        return true;
    };
    auto& v = net.add("Visitor");
    auto& e = net.add("Editor");
    CHECK(v.guest->role() == Role::Viewer);
    drawLine(docOf(v), {0, 0}, {1, 1});
    net.settle();
    REQUIRE(v.conflicts.size() == 1);
    CHECK(v.conflicts[0].reason == RejectReason::NotAllowed);
    CHECK(net.hostDoc.modelSpaceCount() == 0);
    CHECK(docOf(v).modelSpaceCount() == 0);
    drawLine(docOf(e), {0, 0}, {2, 2});
    net.settle();
    CHECK(docOf(v).modelSpaceCount() == 1);

    // The host can promote the viewer.
    REQUIRE(net.hs.setRole(v.guest->self(), Role::Editor));
    net.settle();
    CHECK(v.guest->role() == Role::Editor);
    drawLine(docOf(v), {5, 5}, {6, 6});
    net.settle();
    CHECK(net.hostDoc.modelSpaceCount() == 2);
}

TEST_CASE("session: the host can refuse a join") {
    Net net;
    net.hs.approve = [](const std::string&, Role&) { return false; };
    auto& a = net.add("Stranger");
    CHECK(a.guest->state() == GuestSession::State::Closed);
    CHECK(a.closedByHost);
    CHECK(net.hs.peers().empty());
}

TEST_CASE("session: damaged frames close the connection and never crash") {
    Net net;
    auto& a = net.add("A");
    // Garbage to the host.
    net.hs.receive(a.conn, Frame{}, 1);
    CHECK(a.closedByHost);
    CHECK(net.hs.peers().empty());

    // Garbage to a guest.
    auto& b = net.add("B");
    b.guest->receive(Frame{4, 1, 2, 3});  // an Accepted that does not decode
    CHECK(b.guest->state() == GuestSession::State::Closed);

    auto& c = net.add("C");
    c.guest->receive(Frame{99});
    CHECK(c.guest->state() == GuestSession::State::Closed);

    // Random bytes at the host from a connection that has not said hello, and from one that has.
    std::mt19937_64 rng(5);
    for (int round = 0; round < 200; ++round) {
        auto& d = net.add("D", round % 2 == 0);
        for (int k = 0; k < 3; ++k) {
            Frame f(1 + rng() % 40);
            for (auto& byte : f)
                byte = static_cast<std::uint8_t>(rng());
            net.hs.receive(d.conn, f, 1000 + round);
        }
    }
    CHECK(true);  // reaching here without a crash is the point
}

TEST_CASE("session: a guest sees the end when the connection drops") {
    Net net;
    auto& a = net.add("A");
    a.guest->disconnected();
    CHECK(a.guest->state() == GuestSession::State::Closed);
    CHECK(!a.closedWhy.empty());
    net.hs.disconnect(a.conn);
    CHECK(net.hs.peers().empty());
}

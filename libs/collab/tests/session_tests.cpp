#include <random>

#include "helpers.h"
#include "tbcad/collab/guest_session.h"
#include "tbcad/collab/host_session.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collab::testing;

namespace {

using Bytes = std::vector<std::uint8_t>;

// A host and any number of guests connected by queues. Frames are delivered when the test says
// so; deliver() runs everything until the session is quiet.
struct Net {
    Document hostDoc;
    Host host;
    HostSession hs;
    std::uint64_t now = 0;

    struct Guest {
        std::unique_ptr<GuestSession> g;
        ConnectionId conn = 0;
        std::deque<Bytes> toHost{};
        std::deque<Bytes> fromHost{};
        std::vector<Conflict> conflicts{};
        std::vector<PeerInfo> arrived{}, left{};
        bool closed = false;
    };
    std::vector<std::unique_ptr<Guest>> guests;

    explicit Net(HostSessionConfig cfg = {}) : host(hostDoc), hs(host, std::move(cfg)) {}

    Guest& connect(const std::string& name) {
        auto g = std::make_unique<Guest>();
        Guest* raw = g.get();
        raw->g = std::make_unique<GuestSession>(
            name, [raw](const Bytes& b) { raw->toHost.push_back(b); });
        raw->conn = hs.open([raw](const Bytes& b) { raw->fromHost.push_back(b); });
        raw->g->onConflict = [raw](const Conflict& c) { raw->conflicts.push_back(c); };
        raw->g->onPeerJoined = [raw](const PeerInfo& p) { raw->arrived.push_back(p); };
        raw->g->onPeerLeft = [raw](const PeerInfo& p) { raw->left.push_back(p); };
        raw->g->start();
        guests.push_back(std::move(g));
        return *raw;
    }

    bool step() {
        bool any = false;
        for (auto& g : guests) {
            while (!g->toHost.empty()) {
                Bytes b = std::move(g->toHost.front());
                g->toHost.pop_front();
                hs.receive(g->conn, b, now += 10);
                any = true;
            }
        }
        for (auto& g : guests) {
            while (!g->fromHost.empty()) {
                Bytes b = std::move(g->fromHost.front());
                g->fromHost.pop_front();
                g->g->receive(b);
                any = true;
            }
        }
        return any;
    }
    void deliver() {
        for (int i = 0; i < 2000; ++i)
            if (!step())
                return;
        FAIL("session did not settle");
    }
    // Delivers only what the guest has sent to the host, and the answers to it nowhere.
    void hostReceives(Guest& g) {
        while (!g.toHost.empty()) {
            Bytes b = std::move(g.toHost.front());
            g.toHost.pop_front();
            hs.receive(g.conn, b, now += 10);
        }
    }
    void guestReceives(Guest& g) {
        while (!g.fromHost.empty()) {
            Bytes b = std::move(g.fromHost.front());
            g.fromHost.pop_front();
            g.g->receive(b);
        }
    }
};

EntityId addLine(Document& doc, Vec2 a, Vec2 b) {
    Transaction tx = doc.begin("line");
    EntityId id = must(tx.add(line(kLayerZero, a, b)));
    tx.commit();
    return id;
}

void moveLine(Document& doc, EntityId id, Vec2 b) {
    Transaction tx = doc.begin("move");
    (void)tx.modify(id, [&](Entity& e) {
        if (auto* l = std::get_if<LineData>(&e.data))
            l->b = b;
    });
    tx.commit();
}

}  // namespace

TEST_CASE("session: a guest joins, gets the drawing, and hears who is there") {
    Net n({.hostName = "Nok"});
    addLine(n.hostDoc, {0, 0}, {10, 0});
    auto& a = n.connect("Somchai");
    n.deliver();
    REQUIRE(a.g->state() == GuestState::Joined);
    CHECK(a.g->hostName() == "Nok");
    CHECK(a.g->self() != kHostPeer);
    CHECK(a.g->document()->modelSpaceCount() == 1);
    REQUIRE(a.g->peers().size() == 1);  // the host
    CHECK(a.g->peers()[0].name == "Nok");

    auto& b = n.connect("Malee");
    n.deliver();
    REQUIRE(b.g->state() == GuestState::Joined);
    CHECK(b.g->peers().size() == 2);  // host and Somchai
    REQUIRE(a.arrived.size() == 1);
    CHECK(a.arrived[0].name == "Malee");
    CHECK(n.hs.peers().size() == 2);

    n.hs.close(b.conn);
    n.deliver();
    REQUIRE(a.left.size() == 1);
    CHECK(a.left[0].name == "Malee");
}

TEST_CASE("session: the host can refuse a guest, and a wrong protocol version is refused") {
    Net n({.approve = [](const std::string& name) { return name != "Intruder"; }});
    auto& bad = n.connect("Intruder");
    n.deliver();
    CHECK(bad.g->state() == GuestState::Denied);
    CHECK(n.hs.peers().empty());

    // A second hello, or an edit before hello, ends the connection.
    auto& rude = n.connect("Rude");
    n.deliver();
    REQUIRE(rude.g->state() == GuestState::Joined);
    rude.toHost.push_back(encodeMessage(msg::Hello{kProtocolVersion, "again"}));
    n.deliver();
    CHECK(n.hs.peers().empty());

    Net m;
    auto& old = m.connect("Old");
    old.toHost.clear();
    old.toHost.push_back(encodeMessage(msg::Hello{kProtocolVersion + 1, "Old"}));
    m.deliver();
    CHECK(old.g->state() == GuestState::Denied);
}

TEST_CASE("session: edits travel to everybody and the drawings stay identical") {
    Net n;
    auto& a = n.connect("A");
    auto& b = n.connect("B");
    n.deliver();
    addLine(*a.g->document(), {0, 0}, {1, 1});
    addLine(*b.g->document(), {2, 2}, {3, 3});
    addLine(n.hostDoc, {4, 4}, {5, 5});
    n.deliver();
    CHECK(n.hostDoc.modelSpaceCount() == 3);
    CHECK(canonicalDump(*a.g->document()) == canonicalDump(n.hostDoc));
    CHECK(canonicalDump(*b.g->document()) == canonicalDump(n.hostDoc));
    CHECK(a.conflicts.empty());
    CHECK(b.conflicts.empty());
}

TEST_CASE("session: the loser of a conflict is told who won, and nothing is half-applied") {
    Net n({.hostName = "Nok"});
    EntityId id = addLine(n.hostDoc, {0, 0}, {10, 0});
    auto& a = n.connect("Somchai");
    auto& b = n.connect("Malee");
    n.deliver();
    moveLine(*a.g->document(), id, {1, 1});
    moveLine(*b.g->document(), id, {2, 2});
    n.hostReceives(a);  // Somchai's edit gets there first
    n.hostReceives(b);
    n.deliver();
    REQUIRE(b.conflicts.size() == 1);
    CHECK(b.conflicts[0].reason == RejectReason::Stale);
    CHECK(b.conflicts[0].byName == "Somchai");
    CHECK(b.conflicts[0].message.find("Somchai") != std::string::npos);
    CHECK(a.conflicts.empty());
    CHECK(canonicalDump(*b.g->document()) == canonicalDump(n.hostDoc));
    CHECK(canonicalDump(*a.g->document()) == canonicalDump(n.hostDoc));
}

TEST_CASE("session: a viewer can look but not edit") {
    Net n({.defaultRole = Role::Viewer});
    EntityId id = addLine(n.hostDoc, {0, 0}, {10, 0});
    auto& v = n.connect("Visitor");
    n.deliver();
    CHECK(v.g->role() == Role::Viewer);
    moveLine(*v.g->document(), id, {5, 5});
    n.deliver();
    REQUIRE(v.conflicts.size() == 1);
    CHECK(v.conflicts[0].reason == RejectReason::NotAllowed);
    CHECK(canonicalDump(*v.g->document()) == canonicalDump(n.hostDoc));

    // Promoted by the host, the same guest can edit.
    REQUIRE(n.hs.setRole(v.g->self(), Role::Editor));
    n.deliver();
    CHECK(v.g->role() == Role::Editor);
    moveLine(*v.g->document(), id, {5, 5});
    n.deliver();
    CHECK(v.conflicts.size() == 1);
    CHECK(canonicalDump(*v.g->document()) == canonicalDump(n.hostDoc));
}

TEST_CASE("session: kicking a guest ends their session and tells the others") {
    Net n;
    auto& a = n.connect("A");
    auto& b = n.connect("B");
    n.deliver();
    REQUIRE(n.hs.kick(b.g->self(), "bye"));
    n.deliver();
    CHECK(b.g->state() == GuestState::Ended);
    CHECK(b.g->endReason() == "bye");
    REQUIRE(a.left.size() == 1);
    CHECK(a.left[0].name == "B");
    // What the removed guest still draws goes nowhere.
    addLine(*b.g->document(), {0, 0}, {1, 1});
    n.deliver();
    CHECK(n.hostDoc.modelSpaceCount() == 0);
}

TEST_CASE("session: a guest that draws a lot gets more ids") {
    Net n({});
    auto& a = n.connect("A");
    n.deliver();
    // Small blocks make the point quickly.
    Net m;
    (void)m;
    for (int round = 0; round < 3; ++round) {
        Transaction tx = a.g->document()->begin("many");
        for (int i = 0; i < 40; ++i)
            (void)must(tx.add(line(kLayerZero, {double(i), 0}, {double(i), 1})));
        tx.commit();
        n.deliver();
    }
    CHECK(n.hostDoc.modelSpaceCount() == 120);
    CHECK(a.conflicts.empty());
}

TEST_CASE("session: undo takes back only the user's own work") {
    Net n;
    auto& a = n.connect("A");
    auto& b = n.connect("B");
    n.deliver();
    EntityId la = addLine(*a.g->document(), {0, 0}, {1, 0});
    n.deliver();
    EntityId lb = addLine(*b.g->document(), {0, 1}, {1, 1});
    n.deliver();
    REQUIRE(n.hostDoc.modelSpaceCount() == 2);
    REQUIRE(a.g->canUndo());

    REQUIRE(a.g->undo().ok());
    n.deliver();
    CHECK(n.hostDoc.modelSpaceCount() == 1);
    CHECK(n.hostDoc.find(lb) != nullptr);  // B's line is still there
    CHECK(n.hostDoc.find(la) == nullptr);
    CHECK(canonicalDump(*b.g->document()) == canonicalDump(n.hostDoc));
    CHECK(a.g->canRedo());

    REQUIRE(a.g->redo().ok());
    n.deliver();
    CHECK(n.hostDoc.find(la) != nullptr);
    CHECK(a.g->canUndo());
    CHECK(canonicalDump(*a.g->document()) == canonicalDump(n.hostDoc));
}

TEST_CASE("session: undo is refused, not forced, when someone changed the same thing since") {
    Net n;
    auto& a = n.connect("A");
    auto& b = n.connect("B");
    n.deliver();
    EntityId id = addLine(*a.g->document(), {0, 0}, {10, 0});
    n.deliver();
    moveLine(*b.g->document(), id, {5, 5});  // B moves A's line
    n.deliver();
    auto r = a.g->undo();
    CHECK_FALSE(r.ok());
    CHECK(n.hostDoc.find(id) != nullptr);  // A's line was not erased from under B
    CHECK(canonicalDump(*a.g->document()) == canonicalDump(n.hostDoc));
    // The blocked entry is dropped, so the user is not stuck behind it.
    CHECK_FALSE(a.g->canUndo());
}

TEST_CASE("session: three peers with random edits and random delays converge") {
    for (unsigned seed = 1; seed <= 6; ++seed) {
        std::mt19937_64 rng(seed);
        Net n;
        auto& a = n.connect("A");
        auto& b = n.connect("B");
        auto& c = n.connect("C");
        n.deliver();
        std::vector<Net::Guest*> gs{&a, &b, &c};
        for (int round = 0; round < 60; ++round) {
            Document* doc = nullptr;
            switch (rng() % 4) {
                case 0:
                    doc = a.g->document();
                    break;
                case 1:
                    doc = b.g->document();
                    break;
                case 2:
                    doc = c.g->document();
                    break;
                default:
                    doc = &n.hostDoc;
            }
            std::vector<EntityId> ids = doc->modelSpace();
            if (ids.empty() || rng() % 3 == 0)
                addLine(*doc, {double(rng() % 50), 0}, {double(rng() % 50), 1});
            else if (rng() % 4 == 0) {
                Transaction tx = doc->begin("erase");
                (void)tx.erase(ids[rng() % ids.size()]);
                tx.commit();
            } else
                moveLine(*doc, ids[rng() % ids.size()], {double(rng() % 50), 9});
            // Random partial delivery.
            for (auto* g : gs) {
                if (rng() % 2)
                    n.hostReceives(*g);
                if (rng() % 2)
                    n.guestReceives(*g);
            }
            if (rng() % 5 == 0 && a.g->canUndo())
                (void)a.g->undo();
        }
        n.deliver();
        const std::string expect = canonicalDump(n.hostDoc);
        for (auto* g : gs)
            CHECK(canonicalDump(*g->g->document()) == expect);
    }
}

TEST_CASE("protocol: every message survives a round trip and garbage never crashes") {
    msg::Refused r;
    r.clientSeq = 7;
    r.reason = RejectReason::Stale;
    r.message = "m";
    r.key = Key{Space::Entity, 99};
    r.currentVersion = 5;
    r.byPeer = 2;
    r.byName = "Malee";
    auto back = must(decodeMessage(encodeMessage(r)));
    const auto* rr = std::get_if<msg::Refused>(&back);
    REQUIRE(rr);
    CHECK(rr->byName == "Malee");
    CHECK(rr->key == r.key);

    msg::Welcome w;
    w.peer = 3;
    w.role = Role::Viewer;
    w.range = {100, 200};
    w.hostName = "Nok";
    w.peers = {{0, "Nok", Role::Editor}, {1, "A", Role::Viewer}};
    w.snapshot = {1, 2, 3};
    auto wb = must(decodeMessage(encodeMessage(w)));
    const auto* ww = std::get_if<msg::Welcome>(&wb);
    REQUIRE(ww);
    CHECK(ww->peers == w.peers);
    CHECK(ww->snapshot == w.snapshot);
    CHECK(ww->range == w.range);

    std::mt19937_64 rng(5);
    for (int i = 0; i < 2000; ++i) {
        Bytes junk(rng() % 40);
        for (auto& b : junk)
            b = static_cast<std::uint8_t>(rng());
        (void)decodeMessage(junk);
    }
}

TEST_CASE("session: each user keeps their own current layer; only shared settings travel") {
    Net n;
    auto& a = n.connect("A");
    auto& b = n.connect("B");
    n.deliver();
    LayerId walls, doors;
    {
        Transaction tx = n.hostDoc.begin("layers");
        walls = must(tx.addLayer({.name = "Walls"}));
        doors = must(tx.addLayer({.name = "Doors"}));
        tx.commit();
    }
    n.deliver();
    auto setCurrent = [](Document& d, LayerId l) {
        Transaction tx = d.begin("current");
        (void)tx.modifyHeader([&](Header& h) { h.currentLayer = l; });
        tx.commit();
    };
    setCurrent(*a.g->document(), walls);
    setCurrent(*b.g->document(), doors);
    setCurrent(n.hostDoc, kLayerZero);
    n.deliver();
    CHECK(a.g->document()->header().currentLayer == walls);
    CHECK(b.g->document()->header().currentLayer == doors);
    CHECK(n.hostDoc.header().currentLayer == kLayerZero);
    CHECK(a.conflicts.empty());
    CHECK(b.conflicts.empty());

    // A shared setting changed by one reaches the others without touching their layer.
    {
        Transaction tx = a.g->document()->begin("scale");
        (void)tx.modifyHeader([&](Header& h) { h.ltScale = 2.5; });
        tx.commit();
    }
    n.deliver();
    CHECK(n.hostDoc.header().ltScale == 2.5);
    CHECK(b.g->document()->header().ltScale == 2.5);
    CHECK(b.g->document()->header().currentLayer == doors);
    CHECK(a.g->document()->header().currentLayer == walls);

    // Active-only is personal as well, and a remote edit does not switch it off.
    {
        Transaction tx = b.g->document()->begin("active");
        (void)tx.modifyHeader([&](Header& h) { h.activeOnly = true; });
        tx.commit();
    }
    addLine(*a.g->document(), {0, 0}, {1, 1});
    n.deliver();
    CHECK(b.g->document()->header().activeOnly);
    CHECK_FALSE(a.g->document()->header().activeOnly);
}

#include "helpers.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collab::testing;

namespace {

// Commits a line on the guest and returns the op the replica produced for it.
Op draw(Session::Guest& g, Vec2 a, Vec2 b, EntityId* id = nullptr) {
    Transaction tx = g.doc->begin("line");
    EntityId e = must(tx.add(line(kLayerZero, a, b)));
    if (id)
        *id = e;
    tx.commit();
    REQUIRE(!g.toHost.empty());
    return must(decodeOp(g.toHost.back()));
}

}  // namespace

TEST_CASE("host: a valid op is applied, numbered and its things get the new version") {
    Session s;
    auto& g = s.join();
    std::vector<Accepted> broadcast;
    auto prev = s.host.onAccepted;
    s.host.onAccepted = [&](const Accepted& a) {
        broadcast.push_back(a);
        prev(a);
    };
    EntityId e;
    Op op = draw(g, {0, 0}, {5, 5}, &e);
    CHECK(e.value >= s.host.ranges().rangesOf(g.peer).front().first);
    Submitted r = s.host.submit(g.peer, op, 100);
    REQUIRE(r.ok());
    CHECK(r.accepted->seq == 1);
    CHECK(r.accepted->author == g.peer);
    CHECK(r.accepted->clientSeq == op.clientSeq);
    CHECK(s.hostDoc.contains(e));
    CHECK(s.host.versions().get({Space::Entity, e.value}) == 1);
    CHECK(s.host.sequence() == 1);
    CHECK(broadcast.size() == 1);
}

TEST_CASE("host: the host's own edits are numbered like everybody else's") {
    Session s;
    auto& g = s.join();
    std::vector<Accepted> broadcast;
    auto prev = s.host.onAccepted;
    s.host.onAccepted = [&](const Accepted& a) {
        broadcast.push_back(a);
        prev(a);
    };
    EntityId e;
    {
        Transaction tx = s.hostDoc.begin("host line");
        e = must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        tx.commit();
    }
    REQUIRE(broadcast.size() == 1);
    CHECK(broadcast[0].author == kHostPeer);
    CHECK(broadcast[0].seq == 1);
    CHECK(s.host.versions().get({Space::Entity, e.value}) == 1);

    s.settle();
    CHECK(g.doc->contains(e));
    CHECK(g.replica->sequence() == 1);

    // Undo on the host is an edit as well.
    s.hostDoc.apply(ChangeSet{}, Direction::Redo);  // empty: nothing recorded
    CHECK(broadcast.size() == 1);
}

TEST_CASE("host: the second of two edits to the same entity is refused as stale") {
    Session s;
    auto& a = s.join();
    auto& b = s.join();
    EntityId e;
    {
        Transaction tx = s.hostDoc.begin("seed");
        e = must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        tx.commit();
    }
    s.settle();

    auto move = [&](Session::Guest& g, Vec2 to) {
        Transaction tx = g.doc->begin("move");
        REQUIRE(tx.modify(e, [&](Entity& x) { std::get<LineData>(x.data).b = to; }).ok());
        tx.commit();
    };
    move(a, {10, 0});
    move(b, {0, 10});
    s.hostReceive(a);
    s.hostReceive(b);
    REQUIRE(s.reasons.size() == 1);
    CHECK(s.reasons[0] == RejectReason::Stale);
    CHECK(b.refusals.size() == 1);
    CHECK(a.refusals.empty());
    s.settle();
    // Everyone sees a's version; b's move was rolled back.
    auto b1 = std::get<LineData>(s.hostDoc.find(e)->data).b;
    CHECK(b1.x == 10);
    CHECK(canonicalDump(*b.doc) == canonicalDump(s.hostDoc));
    CHECK(canonicalDump(*a.doc) == canonicalDump(s.hostDoc));
    CHECK(b.rejected == 1);
}

TEST_CASE("host: editing something that was erased in the meantime is refused") {
    Session s;
    auto& a = s.join();
    auto& b = s.join();
    EntityId e;
    {
        Transaction tx = s.hostDoc.begin("seed");
        e = must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        tx.commit();
    }
    s.settle();
    {
        Transaction tx = a.doc->begin("erase");
        REQUIRE(tx.erase(e).ok());
        tx.commit();
    }
    {
        Transaction tx = b.doc->begin("move");
        REQUIRE(tx.modify(e, [](Entity& x) { std::get<LineData>(x.data).b = {9, 9}; }).ok());
        tx.commit();
    }
    s.settle();
    CHECK_FALSE(s.hostDoc.contains(e));
    CHECK_FALSE(b.doc->contains(e));
    CHECK(canonicalDump(*b.doc) == canonicalDump(s.hostDoc));
    REQUIRE(s.reasons.size() == 1);
    CHECK(s.reasons[0] == RejectReason::Stale);
}

TEST_CASE("host: two guests creating a layer with the same id: the second is refused") {
    Session s;
    auto& a = s.join();
    auto& b = s.join();
    for (auto* g : {&a, &b}) {
        Transaction tx = g->doc->begin("layer");
        (void)must(tx.addLayer({.name = g == &a ? "FromA" : "FromB"}));
        tx.commit();
    }
    s.settle();
    REQUIRE(s.reasons.size() == 1);
    CHECK(s.reasons[0] == RejectReason::Stale);
    CHECK(s.hostDoc.layerByName("FromA") != nullptr);
    CHECK(s.hostDoc.layerByName("FromB") == nullptr);
    CHECK(canonicalDump(*b.doc) == canonicalDump(s.hostDoc));
}

TEST_CASE("host: a viewer, a stranger and an author mismatch are not allowed") {
    Session s;
    auto& v = s.join(Role::Viewer);
    auto& e = s.join();
    // Hand-made ops so the guests' own replicas do not get in the way.
    Document scratch;
    ChangeSet cs;
    {
        Transaction tx = scratch.begin("x");
        (void)must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        cs = tx.commit();
    }
    Op op = makeOp(v.peer, 1, cs, {});
    auto r = s.host.submit(v.peer, op, 1);
    REQUIRE_FALSE(r.ok());
    CHECK(r.rejection->reason == RejectReason::NotAllowed);

    Op stranger = makeOp(99, 1, cs, {});
    CHECK(s.host.submit(99, stranger, 2).rejection->reason == RejectReason::UnknownPeer);

    Op forged = makeOp(v.peer, 1, cs, {});
    CHECK(s.host.submit(e.peer, forged, 3).rejection->reason == RejectReason::Malformed);

    // Promoting the viewer makes the same op acceptable (id outside the range though).
    CHECK(s.host.setRole(v.peer, Role::Editor));
    CHECK(s.host.roleOf(v.peer) == Role::Editor);
    CHECK_FALSE(s.host.setRole(1234, Role::Editor));
}

TEST_CASE("host: new entities must use the guest's own ids") {
    Session s;
    auto& a = s.join();
    auto& b = s.join();
    Document scratch;
    REQUIRE(scratch.setEntityIdWindow(b.replica->idsLeft() ? 1 : 1, 1000).ok());
    // Use b's range for an op sent by a.
    IdRange br = s.host.ranges().rangesOf(b.peer).front();
    Document other;
    REQUIRE(other.setEntityIdWindow(br.first, br.limit).ok());
    ChangeSet cs;
    {
        Transaction tx = other.begin("x");
        (void)must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        cs = tx.commit();
    }
    auto r = s.host.submit(a.peer, makeOp(a.peer, 1, cs, {}), 1);
    REQUIRE_FALSE(r.ok());
    CHECK(r.rejection->reason == RejectReason::OutsideRange);
    // Own range is fine.
    IdRange ar = s.host.ranges().rangesOf(a.peer).front();
    Document mine;
    REQUIRE(mine.setEntityIdWindow(ar.first, ar.limit).ok());
    {
        Transaction tx = mine.begin("x");
        (void)must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        cs = tx.commit();
    }
    CHECK(s.host.submit(a.peer, makeOp(a.peer, 2, cs, {}), 2).ok());
}

TEST_CASE("host: edits that break the rules of the drawing are refused and change nothing") {
    Session s;
    auto& a = s.join();
    const std::string before = canonicalDump(s.hostDoc);
    IdRange ar = s.host.ranges().rangesOf(a.peer).front();
    Document mine;
    REQUIRE(mine.setEntityIdWindow(ar.first, ar.limit).ok());
    // An entity on a layer only the guest's copy has.
    ChangeSet cs;
    {
        Transaction tx = mine.begin("x");
        LayerId l = must(tx.addLayer({.name = "OnlyHere"}));
        (void)must(tx.add(line(l, {0, 0}, {1, 1})));
        cs = tx.commit();
    }
    Op op = makeOp(a.peer, 1, cs, {});
    // Drop the layer from the op: the entity now names a layer the host does not know.
    op.delta.layers.clear();
    std::vector<Base> bases;
    for (const Key& k : keysOf(op.delta))
        bases.push_back({k, 0, false});
    op.bases = bases;
    auto r = s.host.submit(a.peer, op, 1);
    REQUIRE_FALSE(r.ok());
    CHECK(r.rejection->reason == RejectReason::Invalid);
    CHECK(canonicalDump(s.hostDoc) == before);
    CHECK(s.host.sequence() == 0);
}

TEST_CASE("host: bases that do not match the edit are malformed") {
    Session s;
    auto& a = s.join();
    EntityId e;
    Op op = draw(a, {0, 0}, {1, 1}, &e);
    a.toHost.clear();

    Op fewer = op;
    fewer.bases.pop_back();
    CHECK(s.host.submit(a.peer, fewer, 1).rejection->reason == RejectReason::Malformed);
    Op moved = op;
    moved.bases[0].key.id += 1;
    CHECK(s.host.submit(a.peer, moved, 2).rejection->reason == RejectReason::Malformed);
    Op empty;
    empty.author = a.peer;
    CHECK(s.host.submit(a.peer, empty, 3).rejection->reason == RejectReason::Malformed);
    CHECK(s.host.submit(a.peer, op, 4).ok());
}

TEST_CASE("host: a guest that sends too fast is slowed down, and size limits hold") {
    HostConfig cfg;
    cfg.maxOpsPerWindow = 3;
    cfg.rateWindowMs = 1000;
    Session s(cfg);
    auto& a = s.join();
    // Ops made by hand, so that the replica's one-at-a-time rule does not get in the way.
    IdRange range = s.host.ranges().rangesOf(a.peer).front();
    Document scratch;
    REQUIRE(scratch.setEntityIdWindow(range.first, range.limit).ok());
    auto next = [&](std::uint64_t seq) {
        Transaction tx = scratch.begin("x");
        (void)must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        return makeOp(a.peer, seq, tx.commit(), {});
    };
    std::size_t limited = 0;
    for (int i = 0; i < 6; ++i) {
        auto r = s.host.submit(a.peer, next(std::uint64_t(i) + 1), 100 + std::uint64_t(i));
        if (!r.ok() && r.rejection->reason == RejectReason::RateLimited)
            ++limited;
    }
    CHECK(limited == 3);
    // After the window the guest may send again.
    CHECK(s.host.submit(a.peer, next(7), 5000).ok());

    // Bytes that do not decode are malformed, oversize ones are too large.
    std::vector<std::uint8_t> junk = {1, 2, 3};
    CHECK(s.host.submitEncoded(a.peer, junk, 6000).rejection->reason == RejectReason::Malformed);
    HostConfig small;
    small.limits.maxBytes = 8;
    Session t(small);
    auto& g = t.join();
    std::vector<std::uint8_t> big(100, 0);
    CHECK(t.host.submitEncoded(g.peer, big, 1).rejection->reason == RejectReason::TooLarge);
}

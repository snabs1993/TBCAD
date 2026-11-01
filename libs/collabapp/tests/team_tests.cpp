#include <doctest/doctest.h>

#include "tbcad/collab/host.h"
#include "tbcad/collab/replica.h"
#include "tbcad/collab/snapshot.h"
#include "team.h"

using namespace tbcad;
using namespace tbcad::collabapp;

TEST_CASE("team: chat and claim frames round-trip and junk is refused") {
    auto c = readChat(chatFrame(7, "สวัสดี"));
    REQUIRE(c);
    CHECK(c->from == 7);
    CHECK(c->text == "สวัสดี");
    CHECK(readChat(restampChat(chatFrame(7, "x"), 3))->from == 3);
    CHECK_FALSE(readChat(Bytes{kChat, 1}));
    auto r = readClaim(claimFrame(true, LayerId{5}));
    REQUIRE(r);
    CHECK(r->claim);
    CHECK(r->layer == LayerId{5});
    CHECK_FALSE(readClaim(Bytes{kClaim, 9, 0, 0, 0, 0}));
}

TEST_CASE("team: one owner per layer, release only by the owner, table travels") {
    Claims host;
    CHECK(host.claim(1, LayerId{10}));
    CHECK_FALSE(host.claim(2, LayerId{10}));
    CHECK_FALSE(host.release(2, LayerId{10}));
    CHECK(host.claim(2, LayerId{11}));
    Claims guest;
    REQUIRE(guest.readTable(host.tableFrame()));
    CHECK(guest.ownerOf(LayerId{10}) == 1u);
    CHECK(guest.ownerOf(LayerId{11}) == 2u);
    CHECK(host.releaseAll(1));
    CHECK_FALSE(host.ownerOf(LayerId{10}));
    Bytes bad = host.tableFrame();
    bad.pop_back();
    CHECK_FALSE(guest.readTable(bad));
}

TEST_CASE("team: the host refuses a guest's edit on a layer somebody else keeps") {
    Document hostDoc;
    collab::Host host(hostDoc);
    const collab::PeerId a = host.addPeer(collab::Role::Editor);
    const collab::PeerId b = host.addPeer(collab::Role::Editor);
    Claims claims;
    REQUIRE(claims.claim(b, kLayerZero));
    host.policy = [&](collab::PeerId from, const collab::Op& op, const Document& d) {
        return claims.check(from, op, d, [](collab::PeerId) { return std::string("Nok"); });
    };

    auto snap = collab::loadSnapshot(
        collab::saveSnapshot(hostDoc, host.versions(), host.sequence()).value());
    REQUIRE(snap.ok());
    std::unique_ptr<Document> doc = std::move(snap.value().document);
    collab::Replica rep(*doc, a);
    rep.restore(snap.value().versions, snap.value().sequence);
    REQUIRE(rep.addRange(host.grantRange(a)).ok());
    std::vector<collab::Op> sent;
    rep.onLocalOp = [&](const collab::Op& op) { sent.push_back(op); };

    Transaction tx = doc->begin("line");
    Entity e;
    e.common.layer = kLayerZero;
    e.data = LineData{{0, 0}, {1, 1}};
    REQUIRE(tx.add(std::move(e)).ok());
    tx.commit();
    REQUIRE(sent.size() == 1);

    collab::Submitted r = host.submit(a, sent[0], 10);
    REQUIRE_FALSE(r.ok());
    CHECK(r.rejection->message.find("claimed by Nok") != std::string::npos);
    CHECK(hostDoc.entityCount() == 0);
    // The owner may.
    claims.releaseAll(b);
    REQUIRE(claims.claim(a, kLayerZero));
    CHECK(host.submit(a, sent[0], 20).ok());
}

TEST_CASE("team: hand-over port messages round-trip and refuse junk addresses") {
    auto m = readPort(portFrame(kMoveTo, 47800, "192.168.1.20"), kMoveTo);
    REQUIRE(m);
    CHECK(m->port == 47800);
    CHECK(m->address == "192.168.1.20");
    CHECK_FALSE(readPort(portFrame(kMoveTo, 1, "evil;host"), kMoveTo));
    CHECK_FALSE(readPort(portFrame(kHostReady, 1), kMoveTo));
}

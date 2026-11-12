#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <thread>

#include "tbcad/collab/host.h"
#include "tbcad/collab/session.h"
#include "tbcad/collabnet/tcp.h"
#include "tbcad/core/dump.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collabnet;

namespace {

net::Argon2Params fastKdf() {
    net::Argon2Params p;
    p.timeCost = 1;
    p.memoryKiB = 64;
    return p;
}

Entity makeLine(Vec2 a, Vec2 b) {
    Entity e;
    e.common.layer = kLayerZero;
    e.data = LineData{a, b};
    return e;
}

EntityId drawLine(Document& d, Vec2 a, Vec2 b) {
    Transaction tx = d.begin("line");
    auto id = tx.add(makeLine(a, b));
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}

// Pumps everybody until `done` or a time limit.
bool pumpUntil(TcpHost& host, std::vector<TcpGuest*> guests, const std::function<bool()>& done,
               int limitMs = 8000) {
    auto start = std::chrono::steady_clock::now();
    std::uint64_t now = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(limitMs)) {
        host.pump(now += 10);
        for (TcpGuest* g : guests)
            g->pump();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

TEST_CASE("tcp: two guests join over TLS, edit, and end with the same drawing") {
    Document hostDoc;
    drawLine(hostDoc, {0, 0}, {5, 5});
    Host host(hostDoc);
    HostSession hs(host, "plan room");
    TcpHost server(hs);
    nettls::Psk psk = nettls::derivePsk("correct horse", "plan room", fastKdf());
    std::string error;
    REQUIRE(server.start(0, false, psk, error));
    REQUIRE(server.port() > 0);

    TcpGuest ta, tb;
    GuestSession ga(ta.sender()), gb(tb.sender());
    ta.attach(ga);
    tb.attach(gb);
    REQUIRE(ta.connect("127.0.0.1", server.port(), psk, "Somchai", 3000, error));
    REQUIRE(tb.connect("127.0.0.1", server.port(), psk, "Nok", 3000, error));
    REQUIRE(pumpUntil(server, {&ta, &tb}, [&] {
        return ga.state() == GuestSession::State::Joined &&
               gb.state() == GuestSession::State::Joined;
    }));
    CHECK(canonicalDump(*ga.document()) == canonicalDump(hostDoc));
    CHECK(hs.peers().size() == 2);

    drawLine(*ga.document(), {1, 1}, {2, 2});
    drawLine(*gb.document(), {3, 3}, {4, 4});
    drawLine(hostDoc, {6, 6}, {7, 7});
    REQUIRE(pumpUntil(server, {&ta, &tb}, [&] {
        return hostDoc.modelSpaceCount() == 4 && ga.document()->modelSpaceCount() == 4 &&
               gb.document()->modelSpaceCount() == 4 && ga.replica()->pendingCount() == 0 &&
               gb.replica()->pendingCount() == 0;
    }));
    CHECK(canonicalDump(*ga.document()) == canonicalDump(hostDoc));
    CHECK(canonicalDump(*gb.document()) == canonicalDump(hostDoc));

    // A guest leaves: the host and the other guest hear about it.
    ga.leave();
    ta.close();
    REQUIRE(pumpUntil(server, {&tb}, [&] { return hs.peers().size() == 1; }));
    server.stop();
    CHECK_FALSE(server.running());
}

TEST_CASE("tcp: a wrong password never joins") {
    Document hostDoc;
    Host host(hostDoc);
    HostSession hs(host);
    TcpHost server(hs);
    std::string error;
    REQUIRE(server.start(0, false, nettls::derivePsk("right", "room", fastKdf()), error));

    TcpGuest t;
    GuestSession g(t.sender());
    t.attach(g);
    bool ok = t.connect("127.0.0.1", server.port(), nettls::derivePsk("wrong", "room", fastKdf()),
                        "Intruder", 3000, error);
    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
    pumpUntil(server, {&t}, [] { return false; }, 300);
    CHECK(hs.peers().empty());
    CHECK(g.state() == GuestSession::State::Idle);
}

TEST_CASE("tcp: the host stops sharing and the guest is told") {
    Document hostDoc;
    Host host(hostDoc);
    HostSession hs(host);
    TcpHost server(hs);
    nettls::Psk psk = nettls::derivePsk("pw", "room", fastKdf());
    std::string error;
    REQUIRE(server.start(0, false, psk, error));
    TcpGuest t;
    GuestSession g(t.sender());
    t.attach(g);
    std::string why;
    g.onClosed = [&](const std::string& w) { why = w; };
    REQUIRE(t.connect("127.0.0.1", server.port(), psk, "A", 3000, error));
    REQUIRE(pumpUntil(server, {&t}, [&] { return g.state() == GuestSession::State::Joined; }));
    server.stop();
    for (int i = 0; i < 100 && g.state() != GuestSession::State::Closed; ++i) {
        t.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(g.state() == GuestSession::State::Closed);
    CHECK_FALSE(why.empty());
}

TEST_CASE("tcp: a client that connects and says nothing does not block the host") {
    Document hostDoc;
    Host host(hostDoc);
    HostSession hs(host);
    TcpHost server(hs);
    nettls::Psk psk = nettls::derivePsk("pw", "room", fastKdf());
    std::string error;
    REQUIRE(server.start(0, false, psk, error));
    net::Handle silent = net::connectLoopback(server.port(), 2000);
    REQUIRE(silent != net::kInvalid);

    TcpGuest t;
    GuestSession g(t.sender());
    t.attach(g);
    REQUIRE(t.connect("127.0.0.1", server.port(), psk, "A", 6000, error));
    REQUIRE(pumpUntil(server, {&t}, [&] { return g.state() == GuestSession::State::Joined; }));
    net::closeSocket(silent);
    server.stop();
}

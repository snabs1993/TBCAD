// The open-source session engine over real encrypted links, as the program uses it.
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "link.h"
#include "tbcad/collab/session.h"
#include "tbcad/core/document.h"

using namespace tbcad;
using namespace tbcad::collabapp;

namespace {

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

EntityId drawLine(Document& d, Vec2 a, Vec2 b) {
    Transaction tx = d.begin("line");
    Entity e;
    e.common.layer = kLayerZero;
    e.data = LineData{a, b};
    auto id = tx.add(std::move(e));
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}

// One host with one guest, both pumped from the test thread like the program's main loop.
struct Rig {
    Document hostDoc;
    collab::Host host{hostDoc};
    collab::HostSession hs{host, "live"};
    std::unique_ptr<Listener> listener;
    std::shared_ptr<Link> hostLink, guestLink;
    collab::ConnId conn = 0;
    std::unique_ptr<collab::GuestSession> guest;

    Rig() {
        std::string err;
        listener = Listener::start(0, "pw-for-test", {}, err);
        REQUIRE_MESSAGE(listener, err);
        guestLink = Link::dial("127.0.0.1", listener->port(), "pw-for-test", {});
        auto g = guestLink;
        guest = std::make_unique<collab::GuestSession>([g](const collab::Frame& f) { g->send(f); });
        guest->join("Guest");
    }

    void pump() {
        for (auto& l : listener->takeNew()) {
            hostLink = l;
            auto h = hostLink;
            conn = hs.accept([h](const collab::Frame& f) { h->send(f); }, [h] { h->close(); });
        }
        Frame f;
        while (hostLink && hostLink->poll(f))
            hs.receive(conn, f, nowMs());
        while (guestLink->poll(f))
            guest->receive(f);
    }
    template <class Pred>
    bool until(Pred done, int ms = 10000) {
        for (int i = 0; i < ms / 5; ++i) {
            pump();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    }
};

}  // namespace

TEST_CASE("live session: a guest joins, sees the drawing and edits travel both ways") {
    Rig rig;
    drawLine(rig.hostDoc, {0, 0}, {10, 0});
    REQUIRE(rig.until([&] { return rig.guest->state() == collab::GuestSession::State::Joined; }));
    Document* gd = rig.guest->document();
    REQUIRE(gd != nullptr);
    CHECK(gd->entityCount() == rig.hostDoc.entityCount());

    // host edit reaches the guest
    drawLine(rig.hostDoc, {0, 5}, {10, 5});
    REQUIRE(rig.until([&] { return gd->entityCount() == rig.hostDoc.entityCount(); }));

    // guest edit reaches the host
    drawLine(*gd, {1, 1}, {2, 2});
    REQUIRE(rig.until([&] { return rig.hostDoc.entityCount() == gd->entityCount(); }));
    CHECK(rig.hostDoc.entityCount() == 3);
}

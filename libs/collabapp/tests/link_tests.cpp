#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "link.h"

using namespace tbcad::collabapp;

namespace {

template <class Pred>
bool waitFor(Pred done, int ms = 8000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

}  // namespace

TEST_CASE("link: frames travel both ways over an encrypted connection") {
    std::string err;
    auto listener = Listener::start(0, "secret-pass", {}, err);
    REQUIRE_MESSAGE(listener, err);
    auto guest = Link::dial("127.0.0.1", listener->port(), "secret-pass", {});

    std::shared_ptr<Link> server;
    REQUIRE(waitFor([&] {
        auto fresh = listener->takeNew();
        if (!fresh.empty())
            server = fresh.front();
        return server != nullptr;
    }));
    guest->send({1, 2, 3});
    server->send({9, 8});
    Frame f;
    REQUIRE(waitFor([&] { return server->poll(f); }));
    CHECK(f == Frame{1, 2, 3});
    REQUIRE(waitFor([&] { return guest->poll(f); }));
    CHECK(f == Frame{9, 8});
}

TEST_CASE("link: a wrong password never gets a connection") {
    std::string err;
    auto listener = Listener::start(0, "right-password", {}, err);
    REQUIRE_MESSAGE(listener, err);
    auto guest = Link::dial("127.0.0.1", listener->port(), "wrong-password", {});
    REQUIRE(waitFor([&] { return guest->finished(); }));
    CHECK_FALSE(guest->error().empty());
}

TEST_CASE("link: dialing nobody reports an error") {
    auto guest = Link::dial("127.0.0.1", 1, "x-password", {});
    REQUIRE(waitFor([&] { return guest->finished(); }));
    CHECK_FALSE(guest->error().empty());
}

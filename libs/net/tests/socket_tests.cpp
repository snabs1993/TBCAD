#include <doctest/doctest.h>

#include <string>
#include <thread>

#include "tbcad/net/socket.h"

using namespace tbcad::net;

TEST_CASE("net: loopback listener accepts, echoes and reports its address") {
    REQUIRE(initialize());
    int port = 0;
    std::string err;
    Handle l = listenLoopback(0, false, port, err);
    REQUIRE(l != kInvalid);
    CHECK(port > 0);
    CHECK(localAddress(l) == "127.0.0.1");

    std::thread server([&] {
        Handle c = acceptWithTimeout(l, 3000);
        REQUIRE(c != kInvalid);
        char buf[16];
        long n = receive(c, buf, sizeof buf, 3000);
        REQUIRE(n == 4);
        CHECK(sendAll(c, buf, static_cast<std::size_t>(n)));
        closeSocket(c);
    });
    Handle s = connectLoopback(port, 2000);
    REQUIRE(s != kInvalid);
    CHECK(sendAll(s, "ping", 4));
    char out[16] = {};
    CHECK(receive(s, out, sizeof out, 3000) == 4);
    CHECK(std::string(out, 4) == "ping");
    CHECK(receive(s, out, sizeof out, 3000) == kClosed);
    closeSocket(s);
    server.join();
    closeSocket(l);
}

TEST_CASE("net: connectTcp resolves names and numbers; any-interface listener works") {
    REQUIRE(initialize());
    int port = 0;
    std::string err;
    Handle l = listenTcp("", 0, false, port, err);
    REQUIRE(l != kInvalid);
    Handle a = connectTcp("127.0.0.1", port, 2000);
    CHECK(a != kInvalid);
    Handle b = connectTcp("localhost", port, 2000);
    CHECK(b != kInvalid);
    Handle c1 = acceptWithTimeout(l, 2000);
    Handle c2 = acceptWithTimeout(l, 2000);
    CHECK(c1 != kInvalid);
    CHECK(c2 != kInvalid);
    for (Handle h : {a, b, c1, c2})
        if (h != kInvalid)
            closeSocket(h);
    closeSocket(l);
}

TEST_CASE("net: errors are reported, not thrown") {
    REQUIRE(initialize());
    int port = 0;
    std::string err;
    CHECK(listenTcp("not-an-ip", 0, false, port, err) == kInvalid);
    CHECK_FALSE(err.empty());
    CHECK(connectTcp("no-such-host.invalid", 9, 500) == kInvalid);

    Handle l = listenLoopback(0, false, port, err);
    REQUIRE(l != kInvalid);
    CHECK(acceptWithTimeout(l, 50) == kInvalid);  // timeout
    Handle s = connectLoopback(port, 1000);
    REQUIRE(s != kInvalid);
    char b[4];
    CHECK(receive(s, b, sizeof b, 50) == kTimeout);
    closeSocket(s);
    closeSocket(l);
}

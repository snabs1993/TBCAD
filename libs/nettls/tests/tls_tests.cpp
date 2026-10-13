#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <vector>

#include "tbcad/nettls/tls.h"

using namespace tbcad;

namespace {

net::Argon2Params fast() {
    net::Argon2Params p;
    p.timeCost = 1;
    p.memoryKiB = 64;
    return p;
}

struct Link {
    net::Handle listener = net::kInvalid;
    int port = 0;
    Link() {
        REQUIRE(net::initialize());
        std::string err;
        listener = net::listenLoopback(0, false, port, err);
        REQUIRE(listener != net::kInvalid);
    }
    ~Link() { net::closeSocket(listener); }
};

struct Result {
    std::unique_ptr<net::Channel> channel;
    std::string error;
};

// Runs both ends of a handshake, each on its own thread.
void handshake(Link& link, const nettls::Psk& client, const nettls::Psk& server, Result& c,
               Result& s, int timeoutMs = 5000) {
    std::thread acceptor([&] {
        net::Handle h = net::acceptWithTimeout(link.listener, 3000);
        REQUIRE(h != net::kInvalid);
        s.channel = nettls::acceptTls(h, server, timeoutMs, s.error);
    });
    net::Handle h = net::connectLoopback(link.port, 3000);
    REQUIRE(h != net::kInvalid);
    c.channel = nettls::connectTls(h, client, timeoutMs, c.error);
    acceptor.join();
}

}  // namespace

TEST_CASE("tls: same password connects and carries frames both ways") {
    Link link;
    auto psk = nettls::derivePsk("correct horse", "plan-room", fast());
    REQUIRE(psk.key.size() == 32);
    Result c, s;
    handshake(link, psk, psk, c, s);
    REQUIRE(c.channel);
    REQUIRE(s.channel);

    std::vector<std::uint8_t> big(200000);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>(i * 7);
    std::thread writer([&] {
        CHECK(net::sendFrame(*c.channel, "ping", 4));
        CHECK(net::sendFrame(*c.channel, big.data(), big.size()));
    });
    net::FrameReader r;
    std::vector<std::uint8_t> f;
    REQUIRE(r.poll(*s.channel, f, 5000) == net::FrameStatus::Ok);
    CHECK(std::string(f.begin(), f.end()) == "ping");
    REQUIRE(r.poll(*s.channel, f, 5000) == net::FrameStatus::Ok);
    CHECK(f == big);
    writer.join();

    CHECK(net::sendFrame(*s.channel, "pong", 4));
    net::FrameReader r2;
    REQUIRE(r2.poll(*c.channel, f, 5000) == net::FrameStatus::Ok);
    CHECK(std::string(f.begin(), f.end()) == "pong");

    // an orderly close is seen as end of stream, a quiet line as a timeout
    CHECK(r2.poll(*c.channel, f, 50) == net::FrameStatus::Timeout);
    s.channel->close();
    CHECK(r2.poll(*c.channel, f, 3000) == net::FrameStatus::Closed);
}

TEST_CASE("tls: a wrong password fails the handshake on both sides") {
    Link link;
    auto good = nettls::derivePsk("correct horse", "plan-room", fast());
    auto bad = nettls::derivePsk("wrong horse", "plan-room", fast());
    Result c, s;
    handshake(link, bad, good, c, s);
    CHECK_FALSE(c.channel);
    CHECK_FALSE(s.channel);
    CHECK_FALSE(c.error.empty());
    CHECK_FALSE(s.error.empty());
}

TEST_CASE("tls: a peer that does not speak TLS is turned away") {
    Link link;
    auto psk = nettls::derivePsk("pw", "room", fast());
    std::thread plain([&] {
        net::Handle h = net::connectLoopback(link.port, 3000);
        REQUIRE(h != net::kInvalid);
        net::sendAll(h, "GET / HTTP/1.1\r\n\r\n", 18);
        char b[64];
        net::receive(h, b, sizeof b, 2000);
        net::closeSocket(h);
    });
    net::Handle h = net::acceptWithTimeout(link.listener, 3000);
    REQUIRE(h != net::kInvalid);
    std::string error;
    auto ch = nettls::acceptTls(h, psk, 3000, error);
    CHECK_FALSE(ch);
    CHECK_FALSE(error.empty());
    plain.join();
}

TEST_CASE("tls: the handshake times out against a silent peer") {
    Link link;
    auto psk = nettls::derivePsk("pw", "room", fast());
    net::Handle quiet = net::connectLoopback(link.port, 3000);
    net::Handle h = net::acceptWithTimeout(link.listener, 3000);
    REQUIRE(h != net::kInvalid);
    std::string error;
    auto ch = nettls::acceptTls(h, psk, 300, error);
    CHECK_FALSE(ch);
    CHECK(error.find("timed out") != std::string::npos);
    net::closeSocket(quiet);
}

TEST_CASE("tls: one thread reads while others write") {
    Link link;
    auto psk = nettls::derivePsk("pw", "room", fast());
    Result c, s;
    handshake(link, psk, psk, c, s);
    REQUIRE(c.channel);
    REQUIRE(s.channel);
    constexpr int kFrames = 200;
    std::thread server([&] {
        net::FrameReader r;
        std::vector<std::uint8_t> f;
        for (int i = 0; i < kFrames * 2; ++i) {
            REQUIRE(r.poll(*s.channel, f, 5000) == net::FrameStatus::Ok);
            // echo from the reading thread while the client's two writers keep sending
            CHECK(net::sendFrame(*s.channel, f.data(), f.size()));
        }
    });
    auto sender = [&](char tag) {
        for (int i = 0; i < kFrames; ++i) {
            std::string m(1, tag);
            m += std::to_string(i);
            CHECK(net::sendFrame(*c.channel, m.data(), m.size()));
        }
    };
    std::thread w1(sender, 'a'), w2(sender, 'b');
    net::FrameReader r;
    std::vector<std::uint8_t> f;
    int got = 0;
    while (got < kFrames * 2) {
        REQUIRE(r.poll(*c.channel, f, 5000) == net::FrameStatus::Ok);
        ++got;
    }
    w1.join();
    w2.join();
    server.join();
}

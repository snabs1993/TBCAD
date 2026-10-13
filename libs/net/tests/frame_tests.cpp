#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <vector>

#include "tbcad/net/channel.h"

using namespace tbcad::net;

namespace {

struct Pair {
    std::unique_ptr<Channel> a, b;  // a = connecting side, b = accepted side
    Pair() {
        REQUIRE(initialize());
        int port = 0;
        std::string err;
        Handle l = listenLoopback(0, false, port, err);
        REQUIRE(l != kInvalid);
        Handle c = connectLoopback(port, 2000);
        Handle s = acceptWithTimeout(l, 2000);
        REQUIRE(c != kInvalid);
        REQUIRE(s != kInvalid);
        closeSocket(l);
        a = makePlainChannel(c);
        b = makePlainChannel(s);
    }
};

}  // namespace

TEST_CASE("frames: payloads of any size arrive intact and in order") {
    Pair p;
    std::vector<std::uint8_t> big(300000);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>(i * 31);
    std::thread writer([&] {
        CHECK(sendFrame(*p.a, "hello", 5));
        CHECK(sendFrame(*p.a, nullptr, 0));
        CHECK(sendFrame(*p.a, big.data(), big.size()));
    });
    FrameReader r;
    std::vector<std::uint8_t> f;
    REQUIRE(r.poll(*p.b, f, 3000) == FrameStatus::Ok);
    CHECK(std::string(f.begin(), f.end()) == "hello");
    REQUIRE(r.poll(*p.b, f, 3000) == FrameStatus::Ok);
    CHECK(f.empty());
    REQUIRE(r.poll(*p.b, f, 3000) == FrameStatus::Ok);
    CHECK(f == big);
    writer.join();
}

TEST_CASE("frames: a frame split across timeouts is not lost") {
    Pair p;
    const unsigned char raw[] = {3, 0, 0, 0, 'a', 'b', 'c'};
    FrameReader r;
    std::vector<std::uint8_t> f;
    CHECK(r.poll(*p.b, f, 30) == FrameStatus::Timeout);
    REQUIRE(p.a->writeAll(raw, 2));
    CHECK(r.poll(*p.b, f, 50) == FrameStatus::Timeout);
    REQUIRE(p.a->writeAll(raw + 2, 3));
    CHECK(r.poll(*p.b, f, 50) == FrameStatus::Timeout);
    REQUIRE(p.a->writeAll(raw + 5, 2));
    REQUIRE(r.poll(*p.b, f, 2000) == FrameStatus::Ok);
    CHECK(std::string(f.begin(), f.end()) == "abc");
}

TEST_CASE("frames: oversized and closed streams are reported") {
    {
        Pair p;
        const unsigned char hdr[] = {0x01, 0x00, 0x00, 0x01};  // length 0x01000001
        REQUIRE(p.a->writeAll(hdr, 4));
        FrameReader r(1024);
        std::vector<std::uint8_t> f;
        CHECK(r.poll(*p.b, f, 2000) == FrameStatus::TooLarge);
        CHECK_FALSE(sendFrame(*p.a, nullptr, kMaxFrameBytes + 1));
    }
    {
        Pair p;
        p.a->close();
        FrameReader r;
        std::vector<std::uint8_t> f;
        CHECK(r.poll(*p.b, f, 2000) == FrameStatus::Closed);
    }
}

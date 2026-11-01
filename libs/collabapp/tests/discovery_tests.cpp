#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "discovery.h"

using namespace tbcad::collabapp;

TEST_CASE("discovery: announcement text round-trips and rejects junk") {
    std::string name;
    int port = 0;
    REQUIRE(parseAnnouncement(makeAnnouncement("Somchai", 47800), name, port));
    CHECK(name == "Somchai");
    CHECK(port == 47800);
    CHECK_FALSE(parseAnnouncement("hello", name, port));
    CHECK_FALSE(parseAnnouncement("TBCAD-COLLAB 1\nx\n99999999", name, port));
    CHECK_FALSE(parseAnnouncement("TBCAD-COLLAB 1\n\n80", name, port));
}

TEST_CASE("discovery: a finder hears an announcer on this computer") {
    Finder finder(0);
    if (!finder.listening())
        return;  // no UDP in this environment
    Announcer announcer("Host A", 47811, finder.port(), "127.0.0.1");
    std::vector<Announcement> hosts;
    for (int i = 0; i < 300 && hosts.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        hosts = finder.hosts();
    }
    REQUIRE(hosts.size() == 1);
    CHECK(hosts[0].name == "Host A");
    CHECK(hosts[0].port == 47811);
    CHECK(hosts[0].address == "127.0.0.1");
}

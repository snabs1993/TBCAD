#pragma once

// Finding a host on the local network: the host announces itself by UDP broadcast, a person who
// wants to join listens. Only the name and port are sent (never the password).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tbcad::collabapp {

inline constexpr int kDiscoveryPort = 47801;

struct Announcement {
    std::string name{};
    std::string address{};  // dotted IPv4 the announcement came from
    int port = 0;
    std::uint64_t seenMs = 0;
};

// Dotted IPv4 of the other end of a connected TCP socket; empty when unknown.
std::string peerAddressOf(std::intptr_t socket);
// This computer's address on the local network (the one used to reach other machines); empty when
// there is no network.
std::string localLanAddress();

// "TBCAD-COLLAB 1\nname\nport" and back. parse returns false for anything else.
std::string makeAnnouncement(const std::string& name, int port);
bool parseAnnouncement(const std::string& text, std::string& name, int& port);

// Sends the announcement every two seconds until destroyed.
class Announcer {
public:
    Announcer(std::string name, int port, int udpPort = kDiscoveryPort,
              std::string target = "255.255.255.255");
    ~Announcer();
    Announcer(const Announcer&) = delete;
    Announcer& operator=(const Announcer&) = delete;

private:
    std::atomic<bool> stop_{false};
    std::thread thread_{};
};

// Collects announcements heard on the network.
class Finder {
public:
    explicit Finder(int udpPort = kDiscoveryPort);
    ~Finder();
    Finder(const Finder&) = delete;
    Finder& operator=(const Finder&) = delete;
    bool listening() const { return listening_; }
    // Hosts heard in the last ten seconds.
    std::vector<Announcement> hosts() const;
    // Port actually bound (differs from the request when 0 was asked).
    int port() const { return port_; }

private:
    mutable std::mutex m_{};
    std::vector<Announcement> heard_{};
    std::atomic<bool> stop_{false}, listening_{false};
    int port_ = 0;
    std::thread thread_{};
};

}  // namespace tbcad::collabapp

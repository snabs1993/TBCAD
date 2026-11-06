#include "discovery.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SockT = SOCKET;
static void closeSock(SockT s) {
    closesocket(s);
}
static bool badSock(SockT s) {
    return s == INVALID_SOCKET;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SockT = int;
static void closeSock(SockT s) {
    ::close(s);
}
static bool badSock(SockT s) {
    return s < 0;
}
#endif

#include "tbcad/net/socket.h"

namespace tbcad::collabapp {

namespace {
constexpr char kMagic[] = "TBCAD-COLLAB 1";

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
}  // namespace

std::string peerAddressOf(std::intptr_t socket) {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    if (::getpeername(static_cast<SockT>(socket), reinterpret_cast<sockaddr*>(&a), &len) != 0 ||
        a.sin_family != AF_INET)
        return {};
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return ip;
}

std::string localLanAddress() {
    if (!net::initialize())
        return {};
    SockT s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (badSock(s))
        return {};
    // Connecting a UDP socket sends nothing; it only picks the interface the route would use.
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);
    std::string out;
    if (::connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
            out = ip;
            if (out == "0.0.0.0")
                out.clear();
        }
    }
    closeSock(s);
    return out;
}

std::string makeAnnouncement(const std::string& name, int port) {
    std::string clean = name.substr(0, 40);
    for (char& c : clean)
        if (c == '\n')
            c = ' ';
    return std::string(kMagic) + "\n" + clean + "\n" + std::to_string(port);
}

bool parseAnnouncement(const std::string& text, std::string& name, int& port) {
    const std::string head = std::string(kMagic) + "\n";
    if (text.size() > 200 || text.compare(0, head.size(), head) != 0)
        return false;
    const std::size_t nl = text.find('\n', head.size());
    if (nl == std::string::npos)
        return false;
    name = text.substr(head.size(), nl - head.size());
    const std::string p = text.substr(nl + 1);
    if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos)
        return false;
    port = std::stoi(p);
    return port > 0 && port <= 65535 && !name.empty();
}

Announcer::Announcer(std::string name, int port, int udpPort, std::string target) {
    thread_ = std::thread([this, name = std::move(name), port, udpPort,
                           target = std::move(target)] {
        if (!net::initialize())
            return;
        SockT s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (badSock(s))
            return;
        int yes = 1;
        ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<std::uint16_t>(udpPort));
        inet_pton(AF_INET, target.c_str(), &to.sin_addr);
        const std::string msg = makeAnnouncement(name, port);
        int sinceSend = 100;
        while (!stop_) {
            if (++sinceSend >= 20) {  // every two seconds, but quick to stop
                sinceSend = 0;
                ::sendto(s, msg.data(), static_cast<int>(msg.size()), 0,
                         reinterpret_cast<const sockaddr*>(&to), sizeof to);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closeSock(s);
    });
}

Announcer::~Announcer() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

Finder::Finder(int udpPort) {
    if (!net::initialize())
        return;
    SockT s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (badSock(s))
        return;
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = htons(static_cast<std::uint16_t>(udpPort));
    if (::bind(s, reinterpret_cast<const sockaddr*>(&me), sizeof me) != 0) {
        closeSock(s);
        return;
    }
    socklen_t len = sizeof me;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
    port_ = ntohs(me.sin_port);
#ifdef _WIN32
    DWORD tv = 200;
#else
    timeval tv{0, 200000};
#endif
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
    listening_ = true;
    thread_ = std::thread([this, s] {
        char buf[256];
        while (!stop_) {
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            const int n = static_cast<int>(
                ::recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl));
            if (n <= 0)
                continue;
            Announcement a;
            if (!parseAnnouncement(std::string(buf, static_cast<std::size_t>(n)), a.name, a.port))
                continue;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            a.address = ip;
            a.seenMs = nowMs();
            std::lock_guard<std::mutex> g(m_);
            bool found = false;
            for (Announcement& h : heard_)
                if (h.address == a.address && h.port == a.port) {
                    h = a;
                    found = true;
                }
            if (!found && heard_.size() < 32)
                heard_.push_back(a);
        }
        closeSock(s);
    });
}

Finder::~Finder() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

std::vector<Announcement> Finder::hosts() const {
    std::vector<Announcement> out;
    const std::uint64_t now = nowMs();
    std::lock_guard<std::mutex> g(m_);
    for (const Announcement& a : heard_)
        if (now - a.seenMs < 10000)
            out.push_back(a);
    return out;
}

}  // namespace tbcad::collabapp

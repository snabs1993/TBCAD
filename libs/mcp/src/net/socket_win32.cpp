// This is the one file outside libs/platform and apps that includes the Winsock headers (the MCP
// server needs plain TCP sockets on 127.0.0.1; see docs/mcp.md). Link ws2_32.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>

#include "socket.h"

namespace tbcad::mcp::net {

bool initialize() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
}

namespace {

SOCKET sock(Handle h) {
    return static_cast<SOCKET>(h);
}

int waitFor(SOCKET s, short events, int timeoutMs) {
    WSAPOLLFD p{};
    p.fd = s;
    p.events = events;
    return WSAPoll(&p, 1, timeoutMs);
}

Handle bindLoopback(int port, int& boundPort, std::string& error) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = "socket() failed";
        return kInvalid;
    }
    // Refuse to share the port with another program.
    BOOL excl = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl),
                 sizeof excl);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, 16) != 0) {
        error = "bind/listen failed (error " + std::to_string(WSAGetLastError()) + ")";
        ::closesocket(s);
        return kInvalid;
    }
    int len = sizeof addr;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    boundPort = ntohs(addr.sin_port);
    return static_cast<Handle>(s);
}

}  // namespace

Handle listenLoopback(int port, bool fallbackToAny, int& boundPort, std::string& error) {
    if (!initialize()) {
        error = "WSAStartup failed";
        return kInvalid;
    }
    Handle h = bindLoopback(port, boundPort, error);
    if (h == kInvalid && fallbackToAny && port != 0)
        h = bindLoopback(0, boundPort, error);
    return h;
}

Handle acceptWithTimeout(Handle listener, int timeoutMs) {
    if (waitFor(sock(listener), POLLRDNORM, timeoutMs) <= 0)
        return kInvalid;
    SOCKET s = ::accept(sock(listener), nullptr, nullptr);
    if (s == INVALID_SOCKET)
        return kInvalid;
    BOOL one = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    return static_cast<Handle>(s);
}

Handle connectLoopback(int port, int timeoutMs) {
    if (!initialize())
        return kInvalid;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return kInvalid;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    u_long nonBlocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonBlocking);
    int r = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        ::closesocket(s);
        return kInvalid;
    }
    if (r != 0) {
        WSAPOLLFD p{};
        p.fd = s;
        p.events = POLLWRNORM;
        if (WSAPoll(&p, 1, timeoutMs) <= 0 || (p.revents & (POLLERR | POLLHUP)) != 0) {
            ::closesocket(s);
            return kInvalid;
        }
    }
    nonBlocking = 0;
    ::ioctlsocket(s, FIONBIO, &nonBlocking);
    BOOL one = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    return static_cast<Handle>(s);
}

long receive(Handle h, void* buffer, std::size_t size, int timeoutMs) {
    int w = waitFor(sock(h), POLLRDNORM, timeoutMs);
    if (w == 0)
        return kTimeout;
    if (w < 0)
        return kError;
    int n = ::recv(sock(h), static_cast<char*>(buffer), static_cast<int>(size), 0);
    if (n > 0)
        return n;
    if (n == 0)
        return kClosed;
    return kError;
}

bool sendAll(Handle h, const void* data, std::size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        int n = ::send(sock(h), p, static_cast<int>(size), 0);
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

void shutdownSocket(Handle h) {
    ::shutdown(sock(h), SD_BOTH);
}

void closeSocket(Handle h) {
    ::closesocket(sock(h));
}

std::string localAddress(Handle h) {
    sockaddr_in addr{};
    int len = sizeof addr;
    if (::getsockname(sock(h), reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return {};
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf);
    return buf;
}

long currentProcessId() {
    return static_cast<long>(::GetCurrentProcessId());
}

}  // namespace tbcad::mcp::net

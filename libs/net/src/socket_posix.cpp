#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "tbcad/net/socket.h"

namespace tbcad::net {

bool initialize() {
    return true;
}

namespace {

int waitFor(int fd, short events, int timeoutMs) {
    pollfd p{fd, events, 0};
    for (;;) {
        int r = ::poll(&p, 1, timeoutMs);
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

Handle bindTcp(const std::string& bindAddress, int port, int& boundPort, std::string& error) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return kInvalid;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (bindAddress.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        error = "not an IPv4 address: " + bindAddress;
        ::close(fd);
        return kInvalid;
    }
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 16) != 0) {
        error = std::strerror(errno);
        ::close(fd);
        return kInvalid;
    }
    socklen_t len = sizeof addr;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    boundPort = ntohs(addr.sin_port);
    return fd;
}

}  // namespace

Handle listenTcp(const std::string& bindAddress, int port, bool fallbackToAny, int& boundPort,
                 std::string& error) {
    Handle h = bindTcp(bindAddress, port, boundPort, error);
    if (h == kInvalid && fallbackToAny && port != 0)
        h = bindTcp(bindAddress, 0, boundPort, error);
    return h;
}

Handle listenLoopback(int port, bool fallbackToAny, int& boundPort, std::string& error) {
    return listenTcp("127.0.0.1", port, fallbackToAny, boundPort, error);
}

Handle acceptWithTimeout(Handle listener, int timeoutMs) {
    if (waitFor(static_cast<int>(listener), POLLIN, timeoutMs) <= 0)
        return kInvalid;
    int fd = ::accept(static_cast<int>(listener), nullptr, nullptr);
    if (fd < 0)
        return kInvalid;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

namespace {

Handle connectAddr(const sockaddr_in& addr, int timeoutMs) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return kInvalid;
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    if (r != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return kInvalid;
    }
    if (r != 0) {
        if (waitFor(fd, POLLOUT, timeoutMs) <= 0) {
            ::close(fd);
            return kInvalid;
        }
        int err = 0;
        socklen_t len = sizeof err;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            ::close(fd);
            return kInvalid;
        }
    }
    ::fcntl(fd, F_SETFL, flags);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

}  // namespace

Handle connectTcp(const std::string& host, int port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return kInvalid;
    Handle h = kInvalid;
    for (addrinfo* a = res; a && h == kInvalid; a = a->ai_next) {
        sockaddr_in addr = *reinterpret_cast<const sockaddr_in*>(a->ai_addr);
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        h = connectAddr(addr, timeoutMs);
    }
    ::freeaddrinfo(res);
    return h;
}

Handle connectLoopback(int port, int timeoutMs) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    return connectAddr(addr, timeoutMs);
}

long receive(Handle s, void* buffer, std::size_t size, int timeoutMs) {
    int fd = static_cast<int>(s);
    int w = waitFor(fd, POLLIN, timeoutMs);
    if (w == 0)
        return kTimeout;
    if (w < 0)
        return kError;
    ssize_t n = ::recv(fd, buffer, size, 0);
    if (n > 0)
        return static_cast<long>(n);
    if (n == 0)
        return kClosed;
    return kError;
}

bool sendAll(Handle s, const void* data, std::size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = ::send(static_cast<int>(s), p, size, flags);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

void shutdownSocket(Handle s) {
    ::shutdown(static_cast<int>(s), SHUT_RDWR);
}

void closeSocket(Handle s) {
    ::close(static_cast<int>(s));
}

std::string localAddress(Handle s) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(static_cast<int>(s), reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return {};
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf);
    return buf;
}

long currentProcessId() {
    return static_cast<long>(::getpid());
}

}  // namespace tbcad::net

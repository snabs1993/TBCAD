#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "socket.h"

namespace tbcad::mcp::net {

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

Handle bindLoopback(int port, int& boundPort, std::string& error) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return kInvalid;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

Handle listenLoopback(int port, bool fallbackToAny, int& boundPort, std::string& error) {
    Handle h = bindLoopback(port, boundPort, error);
    if (h == kInvalid && fallbackToAny && port != 0)
        h = bindLoopback(0, boundPort, error);
    return h;
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

Handle connectLoopback(int port, int timeoutMs) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return kInvalid;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
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

}  // namespace tbcad::mcp::net

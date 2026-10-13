#pragma once

// Thin loopback-only socket layer. The two implementations (socket_win32.cpp, socket_posix.cpp)
// are the only places that include system socket headers.

#include <cstddef>
#include <cstdint>
#include <string>

namespace tbcad::mcp::net {

using Handle = std::intptr_t;
inline constexpr Handle kInvalid = -1;

inline constexpr long kClosed = 0;    // receive: the peer closed the connection
inline constexpr long kError = -1;    // receive: socket error
inline constexpr long kTimeout = -2;  // receive: nothing arrived in time

// One-time start-up (WSAStartup on Windows). Safe to call repeatedly.
bool initialize();

// Listens on 127.0.0.1 only. `port` 0 picks a free one; `boundPort` receives the real port. When
// `port` is in use and `fallbackToAny` is set, binds port 0 instead.
Handle listenLoopback(int port, bool fallbackToAny, int& boundPort, std::string& error);
// Waits up to timeoutMs for a connection. kInvalid on timeout or when the listener was closed.
Handle acceptWithTimeout(Handle listener, int timeoutMs);
// Connects to 127.0.0.1:port. kInvalid on failure.
Handle connectLoopback(int port, int timeoutMs);

// >0 bytes read, kClosed, kError or kTimeout.
long receive(Handle s, void* buffer, std::size_t size, int timeoutMs);
bool sendAll(Handle s, const void* data, std::size_t size);
void shutdownSocket(Handle s);
void closeSocket(Handle s);

// "127.0.0.1" style text of the address the socket is bound to (tests check it is loopback).
std::string localAddress(Handle s);

long currentProcessId();

}  // namespace tbcad::mcp::net

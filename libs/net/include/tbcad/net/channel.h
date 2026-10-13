#pragma once

// A connected, ordered byte stream (plain TCP or TLS) and length-prefixed frames on top of it.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "tbcad/net/socket.h"

namespace tbcad::net {

class Channel {
public:
    virtual ~Channel() = default;
    // >0 bytes read, kClosed, kError or kTimeout (same codes as net::receive).
    virtual long read(void* buffer, std::size_t size, int timeoutMs) = 0;
    virtual bool writeAll(const void* data, std::size_t size) = 0;
    // Wakes up a thread blocked in read() on another thread; the channel stays open until close().
    virtual void shutdown() = 0;
    virtual void close() = 0;
};

// Wraps a connected socket. The channel owns the handle and closes it.
std::unique_ptr<Channel> makePlainChannel(Handle socket);

inline constexpr std::size_t kMaxFrameBytes = 16u * 1024u * 1024u;

enum class FrameStatus { Ok, Closed, Timeout, Error, TooLarge };

// Sends one frame: a 4-byte little-endian length followed by the payload. False on a socket error
// or a payload over kMaxFrameBytes.
bool sendFrame(Channel& channel, const void* data, std::size_t size);

// Reads frames one at a time. A timeout in the middle of a frame keeps what has arrived, so the
// next poll() carries on from there.
class FrameReader {
public:
    explicit FrameReader(std::size_t maxFrameBytes = kMaxFrameBytes) : max_(maxFrameBytes) {}
    FrameStatus poll(Channel& channel, std::vector<std::uint8_t>& frame, int timeoutMs);

private:
    std::size_t max_;
    std::vector<std::uint8_t> buf_;  // header bytes, then payload bytes
    std::size_t have_ = 0;
    std::size_t need_ = 4;
    bool haveHeader_ = false;
};

}  // namespace tbcad::net

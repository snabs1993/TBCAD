#include "tbcad/net/channel.h"

#include <algorithm>
#include <chrono>

namespace tbcad::net {

namespace {

class PlainChannel final : public Channel {
public:
    explicit PlainChannel(Handle h) : h_(h) {}
    ~PlainChannel() override { close(); }

    long read(void* buffer, std::size_t size, int timeoutMs) override {
        return h_ == kInvalid ? kClosed : receive(h_, buffer, size, timeoutMs);
    }
    bool writeAll(const void* data, std::size_t size) override {
        return h_ != kInvalid && sendAll(h_, data, size);
    }
    void shutdown() override {
        if (h_ != kInvalid)
            shutdownSocket(h_);
    }
    void close() override {
        if (h_ != kInvalid) {
            closeSocket(h_);
            h_ = kInvalid;
        }
    }

private:
    Handle h_;
};

}  // namespace

std::unique_ptr<Channel> makePlainChannel(Handle socket) {
    return std::make_unique<PlainChannel>(socket);
}

bool sendFrame(Channel& channel, const void* data, std::size_t size) {
    if (size > kMaxFrameBytes)
        return false;
    // One write so a frame is never interleaved or split into a header-only TLS record.
    std::vector<std::uint8_t> out(4 + size);
    for (int i = 0; i < 4; ++i)
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((size >> (8 * i)) & 0xff);
    if (size > 0)
        std::copy_n(static_cast<const std::uint8_t*>(data), size, out.begin() + 4);
    return channel.writeAll(out.data(), out.size());
}

FrameStatus FrameReader::poll(Channel& channel, std::vector<std::uint8_t>& frame, int timeoutMs) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (haveHeader_ ? have_ == need_ : false) {
            frame.assign(buf_.begin(), buf_.end());
            buf_.clear();
            have_ = 0;
            need_ = 4;
            haveHeader_ = false;
            return FrameStatus::Ok;
        }
        if (!haveHeader_ && have_ == 4) {
            std::size_t len = 0;
            for (int i = 0; i < 4; ++i)
                len |= static_cast<std::size_t>(buf_[static_cast<std::size_t>(i)]) << (8 * i);
            if (len > max_)
                return FrameStatus::TooLarge;
            buf_.clear();
            have_ = 0;
            need_ = len;
            haveHeader_ = true;
            continue;
        }
        if (buf_.size() < need_)
            buf_.resize(need_);
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        int wait = left.count() > 0 ? static_cast<int>(left.count()) : 0;
        long n = channel.read(buf_.data() + have_, need_ - have_, wait);
        if (n > 0) {
            have_ += static_cast<std::size_t>(n);
        } else if (n == kTimeout) {
            return FrameStatus::Timeout;
        } else if (n == kClosed) {
            return FrameStatus::Closed;
        } else {
            return FrameStatus::Error;
        }
    }
}

}  // namespace tbcad::net

#include "tbcad/collabnet/tcp.h"

#include <algorithm>

namespace tbcad::collabnet {

namespace {
constexpr int kHandshakeTimeoutMs = 5000;
constexpr int kMaxHandshakes = 8;
constexpr std::size_t kMaxConnections = 64;
}  // namespace

// ---- TcpHost -----------------------------------------------------------------------------------

TcpHost::TcpHost(collab::HostSession& session) : session_(&session) {}

TcpHost::~TcpHost() {
    stop();
}

bool TcpHost::start(int port, bool lan, const nettls::Psk& psk, std::string& error) {
    if (running()) {
        error = "already sharing";
        return false;
    }
    if (!net::initialize()) {
        error = "the network could not be started";
        return false;
    }
    int bound = 0;
    listener_ = lan ? net::listenTcp("", port, false, bound, error)
                    : net::listenLoopback(port, false, bound, error);
    if (listener_ == net::kInvalid) {
        if (error.empty())
            error = "could not listen on port " + std::to_string(port);
        return false;
    }
    port_ = bound;
    psk_ = psk;
    stopping_ = false;
    acceptor_ = std::thread([this] { acceptLoop(); });
    return true;
}

void TcpHost::acceptLoop() {
    while (!stopping_) {
        net::Handle h = net::acceptWithTimeout(listener_, 200);
        if (h == net::kInvalid)
            continue;
        if (handshaking_.load() >= kMaxHandshakes) {
            net::closeSocket(h);
            continue;
        }
        ++handshaking_;
        std::lock_guard lock(mutex_);
        helpers_.emplace_back([this, h] { handshake(h); });
    }
}

void TcpHost::handshake(net::Handle socket) {
    std::string error;
    std::unique_ptr<net::Channel> ch = nettls::acceptTls(socket, psk_, kHandshakeTimeoutMs, error);
    if (ch && !stopping_) {
        std::lock_guard lock(mutex_);
        ready_.push_back(std::move(ch));
    }
    --handshaking_;
}

void TcpHost::stop() {
    if (listener_ == net::kInvalid && !acceptor_.joinable())
        return;
    stopping_ = true;
    if (acceptor_.joinable())
        acceptor_.join();
    std::vector<std::thread> helpers;
    {
        std::lock_guard lock(mutex_);
        helpers.swap(helpers_);
    }
    for (std::thread& t : helpers)
        if (t.joinable())
            t.join();
    if (listener_ != net::kInvalid) {
        net::closeSocket(listener_);
        listener_ = net::kInvalid;
    }
    for (auto& c : conns_) {
        session_->disconnect(c->id);
        c->channel->close();
    }
    conns_.clear();
    std::lock_guard lock(mutex_);
    ready_.clear();
    port_ = 0;
}

std::size_t TcpHost::pump(std::uint64_t nowMs) {
    if (!running())
        return 0;
    std::vector<std::unique_ptr<net::Channel>> fresh;
    {
        std::lock_guard lock(mutex_);
        while (!ready_.empty()) {
            fresh.push_back(std::move(ready_.front()));
            ready_.pop_front();
        }
        // Finished helper threads are joined lazily: they are all short, so just sweep when idle.
        if (handshaking_.load() == 0) {
            for (std::thread& t : helpers_)
                if (t.joinable())
                    t.join();
            helpers_.clear();
        }
    }
    for (auto& ch : fresh) {
        if (conns_.size() >= kMaxConnections) {
            ch->close();
            continue;
        }
        auto conn = std::make_unique<Conn>();
        net::Channel* raw = ch.get();
        Conn* c = conn.get();
        conn->channel = std::move(ch);
        conn->id = session_->accept(
            [raw, c](const collab::Frame& f) {
                if (!net::sendFrame(*raw, f.data(), f.size()))
                    c->dead = true;
            },
            [c] { c->dead = true; });
        conns_.push_back(std::move(conn));
    }

    std::size_t handled = 0;
    for (std::size_t i = 0; i < conns_.size(); ++i) {
        Conn& c = *conns_[i];
        while (!c.dead) {
            std::vector<std::uint8_t> frame;
            net::FrameStatus st = c.reader.poll(*c.channel, frame, 0);
            if (st == net::FrameStatus::Ok) {
                session_->receive(c.id, frame, nowMs);
                ++handled;
                continue;
            }
            if (st != net::FrameStatus::Timeout)
                c.dead = true;
            break;
        }
    }
    // Remove the dead; the session learns about connections it did not close itself.
    for (auto it = conns_.begin(); it != conns_.end();) {
        if ((*it)->dead) {
            session_->disconnect((*it)->id);
            (*it)->channel->close();
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
    return handled;
}

// ---- TcpGuest ----------------------------------------------------------------------------------

TcpGuest::~TcpGuest() {
    close();
}

collab::SendFn TcpGuest::sender() {
    return [this](const collab::Frame& f) {
        if (channel_ && !net::sendFrame(*channel_, f.data(), f.size())) {
            channel_->close();
            channel_.reset();
            if (session_)
                session_->disconnected();
        }
    };
}

bool TcpGuest::connect(const std::string& host, int port, const nettls::Psk& psk,
                       const std::string& name, int timeoutMs, std::string& error) {
    if (channel_ || !session_) {
        error = channel_ ? "already connected" : "no session attached";
        return false;
    }
    if (!net::initialize()) {
        error = "the network could not be started";
        return false;
    }
    net::Handle h = net::connectTcp(host, port, timeoutMs);
    if (h == net::kInvalid) {
        error = "could not reach " + host + ":" + std::to_string(port);
        return false;
    }
    channel_ = nettls::connectTls(h, psk, timeoutMs, error);
    if (!channel_) {
        if (error.empty())
            error = "the secure connection failed (wrong password?)";
        return false;
    }
    session_->join(name);
    return true;
}

std::size_t TcpGuest::pump() {
    if (!channel_ || !session_)
        return 0;
    std::size_t handled = 0;
    while (channel_) {
        std::vector<std::uint8_t> frame;
        net::FrameStatus st = reader_.poll(*channel_, frame, 0);
        if (st == net::FrameStatus::Ok) {
            session_->receive(frame);
            ++handled;
            continue;
        }
        if (st != net::FrameStatus::Timeout) {
            channel_->close();
            channel_.reset();
            session_->disconnected();
        }
        break;
    }
    return handled;
}

void TcpGuest::close() {
    if (channel_) {
        channel_->close();
        channel_.reset();
    }
}

}  // namespace tbcad::collabnet

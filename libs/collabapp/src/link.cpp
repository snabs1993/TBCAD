#include "link.h"

#include <chrono>
#include <utility>

#include "discovery.h"

namespace tbcad::collabapp {

namespace {
constexpr char kIdentity[] = "tbcad-collab-1";
constexpr int kHandshakeMs = 10000;
}  // namespace

nettls::Psk sessionPsk(const std::string& password) {
    return nettls::derivePsk(password, kIdentity);
}

std::shared_ptr<Link> Link::dial(const std::string& host, int port, const std::string& password,
                                 std::function<void()> wake) {
    std::shared_ptr<Link> link(new Link());
    link->wake_ = std::move(wake);
    link->thread_ = std::thread([l = link.get(), host, port, password] {
        l->run([&](std::string& err) -> std::unique_ptr<net::Channel> {
            if (!net::initialize()) {
                err = "network start-up failed";
                return nullptr;
            }
            const net::Handle s = net::connectTcp(host, port, kHandshakeMs);
            if (s == net::kInvalid) {
                err = "cannot reach " + host + ":" + std::to_string(port);
                return nullptr;
            }
            return nettls::connectTls(s, sessionPsk(password), kHandshakeMs, err);
        });
    });
    return link;
}

std::shared_ptr<Link> Link::accepted(net::Handle socket, nettls::Psk psk,
                                     std::function<void()> wake) {
    std::shared_ptr<Link> link(new Link());
    link->wake_ = std::move(wake);
    link->thread_ = std::thread([l = link.get(), socket, psk = std::move(psk)] {
        l->run([&](std::string& err) { return nettls::acceptTls(socket, psk, kHandshakeMs, err); });
    });
    return link;
}

void Link::run(std::function<std::unique_ptr<net::Channel>(std::string&)> connect) {
    std::string err;
    std::unique_ptr<net::Channel> ch = connect(err);
    if (!ch) {
        std::lock_guard<std::mutex> g(m_);
        error_ = err.empty() ? "connection failed" : err;
    } else {
        open_ = true;
        net::FrameReader reader;
        Frame frame;
        std::string why;
        bool flushOnly = false;
        while (true) {
            if (!flushOnly) {
                const net::FrameStatus st = reader.poll(*ch, frame, 10);
                if (st == net::FrameStatus::Ok) {
                    {
                        std::lock_guard<std::mutex> g(m_);
                        in_.push_back(std::move(frame));
                    }
                    frame = {};
                    if (wake_)
                        wake_();
                } else if (st != net::FrameStatus::Timeout) {
                    why = st == net::FrameStatus::Closed ? "the other side closed the connection"
                                                         : "connection lost";
                    break;
                }
            }
            std::deque<Frame> batch;
            {
                std::lock_guard<std::mutex> g(m_);
                batch.swap(out_);
            }
            bool failed = false;
            for (const Frame& f : batch)
                if (!net::sendFrame(*ch, f.data(), f.size())) {
                    failed = true;
                    break;
                }
            if (failed) {
                why = "connection lost";
                break;
            }
            if (stop_) {
                // Frames queued before close() have just been written.
                std::lock_guard<std::mutex> g(m_);
                if (out_.empty())
                    break;
                flushOnly = true;
            }
        }
        std::lock_guard<std::mutex> g(m_);
        error_ = why;
        ch->close();
    }
    open_ = false;
    done_ = true;
    if (wake_)
        wake_();
}

Link::~Link() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

void Link::send(Frame frame) {
    std::lock_guard<std::mutex> g(m_);
    out_.push_back(std::move(frame));
}

bool Link::poll(Frame& out) {
    std::lock_guard<std::mutex> g(m_);
    if (in_.empty())
        return false;
    out = std::move(in_.front());
    in_.pop_front();
    return true;
}

void Link::close() {
    stop_ = true;
}

std::string Link::error() const {
    std::lock_guard<std::mutex> g(m_);
    return error_;
}

std::unique_ptr<Listener> Listener::start(int port, const std::string& password,
                                          std::function<void()> wake, std::string& error) {
    if (!net::initialize()) {
        error = "network start-up failed";
        return nullptr;
    }
    std::unique_ptr<Listener> l(new Listener());
    l->wake_ = std::move(wake);
    l->psk_ = sessionPsk(password);
    int bound = 0;
    l->socket_ = net::listenTcp("", port, false, bound, error);
    if (l->socket_ == net::kInvalid) {
        if (error.empty())
            error = "cannot listen on port " + std::to_string(port);
        return nullptr;
    }
    l->port_ = bound;
    l->thread_ = std::thread([p = l.get()] {
        while (!p->stop_) {
            const net::Handle s = net::acceptWithTimeout(p->socket_, 100);
            if (s == net::kInvalid)
                continue;
            std::string from = peerAddressOf(s);
            auto link = Link::accepted(s, p->psk_, p->wake_);
            link->setPeerAddress(std::move(from));
            {
                std::lock_guard<std::mutex> g(p->m_);
                p->fresh_.push_back(std::move(link));
            }
            if (p->wake_)
                p->wake_();
        }
    });
    return l;
}

Listener::~Listener() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    if (socket_ != net::kInvalid)
        net::closeSocket(socket_);
}

std::vector<std::shared_ptr<Link>> Listener::takeNew() {
    std::lock_guard<std::mutex> g(m_);
    return std::exchange(fresh_, {});
}

}  // namespace tbcad::collabapp

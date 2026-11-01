#pragma once

// One encrypted connection to another TBCAD, run on its own thread. The main thread only queues
// frames to send and polls frames that arrived, so it never blocks on the network.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tbcad/net/channel.h"
#include "tbcad/nettls/tls.h"

namespace tbcad::collabapp {

using Frame = std::vector<std::uint8_t>;

// Both sides derive the key from the session password; the identity is fixed so that a guest
// needs only the password.
nettls::Psk sessionPsk(const std::string& password);

class Link {
public:
    // Connects to host:port and runs the TLS handshake in the background. error() says why when
    // it failed.
    static std::shared_ptr<Link> dial(const std::string& host, int port, const std::string& password,
                                      std::function<void()> wake);
    // Takes over a socket accepted by a Listener and runs the server side of the handshake.
    static std::shared_ptr<Link> accepted(net::Handle socket, nettls::Psk psk,
                                          std::function<void()> wake);
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    void send(Frame frame);        // any thread
    bool poll(Frame& out);         // next frame that arrived, oldest first
    void close();                  // stop soon; frames already queued for sending are flushed
    bool open() const { return open_; }
    // The thread has ended (connection lost, closed, or never made). Frames that arrived before
    // it ended can still be polled.
    bool finished() const { return done_; }
    std::string error() const;
    // Dotted IPv4 of the other side, for a link a Listener accepted (empty otherwise).
    const std::string& peerAddress() const { return peerAddress_; }
    void setPeerAddress(std::string a) { peerAddress_ = std::move(a); }

private:
    Link() = default;
    void run(std::function<std::unique_ptr<net::Channel>(std::string&)> connect);

    std::function<void()> wake_{};
    std::string peerAddress_{};
    mutable std::mutex m_{};
    std::deque<Frame> in_{}, out_{};
    std::string error_{};
    std::atomic<bool> open_{false}, done_{false}, stop_{false};
    std::thread thread_{};
};

// Accepts connections on a port and makes a Link of each.
class Listener {
public:
    // port 0 picks a free one. Null with `error` set when the port cannot be used.
    static std::unique_ptr<Listener> start(int port, const std::string& password,
                                           std::function<void()> wake, std::string& error);
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    int port() const { return port_; }
    std::vector<std::shared_ptr<Link>> takeNew();  // main thread

private:
    Listener() = default;

    std::function<void()> wake_{};
    nettls::Psk psk_{};
    net::Handle socket_ = net::kInvalid;
    int port_ = 0;
    std::mutex m_{};
    std::vector<std::shared_ptr<Link>> fresh_{};
    std::atomic<bool> stop_{false};
    std::thread thread_{};
};

}  // namespace tbcad::collabapp

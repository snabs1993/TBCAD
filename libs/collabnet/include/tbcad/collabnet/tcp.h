#pragma once

// A host and guests of a collaboration session over TCP with TLS 1.3 and a key derived from the
// session password (libs/nettls). Both classes are driven by pump(), which the application calls
// often (every frame of the UI) on the thread that owns the document; nothing else touches the
// session. The only helper threads are the TLS handshakes of arriving connections, so a slow or
// hostile client can not stall the program.
//
// Limits of this first version: a drawing larger than net::kMaxFrameBytes (16 MB as a .tbcad
// image) can not be sent as a join snapshot, and a guest that stops reading blocks the host's
// pump for as long as the socket buffer is full.

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tbcad/collab/session.h"
#include "tbcad/net/channel.h"
#include "tbcad/nettls/tls.h"

namespace tbcad::collabnet {

class TcpHost {
public:
    explicit TcpHost(collab::HostSession& session);
    ~TcpHost();
    TcpHost(const TcpHost&) = delete;
    TcpHost& operator=(const TcpHost&) = delete;

    // Listens on `port` (0 = pick one). `lan` false: this computer only (127.0.0.1); true: every
    // network interface. Returns the port, or an empty result with `error` set.
    bool start(int port, bool lan, const nettls::Psk& psk, std::string& error);
    void stop();
    bool running() const { return listener_ != net::kInvalid; }
    int port() const { return port_; }
    std::size_t connectionCount() const { return conns_.size(); }

    // Takes in connections whose handshake finished and reads whatever has arrived. Returns the
    // number of frames handled.
    std::size_t pump(std::uint64_t nowMs);

private:
    struct Conn {
        std::unique_ptr<net::Channel> channel{};
        net::FrameReader reader{};
        collab::ConnId id = 0;
        bool dead = false;
    };
    void acceptLoop();
    void handshake(net::Handle socket);

    collab::HostSession* session_;
    net::Handle listener_ = net::kInvalid;
    int port_ = 0;
    nettls::Psk psk_{};
    std::thread acceptor_{};
    std::atomic<bool> stopping_{false};
    std::atomic<int> handshaking_{0};
    std::mutex mutex_{};
    std::vector<std::thread> helpers_{};
    std::deque<std::unique_ptr<net::Channel>> ready_{};
    std::vector<std::unique_ptr<Conn>> conns_{};
};

class TcpGuest {
public:
    TcpGuest() = default;
    ~TcpGuest();
    TcpGuest(const TcpGuest&) = delete;
    TcpGuest& operator=(const TcpGuest&) = delete;

    // The function a GuestSession is built with: it sends over this connection.
    //   TcpGuest tcp;  GuestSession guest(tcp.sender());  tcp.attach(guest);
    collab::SendFn sender();
    void attach(collab::GuestSession& session) { session_ = &session; }

    // Connects, runs the TLS handshake (blocking, up to timeoutMs) and asks to join as `name`.
    // False with `error` set when the host can not be reached or the password is wrong.
    bool connect(const std::string& host, int port, const nettls::Psk& psk, const std::string& name,
                 int timeoutMs, std::string& error);
    // Reads what has arrived and hands it to the session. Returns the frames handled. When the
    // connection is gone the session is told.
    std::size_t pump();
    void close();
    bool connected() const { return channel_ != nullptr; }

private:
    collab::GuestSession* session_ = nullptr;
    std::unique_ptr<net::Channel> channel_{};
    net::FrameReader reader_{};
};

}  // namespace tbcad::collabnet

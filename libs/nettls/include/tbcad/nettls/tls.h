#pragma once

// TLS 1.3 with a pre-shared key (psk_dhe_ke: forward secret) over a connected socket, as a
// net::Channel. Both sides know a key derived from the session password; a peer with the wrong
// password fails the handshake. Built on Mbed TLS.

#include <memory>
#include <string>
#include <vector>

#include "tbcad/net/argon2.h"
#include "tbcad/net/channel.h"

namespace tbcad::nettls {

struct Psk {
    std::vector<std::uint8_t> key;  // 16..64 bytes
    std::string identity;           // public label, e.g. the session name
};

// Password -> key: Argon2id over the password with a salt taken from `identity`.
Psk derivePsk(const std::string& password, const std::string& identity,
              const net::Argon2Params& params = {});

// Runs the TLS handshake on `socket` (blocking, up to timeoutMs). On success the returned channel
// owns the socket. On failure the socket is closed, nullptr is returned and `error` says why.
std::unique_ptr<net::Channel> connectTls(net::Handle socket, const Psk& psk, int timeoutMs,
                                         std::string& error);
std::unique_ptr<net::Channel> acceptTls(net::Handle socket, const Psk& psk, int timeoutMs,
                                        std::string& error);

}  // namespace tbcad::nettls

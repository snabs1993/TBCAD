#pragma once

// Argon2id (RFC 9106, version 0x13) and BLAKE2b (RFC 7693), written from the RFCs and checked
// against reference vectors in the tests. Used to turn a session password into a TLS pre-shared
// key, so guessing passwords offline against a captured handshake is expensive.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tbcad::net {

// BLAKE2b with an unkeyed digest of `outLen` bytes (1..64).
std::vector<std::uint8_t> blake2b(const void* data, std::size_t size, std::size_t outLen);

struct Argon2Params {
    std::uint32_t timeCost = 2;       // passes
    std::uint32_t memoryKiB = 19456;  // 19 MiB (OWASP interactive minimum for Argon2id, t=2, p=1)
    std::uint32_t lanes = 1;
    std::uint32_t tagLength = 32;
};

// Argon2id tag of `password` with `salt` (at least 8 bytes). Returns an empty vector when the
// parameters are invalid.
std::vector<std::uint8_t> argon2id(const std::string& password,
                                   const std::vector<std::uint8_t>& salt,
                                   const Argon2Params& params = {});

}  // namespace tbcad::net

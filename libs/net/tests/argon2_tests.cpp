#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "tbcad/net/argon2.h"

using namespace tbcad::net;

namespace {

std::string hex(const std::vector<std::uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : v) {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 15]);
    }
    return s;
}

std::vector<std::uint8_t> bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

// Expected values come from the reference implementation (argon2-cffi / hashlib).
std::string argon(const std::string& pw, const std::string& salt, std::uint32_t t, std::uint32_t m,
                  std::uint32_t p, std::uint32_t len) {
    Argon2Params ap;
    ap.timeCost = t;
    ap.memoryKiB = m;
    ap.lanes = p;
    ap.tagLength = len;
    return hex(argon2id(pw, bytes(salt), ap));
}

}  // namespace

TEST_CASE("blake2b matches the reference digests, also across block boundaries") {
    std::string in;
    for (int i = 0; i < 50; ++i)
        in += "abc";
    CHECK(hex(blake2b(in.data(), in.size(), 1)) == "91");
    CHECK(hex(blake2b(in.data(), in.size(), 32)) ==
          "42cd60eb124cd83951938db4e9d9080cbdfda078f6fb58b151dc6d967415533e");
    CHECK(hex(blake2b(in.data(), in.size(), 64)) ==
          "b01fd1a6a60fcff16e5d1bc2f33912790475bea0339ccd6725ca271129f4db5d45a0934c979b3be1353160"
          "f8130c4eb32c01bd80684d8a45fb0d72033ad433ca");
    CHECK(blake2b(in.data(), in.size(), 0).empty());
    CHECK(blake2b(in.data(), in.size(), 65).empty());
}

TEST_CASE("argon2id matches the reference implementation") {
    CHECK(argon("password", "somesalt", 2, 64, 1, 32) ==
          "16a1a498734609dd01456da406de9f3d9da93e6c86c300a12fc1465214ce4922");
    // several lanes and passes (data-independent and data-dependent addressing)
    CHECK(argon("pw", "saltsaltsalt", 3, 256, 4, 32) ==
          "b567805c5433e3c597465240b30313f1116d519161e9fe0b3980c373a94c694e");
    // tag longer than one BLAKE2b digest
    CHECK(argon("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                "12345678", 1, 32, 2, 64) ==
          "0bf5373c52ab92c97df6d0c9c5f2c1fd15de790a3e2d5c5ec85363b86861a90c657cb50e100ae598f2"
          "79b8353a114a8df2f566109c68ab8aa1bdd164e5b0fba1");
    CHECK(argon("p", "abcdefgh", 4, 128, 3, 100) ==
          "a3caf7271a7124655a91796d8181aceaf6aa01bfeb8e5daaa44a3b26083402e282743d96ac13a71a44ccc3"
          "7812f84603c6b6b012f289c108199706ff432e0f67f09989f24da662fe618325c6a7d28ea40a878db58f5f"
          "f81cc2547c90f20a259920cb3242");
}

TEST_CASE("argon2id default parameters (19 MiB, t=2) match the reference") {
    CHECK(argon("", "abcdefgh", 2, 19456, 1, 32) ==
          "a3ded579f5827c110663664918b2ed08949a6ba5fa8b17e7b4d1b8d625f90aeb");
}

TEST_CASE("argon2id rejects unusable parameters") {
    Argon2Params p;
    CHECK(argon2id("x", bytes("short"), p).empty());  // salt under 8 bytes
    p.lanes = 0;
    CHECK(argon2id("x", bytes("saltsalt"), p).empty());
    p = {};
    p.memoryKiB = 4;
    CHECK(argon2id("x", bytes("saltsalt"), p).empty());
    p = {};
    p.timeCost = 0;
    CHECK(argon2id("x", bytes("saltsalt"), p).empty());
}

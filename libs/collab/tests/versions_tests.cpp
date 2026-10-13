#include <random>

#include "helpers.h"
#include "tbcad/collab/snapshot.h"
#include "tbcad/collab/versions.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collab::testing;

TEST_CASE("versions: a map forgets version 0 and keeps the rest") {
    VersionMap m;
    CHECK(m.get({Space::Entity, 5}) == 0);
    m.set({Space::Entity, 5}, 3);
    m.set({Space::Layer, 5}, 4);
    CHECK(m.get({Space::Entity, 5}) == 3);
    CHECK(m.get({Space::Layer, 5}) == 4);
    m.set({Space::Entity, 5}, 0);
    CHECK(m.size() == 1);
}

TEST_CASE("versions: encode and decode round trip") {
    VersionMap m;
    for (std::uint64_t i = 1; i <= 1000; i += 3)
        m.set({Space::Entity, i * 7}, i + 1);
    m.set({Space::Layer, 2}, 9);
    m.set({Space::Header, 0}, 12);
    m.set({Space::Order, 14}, 15);
    auto bytes = encodeVersions(m, 77);
    auto back = decodeVersions(bytes);
    REQUIRE(back.ok());
    CHECK(back.value().versions == m);
    CHECK(back.value().sequence == 77);
}

TEST_CASE("versions: damaged records are refused") {
    VersionMap m;
    m.set({Space::Entity, 1}, 1);
    m.set({Space::Entity, 9}, 2);
    auto bytes = encodeVersions(m, 3);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        CHECK_FALSE(decodeVersions(std::span<const std::uint8_t>(bytes.data(), n)).ok());
    CHECK_FALSE(decodeVersions(bytes, 1).ok());  // more entries than allowed
    auto wrong = bytes;
    wrong[0] = 'X';
    CHECK_FALSE(decodeVersions(wrong).ok());
    std::mt19937_64 rng(7);
    for (int i = 0; i < 2000; ++i) {
        auto b = bytes;
        b[rng() % b.size()] ^= static_cast<std::uint8_t>(1 + rng() % 255);
        (void)decodeVersions(b);  // must not crash
    }
}

TEST_CASE("versions: they travel in the file and an older reader still opens it") {
    Document doc;
    {
        Transaction tx = doc.begin("x");
        (void)must(tx.add(line(kLayerZero, {0, 0}, {1, 1})));
        tx.commit();
    }
    VersionMap m;
    m.set({Space::Entity, 1}, 5);
    auto bytes = must(saveSnapshot(doc, m, 5));
    Snapshot s = must(loadSnapshot(bytes));
    CHECK(s.versions == m);
    CHECK(s.sequence == 5);
    CHECK(canonicalDump(*s.document).find("LINE") != std::string::npos);

    // A plain file has no versions.
    Document plain;
    auto plainBytes = must(io::writeTbcadToMemory(plain));
    Snapshot p = must(loadSnapshot(plainBytes));
    CHECK(p.versions.empty());
    CHECK(p.sequence == 0);
}

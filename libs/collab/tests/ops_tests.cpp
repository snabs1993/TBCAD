#include <random>

#include "helpers.h"
#include "tbcad/collab/ops.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collab::testing;

namespace {

ChangeSet commitSample(Document& doc, EntityId* modified = nullptr) {
    Transaction tx = doc.begin("sample");
    LayerId layer = must(tx.addLayer({.name = "Walls ผนัง"}));
    EntityId a = must(tx.add(line(layer, {0, 0}, {10, 0})));
    (void)must(tx.add(line(layer, {0, 1}, {10, 1})));
    if (modified)
        *modified = a;
    return tx.commit();
}

}  // namespace

TEST_CASE("ops: bases cover every touched thing with its version and existence") {
    Document doc;
    EntityId first;
    ChangeSet cs = commitSample(doc, &first);
    VersionMap seen;
    Op op = makeOp(4, 1, cs, seen);
    CHECK(op.author == 4);
    CHECK(op.clientSeq == 1);
    REQUIRE(op.bases.size() == 3);  // two entities and one layer
    for (const Base& b : op.bases) {
        CHECK(b.version == 0);
        CHECK_FALSE(b.existed);  // all new
    }

    // A later edit of the entity and the layer.
    seen.set({Space::Entity, first.value}, 7);
    seen.set({Space::Layer, 2}, 5);
    Transaction tx = doc.begin("move");
    REQUIRE(tx.modify(first, [](Entity& e) { std::get<LineData>(e.data).b = {1, 1}; }).ok());
    REQUIRE(tx.modifyLayer(LayerId{2}, [](Layer& l) { l.locked = true; }).ok());
    ChangeSet cs2 = tx.commit();
    Op op2 = makeOp(4, 2, cs2, seen);
    REQUIRE(op2.bases.size() == 2);
    for (const Base& b : op2.bases) {
        CHECK(b.existed);
        CHECK(b.version == (b.key.space == Space::Entity ? 7u : 5u));
    }
}

TEST_CASE("ops: an op survives encoding and decoding") {
    Document doc;
    ChangeSet cs = commitSample(doc);
    VersionMap seen;
    seen.set({Space::Layer, 2}, 3);
    Op op = makeOp(9, 12, cs, seen);
    auto bytes = encodeOp(op);
    Op back = must(decodeOp(bytes));
    CHECK(back.author == 9);
    CHECK(back.clientSeq == 12);
    CHECK(back.bases == op.bases);
    CHECK(encodeOp(back) == bytes);

    Accepted a{42, 9, 12, op.delta};
    auto ab = encodeAccepted(a);
    Accepted ar = must(decodeAccepted(ab));
    CHECK(ar.seq == 42);
    CHECK(ar.author == 9);
    CHECK(ar.clientSeq == 12);
    CHECK(encodeAccepted(ar) == ab);
}

TEST_CASE("ops: bases must be in order and a broadcast must have a sequence") {
    Document doc;
    ChangeSet cs = commitSample(doc);
    Op op = makeOp(1, 1, cs, {});
    std::swap(op.bases.front(), op.bases.back());
    CHECK_FALSE(decodeOp(encodeOp(op)).ok());

    Accepted a{0, 1, 1, makeOp(1, 1, cs, {}).delta};
    CHECK_FALSE(decodeAccepted(encodeAccepted(a)).ok());
}

TEST_CASE("ops: damaged input never crashes the decoders") {
    Document doc;
    ChangeSet cs = commitSample(doc);
    Op op = makeOp(3, 1, cs, {});
    auto opBytes = encodeOp(op);
    auto accBytes = encodeAccepted({5, 3, 1, op.delta});

    for (std::size_t n = 0; n < opBytes.size(); ++n)
        CHECK_FALSE(decodeOp(std::span<const std::uint8_t>(opBytes.data(), n)).ok());
    for (std::size_t n = 0; n < accBytes.size(); ++n)
        CHECK_FALSE(decodeAccepted(std::span<const std::uint8_t>(accBytes.data(), n)).ok());

    std::mt19937_64 rng(2024);
    std::size_t ok = 0;
    for (int i = 0; i < 6000; ++i) {
        auto b = (i % 2) ? opBytes : accBytes;
        for (int k = 0; k < 1 + int(rng() % 3); ++k)
            b[rng() % b.size()] ^= static_cast<std::uint8_t>(1 + rng() % 255);
        if (rng() % 5 == 0)
            b.resize(rng() % b.size());
        if (i % 2) {
            ok += decodeOp(b).ok() ? 1 : 0;
        } else {
            ok += decodeAccepted(b).ok() ? 1 : 0;
        }
    }
    INFO("decoded " << ok << " of 6000 damaged records");
    CHECK(ok < 6000);

    // Hostile sizes: a huge claimed count fails fast, nothing is allocated for it.
    std::vector<std::uint8_t> huge = {'T', 'B', 'O', 'P', 1, 1, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
    CHECK_FALSE(decodeOp(huge).ok());
    io::DeltaLimits tight;
    tight.maxBytes = 16;
    CHECK_FALSE(decodeOp(opBytes, tight).ok());
}

TEST_CASE("ops: rebasing moves pending bases to the accepted sequence") {
    Document doc;
    EntityId e;
    ChangeSet first = commitSample(doc, &e);
    Op firstOp = makeOp(1, 1, first, {});

    // The second edit was made on top of the first (before the host answered).
    Transaction tx = doc.begin("move");
    REQUIRE(tx.modify(e, [](Entity& x) { std::get<LineData>(x.data).b = {5, 5}; }).ok());
    ChangeSet second = tx.commit();
    std::vector<Op> pending{makeOp(1, 2, second, {})};
    REQUIRE(pending[0].bases.size() == 1);
    CHECK(pending[0].bases[0].version == 0);
    CHECK(pending[0].bases[0].existed);  // it exists in the author's document

    rebasePending(pending, Accepted{30, 1, 1, firstOp.delta});
    CHECK(pending[0].bases[0].version == 30);
    CHECK(pending[0].bases[0].existed);
}

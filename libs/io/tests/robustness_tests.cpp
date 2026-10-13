#include <doctest/doctest.h>

#include <random>

#include "helpers.h"
#include "tbcad/core/dump.h"

using namespace tbcad;
using namespace tbcad::testing;

namespace {

// A small document (a few hundred bytes to a few KB) for exhaustive truncation.
std::unique_ptr<Document> smallDocument() {
    auto doc = std::make_unique<Document>();
    Transaction tx = doc->begin("t");
    auto layer = tx.addLayer({.name = "Walls ผนัง", .color = Color::index(3)});
    auto child = tx.addLayer({.name = "sub", .parent = layer.value(), .color = Color::byParent()});
    auto block = tx.addBlock({.name = "B", .base = {1, 2}});
    for (int i = 0; i < 4; ++i) {
        Entity e;
        e.common.layer = child.value();
        e.data = LineData{{double(i), 0}, {double(i), 1}};
        (void)tx.add(std::move(e), i == 3 ? block.value() : BlockId{});
    }
    Entity t;
    t.common.layer = layer.value();
    t.data = TextData{.position = {1, 1}, .height = 2, .utf8 = "สวัสดี"};
    (void)tx.add(std::move(t));
    Entity c;
    c.common.layer = kLayerZero;
    c.data = CircleData{{0, 0}, 5};
    (void)tx.add(std::move(c));
    (void)tx.modifyHeader([](Header& h) { h.createdBy = "x"; });
    tx.commit();
    return doc;
}

}  // namespace

TEST_CASE("truncated files: every cut of a small file is rejected cleanly") {
    for (bool compress : {false, true}) {
        io::WriteOptions o;
        o.compress = compress;
        auto bytes = writeOrFail(*smallDocument(), o);
        INFO("file size " << bytes.size());
        for (std::size_t n = 0; n < bytes.size(); ++n) {
            io::IoReport report;
            std::span<const std::uint8_t> cut(bytes.data(), n);
            auto r = io::readTbcad(cut, report);
            CHECK_FALSE(r.ok());
            // Also without the CRC check: still no crash.
            io::ReadOptions noCrc;
            noCrc.verifyCrc = false;
            auto r2 = io::readTbcad(cut, report, noCrc);
            CHECK_FALSE(r2.ok());
        }
        io::IoReport report;
        CHECK(io::readTbcad(bytes, report).ok());
    }
}

TEST_CASE("10,000 random mutations never crash (run under ASan/UBSan in CI)") {
    auto doc = makeRichDocument();
    std::mt19937 rng(12345);
    for (bool compress : {false, true}) {
        io::WriteOptions o;
        o.compress = compress;
        auto original = writeOrFail(*doc, o);
        int accepted = 0, rejected = 0;
        for (int i = 0; i < 5000; ++i) {
            auto bytes = original;
            int edits = 1 + static_cast<int>(rng() % 4);
            for (int k = 0; k < edits; ++k) {
                std::size_t at = rng() % bytes.size();
                switch (rng() % 5) {
                    case 0:
                        bytes[at] = static_cast<std::uint8_t>(rng());
                        break;
                    case 1:
                        bytes[at] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
                        break;
                    case 2:
                        bytes[at] = 0xFF;
                        break;
                    case 3:
                        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at));
                        break;
                    default:
                        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                     static_cast<std::uint8_t>(rng()));
                }
                if (bytes.empty())
                    bytes.push_back(0);
            }
            io::IoReport report;
            io::ReadOptions opt;
            opt.verifyCrc = (i % 5 == 0);  // mostly skip the CRC so the parsers see the damage
            auto r = io::readTbcad(bytes, report, opt);
            if (r.ok()) {
                ++accepted;
                (void)canonicalDump(*r.value());  // a document that loads must be usable
            } else {
                ++rejected;
                CHECK_FALSE(r.error().message.empty());
            }
        }
        INFO("compress=" << compress << " accepted=" << accepted << " rejected=" << rejected);
        CHECK(rejected > 0);
    }
}

TEST_CASE("a CRC mismatch is reported unless checking is switched off") {
    auto bytes = writeOrFail(*smallDocument(), {.compress = false});
    // Flip a byte inside the first chunk's payload (after the 22 byte file header and the
    // 16 byte chunk header).
    bytes[22 + 16 + 2] ^= 0x55;
    io::IoReport report;
    auto r = io::readTbcad(bytes, report);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("checksum") != std::string::npos);
}

TEST_CASE("absurd sizes cannot make the reader allocate or spin") {
    auto bytes = writeOrFail(*smallDocument(), {.compress = false});

    SUBCASE("chunk length larger than the file") {
        auto b = bytes;
        for (int i = 0; i < 8; ++i)
            b[22 + 8 + static_cast<std::size_t>(i)] = 0xFF;
        io::IoReport report;
        CHECK_FALSE(io::readTbcad(b, report).ok());
    }
    SUBCASE("deflated chunk that claims a huge uncompressed size") {
        // payload: u64 rawLen = 2^40 followed by a few bytes
        std::vector<std::uint8_t> payload{0, 0, 0, 0, 0, 1, 0, 0, 0x78, 0x9C, 3, 0, 0, 0, 0, 1};
        auto withBad = spliceChunkBeforeEnd(bytes, "ENTS", 1, payload);
        io::IoReport report;
        auto r = io::readTbcad(withBad, report);
        REQUIRE_FALSE(r.ok());
    }
    SUBCASE("element count larger than the data") {
        std::vector<std::uint8_t> payload{0xFF, 0xFF, 0xFF, 0xFF, 0x0F};  // varint ~4 billion
        auto withBad = spliceChunkBeforeEnd(bytes, "ENTS", 0, payload);
        io::IoReport report;
        CHECK_FALSE(io::readTbcad(withBad, report).ok());
    }
    SUBCASE("entity id far beyond the previous one") {
        // One ENTS chunk with a single record whose id is 2^40: a gap this big is refused.
        std::vector<std::uint8_t> payload;
        payload.push_back(1);  // one record
        auto le = [&](std::uint64_t v, int n) {
            for (int i = 0; i < n; ++i)
                payload.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        le(4, 2);  // kind Point
        le(1, 2);  // record version
        std::vector<std::uint8_t> body{0x80, 0x80, 0x80,        0x80,
                                       0x80, 0x20, 0 /*flags*/, 1 /*layer*/};
        body.resize(body.size() + 16, 0);
        le(body.size(), 4);
        payload.insert(payload.end(), body.begin(), body.end());
        // Replace the file's own ENTS chunk by dropping it: build from an empty document.
        Document empty;
        auto emptyBytes = writeOrFail(empty, {.compress = false});
        auto withBad = spliceChunkBeforeEnd(emptyBytes, "ENTS", 0, payload);
        io::IoReport report;
        auto r = io::readTbcad(withBad, report);
        REQUIRE_FALSE(r.ok());
        INFO(r.error().message);
        CHECK(r.error().message.find("gap") != std::string::npos);
    }
}

TEST_CASE("an invalid entity is skipped with a warning, the rest of the file loads") {
    // A circle with a negative radius is refused by the document; the other entity survives.
    Document doc;
    {
        Transaction tx = doc.begin("t");
        Entity a;
        a.common.layer = kLayerZero;
        a.data = CircleData{{0, 0}, 5};
        (void)tx.add(std::move(a));
        Entity b;
        b.common.layer = kLayerZero;
        b.data = LineData{{0, 0}, {1, 1}};
        (void)tx.add(std::move(b));
        tx.commit();
    }
    auto bytes = writeOrFail(doc, {.compress = false});
    // Find the circle's radius (f64 5.0) and make it -5.0 by flipping the sign bit.
    const std::uint8_t five[8] = {0, 0, 0, 0, 0, 0, 0x14, 0x40};
    bool patched = false;
    for (std::size_t i = 0; i + 8 < bytes.size() && !patched; ++i) {
        if (std::equal(five, five + 8, bytes.begin() + static_cast<std::ptrdiff_t>(i))) {
            bytes[i + 7] |= 0x80;
            patched = true;
        }
    }
    REQUIRE(patched);
    io::IoReport report;
    io::ReadOptions noCrc;
    noCrc.verifyCrc = false;
    auto r = io::readTbcad(bytes, report, noCrc);
    REQUIRE(r.ok());
    CHECK(report.warnings.size() == 1);
    CHECK(report.entityCount == 1);
    CHECK(r.value()->find(EntityId{1}) == nullptr);
    CHECK(r.value()->find(EntityId{2}) != nullptr);
}

TEST_CASE("listChunks describes the file layout") {
    auto bytes = writeOrFail(*makeRichDocument());
    auto chunks = io::listChunks(bytes);
    REQUIRE(chunks.ok());
    CHECK(chunks.value().front().type == "HEAD");
    CHECK(chunks.value()[1].type == "STRS");
    CHECK(chunks.value().back().type == "END ");
    for (const auto& c : chunks.value())
        CHECK(c.crcOk);
}

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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

TEST_CASE("burst damage to a big file never crashes the reader") {
    // Runs of damage (not single bytes) in a file with thousands of records: length fields set to
    // all-ones or zero, blocks swapped or repeated, the tail cut off.
    auto doc = makeRichDocument();
    {
        Transaction tx = doc->begin("many");
        for (int i = 0; i < 3000; ++i) {
            Entity e;
            if (i % 3 == 0) {
                e.data = LineData{{double(i), 0}, {double(i), 5}};
            } else if (i % 3 == 1) {
                PolylineData p;
                for (int k = 0; k < 5; ++k)
                    p.v.push_back({{double(i + k), double(k)}, k == 2 ? 0.5 : 0.0, 0, 0});
                e.data = std::move(p);
            } else {
                e.data = TextData{.position = {double(i), 1}, .height = 2, .utf8 = "ข้อความ"};
            }
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    std::mt19937 rng(777);
    for (bool compress : {false, true}) {
        io::WriteOptions o;
        o.compress = compress;
        auto original = writeOrFail(*doc, o);
        int rejected = 0;
        for (int i = 0; i < 600; ++i) {
            auto bytes = original;
            const std::size_t at = rng() % bytes.size();
            const std::size_t len = std::min<std::size_t>(1 + rng() % 64, bytes.size() - at);
            switch (rng() % 6) {
                case 0:
                    for (std::size_t k = 0; k < len; ++k)
                        bytes[at + k] = static_cast<std::uint8_t>(rng());
                    break;
                case 1:
                    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), len, 0xFF);
                    break;
                case 2:
                    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), len, 0);
                    break;
                case 3: {  // swap two blocks
                    const std::size_t other = rng() % (bytes.size() - len + 1);
                    std::swap_ranges(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(at + len),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(
                                                         std::min(other, bytes.size() - len)));
                    break;
                }
                case 4: {  // repeat a block
                    std::vector<std::uint8_t> blk(
                        bytes.begin() + static_cast<std::ptrdiff_t>(at),
                        bytes.begin() + static_cast<std::ptrdiff_t>(at + len));
                    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), blk.begin(),
                                 blk.end());
                    break;
                }
                default:
                    bytes.resize(at);  // the tail is lost
                    if (bytes.empty())
                        bytes.push_back(0);
            }
            io::IoReport report;
            io::ReadOptions opt;
            opt.verifyCrc = (i % 4 == 0);
            auto r = io::readTbcad(bytes, report, opt);
            if (r.ok())
                (void)canonicalDump(*r.value());
            else
                ++rejected;
        }
        INFO("compress=" << compress << " rejected=" << rejected);
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

TEST_CASE("associative dimension links survive a .tbcad round trip") {
    Document doc;
    EntityId line;
    {
        Transaction tx = doc.begin("t");
        Entity l;
        l.data = LineData{{0, 0}, {10, 0}};
        line = tx.add(std::move(l)).value();
        DimensionData d;
        d.kind = DimKind::Linear;
        d.defPts = {Vec2{0, 0}, Vec2{10, 0}, Vec2{0, 5}, Vec2{}};
        d.assoc[0] = {DimAssoc::Kind::Vertex, line, 0};
        d.assoc[1] = {DimAssoc::Kind::Vertex, line, 1};
        Entity e;
        e.data = Indirect<DimensionData>(std::move(d));
        (void)tx.add(std::move(e));
        tx.commit();
    }
    auto bytes = writeOrFail(doc, {.compress = false});
    io::IoReport report;
    auto r = io::readTbcad(bytes, report);
    REQUIRE(r.ok());
    int seen = 0;
    r.value()->forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<DimensionData>(e)) {
            ++seen;
            CHECK(d->assoc[0].kind == DimAssoc::Kind::Vertex);
            CHECK(d->assoc[0].target == line);
            CHECK(d->assoc[1].index == 1);
            CHECK(d->assoc[2].kind == DimAssoc::Kind::None);
        }
    });
    CHECK(seen == 1);
}

TEST_CASE("files that are not .tbcad at all give a clear error, not a crash") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tbcad_notatbcad";
    fs::create_directories(dir);
    auto write = [&](const char* name, const std::string& text) {
        std::ofstream(dir / name, std::ios::binary) << text;
        return dir / name;
    };
    const fs::path files[] = {write("empty.tbcad", ""),
                              write("one.tbcad", "T"),
                              write("text.tbcad", "0\nSECTION\n2\nHEADER\n0\nENDSEC\n0\nEOF\n"),
                              write("zeros.tbcad", std::string(4096, '\0')),
                              write("ones.tbcad", std::string(4096, '\xff')),
                              write("pdf.tbcad", "%PDF-1.7\n1 0 obj\n"),
                              dir / "missing.tbcad",
                              dir};
    for (const fs::path& f : files) {
        io::IoReport report;
        auto r = io::readTbcad(f, report);
        CHECK_FALSE(r.ok());
        if (!r.ok())
            CHECK_FALSE(r.error().message.empty());
    }
}

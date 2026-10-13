#include <doctest/doctest.h>

#include <set>

#include "helpers.h"
#include "tbcad/core/dump.h"

using namespace tbcad;
using namespace tbcad::testing;

TEST_CASE("round trip: every entity kind and table, canonical dump identical") {
    auto doc = makeRichDocument();
    std::string before = canonicalDump(*doc);
    // The test document really covers every kind.
    std::set<EntityKind> kinds;
    doc->forEachEntity([&](const Entity& e) { kinds.insert(kind(e)); });
    CHECK(kinds.size() == 14);

    for (bool compress : {true, false}) {
        CAPTURE(compress);
        io::WriteOptions o;
        o.compress = compress;
        auto bytes = writeOrFail(*doc, o);
        io::IoReport report;
        auto back = readOrFail(bytes, &report);
        CHECK(canonicalDump(*back) == before);
        CHECK(report.formatMajor == 1);
        CHECK(report.unknownChunks == 0);
        CHECK(report.unknownEntityKinds == 0);
        CHECK(report.warnings.empty());
        CHECK(report.entityCount == doc->entityCount());
        // Writing what was read gives the same bytes.
        CHECK(writeOrFail(*back, o) == bytes);
    }
}

TEST_CASE("round trip: Thai strings, far coordinates and header settings survive") {
    auto doc = makeRichDocument();
    auto back = readOrFail(writeOrFail(*doc));
    CHECK(back->layerByName("ห้องนอน") != nullptr);
    CHECK(back->header().createdBy == "TBCAD test ทดสอบ");
    CHECK(back->header().custom.at("note") == std::string("binary\0safe", 11));
    CHECK(back->header().units == LengthUnit::Meter);
    CHECK(back->header().workArea == 2e9);
    const Entity* far = back->find(EntityId{6});  // the second model-space entity
    REQUIRE(far != nullptr);
    const auto* line = entityAs<LineData>(*far);
    REQUIRE(line != nullptr);
    CHECK(line->a.x == 1e8);
    CHECK(line->b.y == 1e8 + 0.001);  // doubles are stored bit for bit
}

TEST_CASE("round trip: ids keep their gaps and nothing is reused") {
    auto doc = makeRichDocument();
    auto back = readOrFail(writeOrFail(*doc));
    CHECK(back->find(EntityId{5}) == nullptr);  // erased before saving
    CHECK(back->find(EntityId{16}) == nullptr);
    CHECK(back->find(EntityId{6}) != nullptr);
    CHECK(back->find(EntityId{17}) != nullptr);
    CHECK(back->blockByName("Door2")->id == doc->blockByName("Door2")->id);
    CHECK(back->linetypeByName("CENTER")->id == doc->linetypeByName("CENTER")->id);
    CHECK(back->modelSpace() == doc->modelSpace());  // draw order, including overrides
}

TEST_CASE("sub-layers: the tree, ByParent values and inherited state are restored") {
    auto doc = makeRichDocument();
    auto back = readOrFail(writeOrFail(*doc));

    const Layer* floor = back->layerByName("S-FLOOR");
    REQUIRE(floor != nullptr);
    const Layer* edge = back->layerByName(floor->id, "edge");
    REQUIRE(edge != nullptr);
    CHECK(edge->parent == floor->id);
    CHECK(edge->color.kind == ColorKind::ByParent);
    CHECK(edge->linetype == kLinetypeByParent);
    CHECK(edge->lineweight.isByParent());

    // Same child name under a different parent is a different layer.
    const Layer* wall = back->layerByName("A-WALL");
    const Layer* wallEdge = back->layerByName(wall->id, "edge");
    REQUIRE(wallEdge != nullptr);
    CHECK(wallEdge->id != edge->id);

    // A parent with a higher id than its child.
    const Layer* lateParent = back->layerByName("late parent");
    REQUIRE(lateParent != nullptr);
    const Layer* late = back->layerByName(lateParent->id, "late child");
    REQUIRE(late != nullptr);
    CHECK(late->id < lateParent->id);
    CHECK(late->parent == lateParent->id);

    // Effective state follows the parents (frozen parent freezes the child; ByParent colour).
    EffectiveLayer a = back->effective(edge->id);
    EffectiveLayer b =
        doc->effective(doc->layerByName(doc->layerByName("S-FLOOR")->id, "edge")->id);
    CHECK(a.color == Color::index(1));
    CHECK(a.color == b.color);
    CHECK(a.linetype == doc->linetypeByName("DASHED")->id);
    CHECK(back->effective(late->id).frozen);
    CHECK_FALSE(back->effective(back->layerByName(floor->id, "Floor hidden")->id).on);
    CHECK(canonicalDump(*back) == canonicalDump(*doc));
}

TEST_CASE("unknown chunk survives read then write byte for byte") {
    auto doc = makeRichDocument();
    auto bytes = writeOrFail(*doc);
    std::vector<std::uint8_t> payload{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0xFF};
    for (std::uint32_t flags : {0u, 0x80u}) {  // an unknown flag bit must not matter either
        auto withExtra = spliceChunkBeforeEnd(bytes, "XTRA", flags, payload);
        io::IoReport report;
        auto back = readOrFail(withExtra, &report);
        CHECK(report.unknownChunks == 1);
        auto again = writeOrFail(*back);
        CHECK(again == withExtra);
    }
    // Two unknown chunks keep their order.
    auto two = spliceChunkBeforeEnd(spliceChunkBeforeEnd(bytes, "AAAA", 0, {1}), "BBBB", 0, {2});
    CHECK(writeOrFail(*readOrFail(two)) == two);
}

TEST_CASE("unknown entity kind and newer record version survive as ProxyData") {
    Document doc;
    {
        Transaction tx = doc.begin("t");
        ProxyData p;
        p.originalType = std::string("\x01tbcad-kind:") + "77:3";
        p.raw = {1, 2, 3, 4, 5};
        Entity e;
        e.common.layer = kLayerZero;
        e.common.color = Color::index(2);
        e.data = std::move(p);
        REQUIRE(tx.add(std::move(e)).ok());
        Entity line;
        line.common.layer = kLayerZero;
        line.data = LineData{{0, 0}, {1, 1}};
        REQUIRE(tx.add(std::move(line)).ok());
        tx.commit();
    }
    auto bytes = writeOrFail(doc);
    io::IoReport report;
    auto back = readOrFail(bytes, &report);
    CHECK(report.unknownEntityKinds == 1);
    CHECK(report.entityCount == 2);
    const auto* proxy = entityAs<ProxyData>(*back->find(EntityId{1}));
    REQUIRE(proxy != nullptr);
    CHECK(proxy->raw == std::vector<std::uint8_t>{1, 2, 3, 4, 5});
    CHECK(back->find(EntityId{1})->common.color == Color::index(2));  // common props still usable
    CHECK(entityAs<LineData>(*back->find(EntityId{2})) != nullptr);
    CHECK(writeOrFail(*back) == bytes);
}

TEST_CASE("a newer major version is refused with a clear message, a newer minor is read") {
    auto doc = makeRichDocument();
    auto bytes = writeOrFail(*doc);

    auto major2 = bytes;
    major2[10] = 2;  // formatMajor, little endian, right after the 10 byte signature
    io::IoReport report;
    auto r = io::readTbcad(major2, report);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("newer") != std::string::npos);
    CHECK(r.error().message.find("update") != std::string::npos);

    auto minor9 = bytes;
    minor9[14] = 9;  // formatMinor
    auto back = readOrFail(minor9, &report);
    CHECK(report.formatMinor == 9);
    CHECK(canonicalDump(*back) == canonicalDump(*doc));
}

TEST_CASE("the signature catches text-mode corruption") {
    auto bytes = writeOrFail(*makeRichDocument());
    CHECK(bytes[0] == 0x89);
    CHECK(bytes[6] == '\r');
    CHECK(bytes[7] == '\n');
    CHECK(bytes[8] == 0x1A);
    CHECK(bytes[9] == '\n');
    // A "\r\n" -> "\n" conversion (what a text-mode copy does) damages the signature.
    std::vector<std::uint8_t> mangled;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
            continue;
        mangled.push_back(bytes[i]);
    }
    io::IoReport report;
    auto r = io::readTbcad(mangled, report);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("signature") != std::string::npos);
    auto empty = io::readTbcad(std::vector<std::uint8_t>{}, report);
    CHECK_FALSE(empty.ok());
}

TEST_CASE("an empty document round trips") {
    Document doc;
    auto back = readOrFail(writeOrFail(doc));
    CHECK(canonicalDump(*back) == canonicalDump(doc));
}

TEST_CASE("compression shrinks repetitive data and is optional") {
    Document doc;
    {
        Transaction tx = doc.begin("t");
        for (int i = 0; i < 5000; ++i) {
            Entity e;
            e.common.layer = kLayerZero;
            e.data = LineData{{double(i), 0}, {double(i), 10}};
            REQUIRE(tx.add(std::move(e)).ok());
        }
        tx.commit();
    }
    io::WriteOptions off;
    off.compress = false;
    auto big = writeOrFail(doc, off);
    auto small = writeOrFail(doc);
    CHECK(small.size() < big.size());
    CHECK(canonicalDump(*readOrFail(small)) == canonicalDump(*readOrFail(big)));
}

TEST_CASE("round trip: the current colour, linetype and lineweight survive") {
    auto doc = makeRichDocument();
    LinetypeId lt = kLinetypeContinuous;
    for (const auto& [id, row] : doc->linetypes())
        lt = id;
    {
        auto tx = doc->begin("current");
        REQUIRE(tx.modifyHeader([&](Header& h) {
            h.currentColor = Color::rgb(12, 200, 255);
            h.currentLinetype = lt;
            h.currentLineweight = LineWeight::mm(0.35);
        }));
        tx.commit();
    }
    auto back = readOrFail(writeOrFail(*doc));
    CHECK(back->header().currentColor == Color::rgb(12, 200, 255));
    CHECK(back->header().currentLinetype == lt);
    CHECK(back->header().currentLineweight == LineWeight::mm(0.35));
    CHECK(canonicalDump(*back) == canonicalDump(*doc));
}

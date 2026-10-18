#include <doctest/doctest.h>

#include <random>

#include "helpers.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/delta.h"

using namespace tbcad;
using namespace tbcad::testing;

namespace {

struct Recorder : DocumentObserver {
    std::vector<ChangeSet> sets{};
    void onChanged(const ChangeSet& cs) override { sets.push_back(cs); }
};

template <class T>
T must(Result<T> r) {
    TB_REQUIRE_OK(r);
    return std::move(r).value();
}

Entity make(auto data, LayerId layer) {
    Entity e;
    e.common.layer = layer;
    e.data = std::move(data);
    return e;
}

// A series of edits that touches every part of a ChangeSet.
void editSeries(Document& doc) {
    LayerId walls, sub;
    BlockId block;
    EntityId first, second;
    {
        Transaction tx = doc.begin("layers and styles");
        walls = must(tx.addLayer({.name = "Walls ผนัง", .color = Color::index(3)}));
        sub = must(tx.addLayer(
            {.name = "sub", .parent = walls, .color = Color::byParent(), .locked = true}));
        LinetypeId dashed = must(tx.addLinetype({.name = "DASHED", .pattern = {12, -6}}));
        TextStyleId thai =
            must(tx.addTextStyle({.name = "Thai", .fontFamily = "Sarabun", .height = 2}));
        DimStyle ds;
        ds.name = "Arch";
        ds.textStyle = thai;
        ds.prefix = "<";
        ds.dimLineColor = Color::rgb(1, 2, 3);
        (void)must(tx.addDimStyle(ds));
        (void)tx.modifyLayer(walls, [&](Layer& l) { l.linetype = dashed; });
        tx.commit();
    }
    {
        Transaction tx = doc.begin("draw");
        block = must(tx.addBlock({.name = "B", .base = {1, 2}}));
        first = must(tx.add(make(LineData{{0, 0}, {10, 0}}, walls)));
        second = must(tx.add(make(CircleData{{5, 5}, 3}, sub)));
        (void)must(tx.add(make(LineData{{1, 1}, {2, 2}}, walls), block));
        (void)must(tx.add(make(TextData{.position = {1, 1}, .height = 2, .utf8 = "สวัสดี"}, walls)));
        tx.commit();
    }
    {
        Transaction tx = doc.begin("edit");
        (void)tx.modify(first, [](Entity& e) { std::get<LineData>(e.data).b = {20, 0}; });
        (void)tx.bringToFront(first);
        (void)tx.erase(second);
        (void)tx.modifyHeader([&](Header& h) {
            h.createdBy = "someone";
            h.custom["note"] = "x";
            h.currentLayer = walls;
        });
        tx.commit();
    }
    {
        Transaction tx = doc.begin("erase rows");
        (void)tx.eraseLayer(sub);
        tx.commit();
    }
}

}  // namespace

TEST_CASE("delta: replaying the encoded changes of an edit series reproduces the document") {
    Document source;
    Recorder rec;
    source.addObserver(&rec);
    editSeries(source);
    source.removeObserver(&rec);
    REQUIRE(rec.sets.size() == 4);

    Document replica;
    for (const ChangeSet& cs : rec.sets) {
        std::vector<std::uint8_t> bytes = io::encodeDelta(io::deltaOf(cs));
        auto delta = io::decodeDelta(bytes);
        TB_REQUIRE_OK(delta);
        auto rebuilt = io::materialize(delta.value(), replica);
        TB_REQUIRE_OK(rebuilt);
        TB_REQUIRE_OK(replica.applyChecked(rebuilt.value()));
    }
    CHECK(canonicalDump(replica) == canonicalDump(source));
}

TEST_CASE("delta: levels, entity levels and the current level travel with a change") {
    Document source;
    Recorder rec;
    source.addObserver(&rec);
    LevelId upper;
    {
        Transaction tx = source.begin("level");
        upper = must(tx.addLevel({.name = "Level 2", .elevation = 3200, .locked = true}));
        REQUIRE(tx.modifyHeader([&](Header& h) { h.currentLevel = upper; }).ok());
        tx.commit();
    }
    {
        Transaction tx = source.begin("line");
        Entity e;
        e.data = LineData{{0, 0}, {5, 5}};
        (void)tx.add(std::move(e));
        tx.commit();
    }
    source.removeObserver(&rec);
    Document replica;
    for (const ChangeSet& cs : rec.sets) {
        auto delta = io::decodeDelta(io::encodeDelta(io::deltaOf(cs)));
        TB_REQUIRE_OK(delta);
        auto rebuilt = io::materialize(delta.value(), replica);
        TB_REQUIRE_OK(rebuilt);
        TB_REQUIRE_OK(replica.applyChecked(rebuilt.value()));
    }
    CHECK(canonicalDump(replica) == canonicalDump(source));
    CHECK(replica.header().currentLevel == upper);
    CHECK(replica.level(upper)->locked);
    CHECK(replica.find(EntityId{1})->common.level == upper);
}

TEST_CASE("delta: the receiver rebuilds the before half from its own document") {
    Document source;
    Recorder rec;
    source.addObserver(&rec);
    editSeries(source);

    Document replica;
    Recorder seen;
    replica.addObserver(&seen);
    for (const ChangeSet& cs : rec.sets) {
        auto rebuilt = io::materialize(io::deltaOf(cs), replica);
        TB_REQUIRE_OK(rebuilt);
        TB_REQUIRE_OK(replica.applyChecked(rebuilt.value()));
    }
    REQUIRE(seen.sets.size() == rec.sets.size());
    // The third set modifies the line and erases the circle: both have a before image.
    const ChangeSet& edit = seen.sets[2];
    std::size_t withBefore = 0;
    for (const EntityChange& c : edit.entities)
        withBefore += edit.before(c) != nullptr ? 1 : 0;
    CHECK(withBefore == 2);
}

TEST_CASE("delta: a change that breaks the rules is refused and leaves the document alone") {
    Document doc;
    Recorder seen;
    doc.addObserver(&seen);
    const std::string before = canonicalDump(doc);

    SUBCASE("entity on a layer that does not exist") {
        io::Delta d;
        d.entities.push_back({EntityId{1}, make(LineData{{0, 0}, {1, 1}}, LayerId{77})});
        auto cs = io::materialize(d, doc);
        TB_REQUIRE_OK(cs);
        CHECK_FALSE(doc.applyChecked(cs.value()).ok());
    }
    SUBCASE("duplicate layer name") {
        io::Delta d;
        d.layers.push_back({5, Layer{.id = LayerId{5}, .name = "A"}});
        d.layers.push_back({6, Layer{.id = LayerId{6}, .name = "a"}});
        auto cs = io::materialize(d, doc);
        TB_REQUIRE_OK(cs);
        CHECK_FALSE(doc.applyChecked(cs.value()).ok());
    }
    SUBCASE("erasing the built-in layer") {
        io::Delta d;
        d.layers.push_back({kLayerZero.value, std::nullopt});
        auto cs = io::materialize(d, doc);
        TB_REQUIRE_OK(cs);
        CHECK_FALSE(doc.applyChecked(cs.value()).ok());
    }
    SUBCASE("erasing a layer other entities still use") {
        {
            Transaction tx = doc.begin("setup");
            LayerId l = must(tx.addLayer({.name = "L"}));
            (void)must(tx.add(make(LineData{{0, 0}, {1, 1}}, l)));
            tx.commit();
        }
        const std::string setup = canonicalDump(doc);
        io::Delta d;
        d.layers.push_back({2, std::nullopt});
        auto cs = io::materialize(d, doc);
        TB_REQUIRE_OK(cs);
        CHECK_FALSE(doc.applyChecked(cs.value()).ok());
        CHECK(canonicalDump(doc) == setup);
        return;
    }
    SUBCASE("block that lists an entity it does not own") {
        io::Delta d;
        BlockDef b;
        b.id = BlockId{1};
        b.name = "B";
        b.entities = {EntityId{9}};
        d.blocks.push_back({1, b});
        auto cs = io::materialize(d, doc);
        TB_REQUIRE_OK(cs);
        CHECK_FALSE(doc.applyChecked(cs.value()).ok());
    }
    SUBCASE("non-finite geometry") {
        io::Delta d;
        d.entities.push_back({EntityId{1}, make(LineData{{0, 0}, {std::nan(""), 1}}, kLayerZero)});
        auto cs = io::materialize(d, doc);
        TB_REQUIRE_OK(cs);
        CHECK_FALSE(doc.applyChecked(cs.value()).ok());
    }
    CHECK(canonicalDump(doc) == before);
    CHECK(seen.sets.empty());
}

TEST_CASE("delta: size limits are enforced before anything is allocated") {
    Document source;
    Recorder rec;
    source.addObserver(&rec);
    editSeries(source);
    std::vector<std::uint8_t> bytes = io::encodeDelta(io::deltaOf(rec.sets[1]));

    io::DeltaLimits tight;
    tight.maxBytes = bytes.size() - 1;
    CHECK_FALSE(io::decodeDelta(bytes, tight).ok());
    tight = {};
    tight.maxEntities = 2;
    CHECK_FALSE(io::decodeDelta(bytes, tight).ok());
    CHECK(io::decodeDelta(bytes).ok());
}

TEST_CASE("delta: damaged input never crashes the decoder or corrupts a document") {
    Document source;
    Recorder rec;
    source.addObserver(&rec);
    editSeries(source);
    std::vector<std::vector<std::uint8_t>> corpus;
    for (const ChangeSet& cs : rec.sets)
        corpus.push_back(io::encodeDelta(io::deltaOf(cs)));

    // Rebuilds a document that has seen the first `n` sets, so damaged input meets real state.
    auto stateAfter = [&](std::size_t n) {
        auto doc = std::make_unique<Document>();
        for (std::size_t i = 0; i < n; ++i) {
            auto cs = io::materialize(io::deltaOf(rec.sets[i]), *doc);
            REQUIRE(cs.ok());
            REQUIRE(doc->applyChecked(cs.value()).ok());
        }
        return doc;
    };

    std::mt19937_64 rng(12345);
    std::size_t decoded = 0, applied = 0;
    for (std::size_t round = 0; round < 4000; ++round) {
        std::size_t which = rng() % corpus.size();
        std::vector<std::uint8_t> bytes = corpus[which];
        switch (rng() % 4) {
            case 0:  // flip a few bytes
                for (int k = 0; k < 1 + int(rng() % 4); ++k)
                    bytes[rng() % bytes.size()] ^= static_cast<std::uint8_t>(1 + rng() % 255);
                break;
            case 1:  // truncate
                bytes.resize(rng() % bytes.size());
                break;
            case 2:  // overwrite a run
                for (std::size_t k = 0, at = rng() % bytes.size(); k < 8 && at + k < bytes.size();
                     ++k)
                    bytes[at + k] = static_cast<std::uint8_t>(rng());
                break;
            default:  // random garbage after a valid header
                bytes.resize(5 + rng() % 64);
                for (std::size_t k = 5; k < bytes.size(); ++k)
                    bytes[k] = static_cast<std::uint8_t>(rng());
        }
        auto d = io::decodeDelta(bytes);
        if (!d.ok())
            continue;
        ++decoded;
        auto doc = stateAfter(which);
        const std::string before = canonicalDump(*doc);
        auto cs = io::materialize(d.value(), *doc);
        if (!cs.ok())
            continue;
        if (doc->applyChecked(cs.value()).ok()) {
            ++applied;
            // Whatever was accepted must still dump and be a state a Transaction could produce.
            (void)canonicalDump(*doc);
        } else {
            CHECK(canonicalDump(*doc) == before);
        }
    }
    INFO("decoded " << decoded << ", applied " << applied);
    CHECK(decoded > 0);
}

TEST_CASE("optional chunks: an extra chunk survives a read and is replaced by a newer one") {
    auto doc = makeRichDocument();
    io::WriteOptions o;
    o.extraChunks.push_back({"VERS", {1, 2, 3, 4, 5}});
    std::vector<std::uint8_t> file = writeOrFail(*doc, o);

    io::IoReport report;
    auto back = readOrFail(file, &report);
    auto got = io::findOptionalChunk(*back, "VERS");
    REQUIRE(got.has_value());
    CHECK(*got == std::vector<std::uint8_t>{1, 2, 3, 4, 5});
    CHECK_FALSE(io::findOptionalChunk(*back, "NOPE").has_value());

    // Saved again with no extra chunk: carried through unchanged.
    auto again = readOrFail(writeOrFail(*back));
    CHECK(io::findOptionalChunk(*again, "VERS") == got);

    // Saved with a new one: the old chunk is not written twice.
    io::WriteOptions n;
    n.extraChunks.push_back({"VERS", {9}});
    auto replaced = readOrFail(writeOrFail(*back, n));
    CHECK(io::findOptionalChunk(*replaced, "VERS") == std::vector<std::uint8_t>{9});
    io::IoReport rep2;
    (void)readOrFail(writeOrFail(*back, n), &rep2);
    CHECK(rep2.unknownChunks == 1);

    // Readers that do not know the chunk still get the same drawing.
    CHECK(back->entityCount() == doc->entityCount());
}

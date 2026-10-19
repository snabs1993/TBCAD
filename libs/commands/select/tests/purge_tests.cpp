#include "test_util.h"

using namespace tbcad;
using namespace tbcad::select::test;

TEST_CASE("PURGE removes unused layers, styles, linetypes and blocks") {
    Fixture f;
    f.addLayer("Unused");
    LayerId used = f.addLayer("Used");
    f.addEntity(LineData{{0, 0}, {1, 1}}, used);
    {
        Transaction tx = f.doc.begin("setup");
        Linetype lt;
        lt.name = "DASHED";
        lt.pattern = {5, -2};
        REQUIRE(tx.addLinetype(lt).ok());
        TextStyle ts;
        ts.name = "Spare";
        REQUIRE(tx.addTextStyle(ts).ok());
        BlockDef b;
        b.name = "Empty";
        REQUIRE(tx.addBlock(b).ok());
        tx.commit();
    }
    CHECK(f.host.run("PURGE", {"", "Yes"}).finished);
    CHECK(f.host.logContains("Purged"));
    CHECK(f.host.logContains("Layers purged: Unused"));  // the names, not just counts
    CHECK(f.host.logContains("Blocks purged: Empty"));
    CHECK(f.doc.layerByName("Unused") == nullptr);
    CHECK(f.doc.layerByName("Used") != nullptr);
    CHECK(f.doc.linetypeByName("DASHED") == nullptr);
    CHECK(f.doc.textStyleByName("Spare") == nullptr);
    CHECK(f.doc.blockByName("Empty") == nullptr);
    f.host.undo();
    CHECK(f.doc.layerByName("Unused") != nullptr);
    CHECK(f.doc.blockByName("Empty") != nullptr);
}

TEST_CASE("PURGE keeps rows that are in use and the built-in ones") {
    Fixture f;
    LayerId used = f.addLayer("Used");
    LayerId parent = f.addLayer("Parent");
    LayerId child = f.addLayer("Child", parent);
    f.addEntity(LineData{{0, 0}, {1, 1}}, child);
    auto before = f.doc.layers().size();
    f.host.run("PURGE", {"", "Yes"});
    // Used is empty and goes; Parent stays because Child is used.
    CHECK(f.doc.layers().size() == before - 1);
    CHECK(f.doc.layerByName("Used") == nullptr);
    CHECK(f.doc.layerByName("Parent") != nullptr);
    CHECK(f.doc.layerByName("0") != nullptr);
    CHECK(f.doc.linetypeByName("Continuous") != nullptr);
    (void)used;
}

TEST_CASE("PURGE repeats: a sub-layer goes first, then its parent") {
    Fixture f;
    LayerId parent = f.addLayer("P");
    f.addLayer("C", parent);
    f.host.run("PURGE", {"Layers", "Yes"});
    CHECK(f.doc.layerByName("P") == nullptr);
    CHECK(f.host.logContains("2 layers"));
}

TEST_CASE("PURGE with nothing unused says so and changes nothing") {
    Fixture f;
    f.host.run("PURGE", {"", "Yes"});
    CHECK(f.host.logContains("Nothing to purge."));
}

TEST_CASE("PURGE Layers leaves the other tables alone") {
    Fixture f;
    f.addLayer("Spare");
    {
        Transaction tx = f.doc.begin("setup");
        BlockDef b;
        b.name = "Empty";
        REQUIRE(tx.addBlock(b).ok());
        tx.commit();
    }
    f.host.run("PURGE", {"Layers", "Yes"});
    CHECK(f.doc.layerByName("Spare") == nullptr);
    CHECK(f.doc.blockByName("Empty") != nullptr);
}

TEST_CASE("PURGE keeps the current layer") {
    Fixture f;
    LayerId cur = f.addLayer("Current");
    {
        Transaction tx = f.doc.begin("setup");
        REQUIRE(tx.modifyHeader([&](Header& h) { h.currentLayer = cur; }).ok());
        tx.commit();
    }
    f.host.run("PURGE", {"", "Yes"});
    CHECK(f.doc.layerByName("Current") != nullptr);
}

TEST_CASE("PURGE lists what would go and keeps everything unless confirmed") {
    Fixture f;
    f.addLayer("Spare");
    const std::size_t before = f.doc.layers().size();
    f.host.run("PURGE", {"Layers", ""});  // Enter takes the default: No
    CHECK(f.doc.layers().size() == before);
    CHECK(f.host.logContains("Spare"));
    CHECK(f.host.logContains("Nothing was purged"));
}

TEST_CASE("PURGE removes a long chain of nested unused blocks in one go") {
    Document doc;
    constexpr int kChain = 400;
    {
        Transaction tx = doc.begin("chain");
        std::vector<BlockId> ids;
        for (int i = 0; i < kChain; ++i)
            ids.push_back(tx.addBlock({.name = "N" + std::to_string(i)}).value());
        for (int i = 0; i < kChain; ++i) {
            Entity e;
            if (i + 1 < kChain)
                e.data = Indirect<InsertData>(
                    InsertData{ids[static_cast<std::size_t>(i + 1)], {1, 0}, {1, 1}, 0});
            else
                e.data = LineData{{0, 0}, {1, 1}};
            (void)tx.add(std::move(e), ids[static_cast<std::size_t>(i)]);
        }
        // One chain that is used: the model places its outermost block.
        BlockId inner = tx.addBlock({.name = "Inner"}).value();
        (void)tx.add(Entity{{}, LineData{{0, 0}, {2, 2}}}, inner);
        BlockId keep = tx.addBlock({.name = "Outer"}).value();
        {
            Entity e;
            e.data = Indirect<InsertData>(InsertData{inner, {0, 0}, {1, 1}, 0});
            (void)tx.add(std::move(e), keep);
        }
        {
            Entity e;
            e.data = Indirect<InsertData>(InsertData{keep, {5, 5}, {1, 1}, 0});
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    Transaction tx = doc.begin("purge");
    const auto n = select::purgeUnused(doc, tx, static_cast<unsigned>(select::PurgeKind::Blocks));
    tx.commit();
    CHECK(n.blocks == kChain);
    CHECK(doc.blockByName("Inner") != nullptr);
    CHECK(doc.blockByName("Outer") != nullptr);
    CHECK(doc.blockByName("N0") == nullptr);
    CHECK(doc.blockByName("N399") == nullptr);
}

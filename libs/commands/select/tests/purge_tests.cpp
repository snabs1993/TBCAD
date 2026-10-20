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
    CHECK(f.host.run("-PURGE", {"", "Yes"}).finished);
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
    f.host.run("-PURGE", {"", "Yes"});
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
    f.host.run("-PURGE", {"Layers", "Yes"});
    CHECK(f.doc.layerByName("P") == nullptr);
    CHECK(f.host.logContains("2 layers"));
}

TEST_CASE("PURGE with nothing unused says so and changes nothing") {
    Fixture f;
    f.host.run("-PURGE", {"", "Yes"});
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
    f.host.run("-PURGE", {"Layers", "Yes"});
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
    f.host.run("-PURGE", {"", "Yes"});
    CHECK(f.doc.layerByName("Current") != nullptr);
}

TEST_CASE("PURGE lists what would go and keeps everything unless confirmed") {
    Fixture f;
    f.addLayer("Spare");
    const std::size_t before = f.doc.layers().size();
    f.host.run("-PURGE", {"Layers", ""});  // Enter takes the default: No
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

TEST_CASE("Purge dialog helpers: what can go, nested or not, and removing a choice") {
    Fixture f;
    // Layer "Inner" is used only inside block "Outer", which nothing inserts.
    LayerId inner = f.addLayer("Inner");
    f.addLayer("Spare");
    {
        Transaction tx = f.doc.begin("setup");
        BlockDef b;
        b.name = "Outer";
        BlockId outer = tx.addBlock(b).value();
        Entity e;
        e.data = LineData{{0, 0}, {1, 0}};
        e.common.layer = inner;
        REQUIRE(tx.add(e, outer).ok());
        tx.commit();
    }
    const std::uint64_t rev = f.doc.revision();
    auto names = [](const std::vector<select::PurgeItem>& items) {
        std::vector<std::string> out;
        for (const auto& it : items)
            out.push_back(it.name);
        return out;
    };
    auto flat = select::purgeableItems(f.doc, false);
    auto deep = select::purgeableItems(f.doc, true);
    CHECK(f.doc.revision() == rev);  // nothing changed
    CHECK(f.doc.layerByName("Spare"));
    const auto flatNames = names(flat), deepNames = names(deep);
    CHECK(std::count(flatNames.begin(), flatNames.end(), "Spare") == 1);
    CHECK(std::count(flatNames.begin(), flatNames.end(), "Outer") == 1);
    CHECK(std::count(flatNames.begin(), flatNames.end(), "Inner") == 0);
    CHECK(std::count(deepNames.begin(), deepNames.end(), "Inner") == 1);

    // Removing the chosen rows: Inner goes once Outer has gone, in one call.
    std::vector<select::PurgeItem> chosen;
    for (const auto& it : deep)
        if (it.name == "Outer" || it.name == "Inner")
            chosen.push_back(it);
    {
        Transaction tx = f.doc.begin("purge");
        CHECK(select::purgeItems(f.doc, tx, chosen) == 2);
        tx.commit();
    }
    CHECK_FALSE(f.doc.layerByName("Inner"));
    CHECK_FALSE(f.doc.blockByName("Outer"));
    CHECK(f.doc.layerByName("Spare"));
}

TEST_CASE("-PURGE and the dialog remove zero-length objects, empty text and orphaned data") {
    Fixture f;
    const EntityId zero = f.addEntity(LineData{{5, 5}, {5, 5}});
    const EntityId keep = f.addEntity(LineData{{0, 0}, {5, 5}});
    TextData blank;
    blank.utf8 = "   ";
    const EntityId empty = f.addEntity(Indirect<TextData>(blank));
    {
        Transaction tx = f.doc.begin("orphan");
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.custom["tbcad.pstyle.ent:999999"] = "Thick";
                      h.custom["tbcad.pstyle.layer:4242"] = "Thin";
                  }).ok());
        tx.commit();
    }
    CHECK(select::zeroLengthObjects(f.doc) == std::vector<EntityId>{zero});
    CHECK(select::emptyTextObjects(f.doc) == std::vector<EntityId>{empty});
    CHECK(select::orphanedDataKeys(f.doc).size() == 2);

    CHECK(f.host.run("-PURGE", {"Zerolength", "Yes"}).finished);
    CHECK(f.doc.find(zero) == nullptr);
    CHECK(f.doc.find(keep) != nullptr);
    CHECK(f.host.run("-PURGE", {"Orphaned", "Yes"}).finished);
    CHECK(select::orphanedDataKeys(f.doc).empty());

    // What the dialog runs: the empty text and a spare layer in one undo step.
    f.addLayer("Spare");
    std::string spec = " E";
    for (const auto& it : select::purgeableItems(f.doc, true))
        if (it.name == "Spare")
            spec += " L" + std::to_string(it.id);
    f.host.setSetting("tmp.purge.items", spec);
    CHECK(f.host.run("_PURGEITEMS", {}).finished);
    CHECK(f.doc.find(empty) == nullptr);
    CHECK_FALSE(f.doc.layerByName("Spare"));
    CHECK(f.host.logContains("Purged 1 named object, 1 object."));
    f.host.undo();
    CHECK(f.doc.find(empty) != nullptr);
    CHECK(f.doc.layerByName("Spare"));
}

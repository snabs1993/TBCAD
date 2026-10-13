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
    CHECK(f.host.run("PURGE", {""}).finished);
    CHECK(f.host.logContains("Purged"));
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
    f.host.run("PURGE", {""});
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
    f.host.run("PURGE", {"Layers"});
    CHECK(f.doc.layerByName("P") == nullptr);
    CHECK(f.host.logContains("2 layers"));
}

TEST_CASE("PURGE with nothing unused says so and changes nothing") {
    Fixture f;
    f.host.run("PURGE", {""});
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
    f.host.run("PURGE", {"Layers"});
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
    f.host.run("PURGE", {""});
    CHECK(f.doc.layerByName("Current") != nullptr);
}

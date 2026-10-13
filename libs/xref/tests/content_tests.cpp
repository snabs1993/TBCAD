#include <doctest/doctest.h>

#include "tbcad/xref/xref.h"

using namespace tbcad;
using namespace tbcad::xref;

namespace {

Entity lineOn(LayerId layer, LinetypeId lt = {}) {
    Entity e;
    e.data = LineData{{0, 0}, {1, 0}};
    e.common.layer = layer;
    e.common.linetype = lt;
    return e;
}

// Layers walls and walls~inner, linetype Hidden, text style Notes, dim style Arch (uses Notes),
// block door (a line on walls with linetype Hidden, plus text in Notes) and block gate that
// inserts door.
Source makeSource() {
    Source s;
    s.doc = std::make_unique<Document>();
    Transaction tx = s.doc->begin("source");
    Layer walls;
    walls.name = "walls";
    LayerId w = tx.addLayer(walls).value();
    Layer inner;
    inner.name = "inner";
    inner.parent = w;
    (void)tx.addLayer(inner);
    Linetype hidden;
    hidden.name = "Hidden";
    hidden.pattern = {6, -3};
    LinetypeId lt = tx.addLinetype(hidden).value();
    TextStyle notes;
    notes.name = "Notes";
    TextStyleId ns = tx.addTextStyle(notes).value();
    DimStyle arch;
    arch.name = "Arch";
    arch.textStyle = ns;
    (void)tx.addDimStyle(arch);
    BlockId door = tx.addBlock(BlockDef{.name = "door"}).value();
    (void)tx.add(lineOn(w, lt), door);
    TextData t;
    t.utf8 = "D";
    t.style = ns;
    Entity te;
    te.data = Indirect<TextData>(t);
    (void)tx.add(te, door);
    BlockId gate = tx.addBlock(BlockDef{.name = "gate"}).value();
    Entity ins;
    ins.data = Indirect<InsertData>(InsertData{door, {0, 0}, {1, 1}, 0});
    (void)tx.add(ins, gate);
    tx.commit();
    return s;
}

}  // namespace

TEST_CASE("listContent names blocks, layers (with sub-layers), linetypes and styles") {
    Source s = makeSource();
    auto items = listContent(*s.doc);
    auto has = [&](ContentKind k, const char* n) {
        for (const auto& i : items)
            if (i.kind == k && i.name == n)
                return true;
        return false;
    };
    CHECK(has(ContentKind::Block, "door"));
    CHECK(has(ContentKind::Block, "gate"));
    CHECK(has(ContentKind::Layer, "walls"));
    CHECK(has(ContentKind::Layer, "walls~inner"));
    CHECK_FALSE(has(ContentKind::Layer, "0"));
    CHECK(has(ContentKind::Linetype, "Hidden"));
    CHECK_FALSE(has(ContentKind::Linetype, "Continuous"));
    CHECK(has(ContentKind::TextStyle, "Notes"));
    CHECK_FALSE(has(ContentKind::TextStyle, "Standard"));
    CHECK(has(ContentKind::DimStyle, "Arch"));
}

TEST_CASE("importing a block brings its layer, linetype, style and nested block") {
    Source s = makeSource();
    Document host;
    ImportReport rep;
    {
        Transaction tx = host.begin("import");
        REQUIRE(importContent(tx, host, s, {{ContentKind::Block, "gate"}}, rep));
        tx.commit();
    }
    CHECK(rep.skipped.empty());
    CHECK(host.blockByName("gate"));
    CHECK(host.blockByName("content$door"));  // nested block, named after what brought it
    CHECK(host.layerByName("walls"));
    CHECK(host.linetypeByName("Hidden"));
    CHECK(host.textStyleByName("Notes"));
    const BlockDef* nested = host.blockByName("content$door");
    REQUIRE(nested);
    REQUIRE(nested->entities.size() == 2);
    const Entity* line = host.find(nested->entities[0]);
    REQUIRE(line);
    CHECK(line->common.layer == host.layerByName("walls")->id);
    CHECK(line->common.linetype == host.linetypeByName("Hidden")->id);
}

TEST_CASE("importing rows skips what the drawing already has and keeps sub-layer parents") {
    Source s = makeSource();
    Document host;
    ImportReport rep;
    {
        Transaction tx = host.begin("import");
        REQUIRE(importContent(tx, host, s,
                              {{ContentKind::Layer, "walls~inner"},
                               {ContentKind::Linetype, "Hidden"},
                               {ContentKind::DimStyle, "Arch"},
                               {ContentKind::Layer, "walls~inner"},
                               {ContentKind::TextStyle, "Notes"},
                               {ContentKind::Block, "nope"}},
                              rep));
        tx.commit();
    }
    const Layer* walls = host.layerByName("walls");
    REQUIRE(walls);
    const Layer* inner = host.layerByName(walls->id, "inner");
    REQUIRE(inner);
    CHECK(inner->parent == walls->id);
    CHECK(host.linetypeByName("Hidden"));
    REQUIRE(host.dimStyleByName("Arch"));
    CHECK(host.dimStyleByName("Arch")->textStyle == host.textStyleByName("Notes")->id);
    // The second walls~inner, the Notes style (came with Arch) and the missing block.
    CHECK(rep.skipped.size() == 3);
    CHECK(rep.added == 4);  // walls, inner, Hidden, Arch (Notes arrived with Arch, not counted)
}

TEST_CASE("a taken block name is skipped") {
    Source s = makeSource();
    Document host;
    ImportReport first, second;
    {
        Transaction tx = host.begin("import");
        REQUIRE(importContent(tx, host, s, {{ContentKind::Block, "door"}}, first));
        tx.commit();
    }
    {
        Transaction tx = host.begin("import again");
        REQUIRE(importContent(tx, host, s, {{ContentKind::Block, "door"}}, second));
        tx.commit();
    }
    CHECK(first.added == 1);
    CHECK(second.added == 0);
    REQUIRE(second.skipped.size() == 1);
}

TEST_CASE("importContent without a source drawing is an error") {
    Document host;
    ImportReport rep;
    Transaction tx = host.begin("x");
    CHECK_FALSE(importContent(tx, host, Source{}, {{ContentKind::Layer, "a"}}, rep));
}

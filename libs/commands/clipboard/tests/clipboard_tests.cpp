#include <doctest/doctest.h>

#include <filesystem>

#include "tbcad/clipboard/clipboard.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;

    Fixture() : host(doc, reg) { registerAllCommands(reg); }

    EntityId add(EntityData d, LayerId layer = {}) {
        Entity e;
        e.data = std::move(d);
        e.common.layer = layer;
        Transaction tx = doc.begin("setup");
        auto id = tx.add(std::move(e));
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    const Entity& get(EntityId id) {
        const Entity* e = doc.find(id);
        REQUIRE(e != nullptr);
        return *e;
    }
    std::size_t count(EntityKind k) {
        std::size_t n = 0;
        doc.forEachEntity([&](const Entity& e) { n += kind(e) == k ? 1 : 0; });
        return n;
    }
};

bool near(Vec2 a, Vec2 b, double tol = 1e-6) {
    return distance(a, b) <= tol;
}

}  // namespace

TEST_CASE("clipboard commands are registered") {
    Fixture f;
    for (const char* n :
         {"COPYCLIP", "COPYBASE", "CUTCLIP", "PASTECLIP", "PASTEBLOCK", "PASTEORIG", "PASTESPEC"})
        CHECK(f.reg.find(n) != nullptr);
}

TEST_CASE("copy then paste lands the lower-left corner on the picked point") {
    Fixture f;
    EntityId a = f.add(LineData{{10, 10}, {20, 10}});
    EntityId b = f.add(CircleData{{15, 20}, 5});
    f.host.run("COPYCLIP", {ScriptItem::select({a, b}), ScriptItem::enter()});
    REQUIRE(clipboard::hasClip());
    CHECK(f.host.undoStack().undoLabel() != "COPYCLIP");  // nothing to undo for a copy
    auto r = f.host.run("PASTECLIP", {"100,100"});
    CHECK(r.finished);
    CHECK(f.count(EntityKind::Line) == 2);
    CHECK(f.count(EntityKind::Circle) == 2);
    bool found = false;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e); l && near(l->a, {100, 100}))
            found = true;
    });
    // The clip's lower-left corner is (10, 10), so the line starts at the pick.
    CHECK(found);
    CHECK(f.host.undoStack().undoLabel() == "PASTECLIP");
    f.host.undo();
    CHECK(f.count(EntityKind::Line) == 1);
}

TEST_CASE("COPYBASE and PASTEORIG") {
    Fixture f;
    EntityId a = f.add(LineData{{10, 10}, {20, 10}});
    f.host.run("COPYBASE", {ScriptItem::select({a}), ScriptItem::enter(), "10,10"});
    f.host.run("PASTECLIP", {"0,0"});
    f.host.run("PASTEORIG", {});
    std::size_t atOrigin = 0, inPlace = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e)) {
            atOrigin += near(l->a, {0, 0}) ? 1 : 0;
            inPlace += near(l->a, {10, 10}) ? 1 : 0;
        }
    });
    CHECK(atOrigin == 1);
    CHECK(inPlace == 2);  // the original and the paste at the original coordinates
}

TEST_CASE("CUTCLIP removes the objects and keeps them for pasting") {
    Fixture f;
    EntityId a = f.add(LineData{{0, 0}, {5, 0}});
    f.host.run("CUTCLIP", {ScriptItem::select({a}), ScriptItem::enter()});
    CHECK(f.count(EntityKind::Line) == 0);
    f.host.run("PASTECLIP", {"3,3"});
    CHECK(f.count(EntityKind::Line) == 1);
}

TEST_CASE("pasting into another drawing creates the layers, linetypes and styles it needs") {
    Fixture src;
    Layer lay;
    lay.name = "WALLS";
    lay.color = Color::index(1);
    LayerId layer;
    {
        Transaction tx = src.doc.begin("rows");
        auto l = tx.addLayer(lay);
        REQUIRE(l.ok());
        layer = l.value();
        tx.commit();
    }
    EntityId a = src.add(LineData{{0, 0}, {5, 0}}, layer);
    TextData t;
    t.utf8 = "hi";
    EntityId txt = src.add(Indirect<TextData>(t), layer);
    src.host.run("COPYCLIP", {ScriptItem::select({a, txt}), ScriptItem::enter()});

    Fixture dst;
    CHECK(dst.doc.layerByName("WALLS") == nullptr);
    dst.host.run("PASTECLIP", {"0,0"});
    const Layer* made = dst.doc.layerByName("WALLS");
    REQUIRE(made != nullptr);
    CHECK(made->color == Color::index(1));
    std::size_t onLayer = 0;
    dst.doc.forEachEntity([&](const Entity& e) { onLayer += e.common.layer == made->id ? 1 : 0; });
    CHECK(onLayer == 2);
    // A second paste reuses the layer.
    dst.host.run("PASTECLIP", {"10,0"});
    std::size_t layers = 0;
    for (const auto& [id, l] : dst.doc.layers())
        layers += l.name == "WALLS" ? 1 : 0;
    CHECK(layers == 1);
}

TEST_CASE("PASTEBLOCK makes one insert of a new block") {
    Fixture f;
    EntityId a = f.add(LineData{{10, 10}, {20, 10}});
    EntityId b = f.add(LineData{{10, 10}, {10, 20}});
    f.host.run("COPYCLIP", {ScriptItem::select({a, b}), ScriptItem::enter()});
    f.host.run("PASTEBLOCK", {"50,50"});
    REQUIRE(f.count(EntityKind::Insert) == 1);
    const BlockDef* blk = f.doc.blockByName("A$C1");
    REQUIRE(blk != nullptr);
    CHECK(blk->entities.size() == 2);
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e))
            CHECK(near(ins->position, {50, 50}));
    });
}

TEST_CASE("blocks referenced by copied inserts travel with the clip") {
    Fixture src;
    BlockId blk;
    {
        Transaction tx = src.doc.begin("blk");
        BlockDef def;
        def.name = "BOLT";
        auto id = tx.addBlock(def);
        REQUIRE(id.ok());
        blk = id.value();
        Entity m;
        m.data = CircleData{{0, 0}, 2};
        REQUIRE(tx.add(std::move(m), blk).ok());
        tx.commit();
    }
    InsertData ins;
    ins.block = blk;
    ins.position = {5, 5};
    EntityId i = src.add(ins);
    src.host.run("COPYCLIP", {ScriptItem::select({i}), ScriptItem::enter()});
    Fixture dst;
    dst.host.run("PASTECLIP", {"0,0"});
    const BlockDef* made = dst.doc.blockByName("BOLT");
    REQUIRE(made != nullptr);
    CHECK(made->entities.size() == 1);
    CHECK(dst.count(EntityKind::Insert) == 1);
}

TEST_CASE("nested blocks and a big selection copy, paste and undo cleanly") {
    Fixture src;
    BlockId inner, outer;
    {
        Transaction tx = src.doc.begin("blk");
        BlockDef a;
        a.name = "INNER";
        inner = tx.addBlock(a).value();
        Entity m;
        m.data = CircleData{{0, 0}, 2};
        REQUIRE(tx.add(std::move(m), inner).ok());
        BlockDef b;
        b.name = "OUTER";
        outer = tx.addBlock(b).value();
        InsertData in;
        in.block = inner;
        in.position = {3, 0};
        Entity e;
        e.data = in;
        REQUIRE(tx.add(std::move(e), outer).ok());
        tx.commit();
    }
    std::vector<EntityId> ids;
    for (int i = 0; i < 3000; ++i) {
        InsertData ins;
        ins.block = (i % 2) ? outer : inner;
        ins.position = {double(i), 0};
        ids.push_back(src.add(ins));
    }
    src.host.run("COPYCLIP", {ScriptItem::select(ids), ScriptItem::enter()});
    Fixture dst;
    CHECK(dst.host.run("PASTECLIP", {"0,0"}).finished);
    // 3000 placed inserts plus the one inside OUTER.
    CHECK(dst.count(EntityKind::Insert) == 3001);
    REQUIRE(dst.doc.blockByName("OUTER") != nullptr);
    REQUIRE(dst.doc.blockByName("INNER") != nullptr);
    // A second paste reuses the blocks it already made.
    CHECK(dst.host.run("PASTECLIP", {"0,50"}).finished);
    CHECK(dst.count(EntityKind::Insert) == 6001);
    std::size_t outers = 0;
    for (const auto& kv : dst.doc.blocks())
        outers += kv.second.name == "OUTER" ? 1 : 0;
    CHECK(outers == 1);
    dst.host.undo();
    CHECK(dst.count(EntityKind::Insert) == 3001);
    dst.host.undo();
    CHECK(dst.count(EntityKind::Insert) == 0);
    dst.host.redo();
    CHECK(dst.count(EntityKind::Insert) == 3001);
}

TEST_CASE("WBLOCK writes the selection to a drawing file at the base point") {
    Fixture f;
    LineData l;
    l.a = {100, 100};
    l.b = {150, 100};
    EntityId a = f.add(l);
    const auto path = std::filesystem::temp_directory_path() / "tbcad_wblock_test.tbcad";
    std::filesystem::remove(path);
    auto r = f.host.run("WBLOCK",
                        {ScriptItem::select({a}), ScriptItem::enter(), "100,100", path.string()});
    CHECK(r.finished);
    io::IoReport report;
    auto doc = io::readTbcad(path, report);
    REQUIRE(doc);
    std::size_t n = 0;
    doc.value()->forEachEntity([&](const Entity& e) {
        const auto* ln = entityAs<LineData>(e);
        REQUIRE(ln);
        CHECK(near(ln->a, {0, 0}));
        CHECK(near(ln->b, {50, 0}));
        ++n;
    });
    CHECK(n == 1);
    std::filesystem::remove(path);
}

TEST_CASE("plot style names of objects and new layers go with a copy") {
    Fixture src;
    Layer lay;
    lay.name = "WALLS";
    LayerId walls;
    {
        Transaction tx = src.doc.begin("layer");
        walls = tx.addLayer(lay).value();
        tx.commit();
    }
    EntityId a = src.add(LineData{{0, 0}, {10, 0}}, walls);
    EntityId b = src.add(LineData{{0, 5}, {10, 5}}, walls);
    {
        Transaction tx = src.doc.begin("styles");
        (void)tx.modifyHeader([&](Header& h) {
            setLayerPlotStyle(h, walls, "Heavy");
            setEntityPlotStyle(h, b, "Thin Red");
        });
        tx.commit();
    }
    src.host.run("COPYCLIP", {ScriptItem::select({a, b}), ScriptItem::enter()});

    Fixture dst;
    dst.host.run("PASTECLIP", {"0,0"});
    const Layer* layer = dst.doc.layerByName("WALLS");
    REQUIRE(layer);
    CHECK(layerPlotStyle(dst.doc.header(), layer->id) == "Heavy");
    std::vector<std::string> names;
    for (EntityId id : dst.doc.modelSpace())
        names.push_back(resolvedPlotStyle(dst.doc.header(), id, dst.get(id).common.layer));
    std::sort(names.begin(), names.end());
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Heavy");
    CHECK(names[1] == "Thin Red");
}

TEST_CASE("PASTESPEC pastes as a block, at the original place or on the current layer") {
    Fixture f;
    Layer lay;
    lay.name = "OTHER";
    LayerId other;
    {
        Transaction tx = f.doc.begin("layer");
        other = tx.addLayer(lay).value();
        tx.commit();
    }
    EntityId a = f.add(LineData{{10, 10}, {20, 10}}, other);
    f.host.run("COPYBASE", {ScriptItem::select({a}), ScriptItem::enter(), "10,10"});
    {
        Transaction tx = f.doc.begin("current");
        (void)tx.modifyHeader([](Header& h) { h.currentLayer = kLayerZero; });
        tx.commit();
    }
    CHECK(f.host.run("PASTESPEC", {"Block", "50,50"}).finished);
    CHECK(f.count(EntityKind::Insert) == 1);
    CHECK(f.host.run("PASTESPEC", {"Original"}).finished);
    std::size_t inPlace = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e);
            l && near(l->a, {10, 10}) && !f.doc.ownerBlock(e.common.id))
            ++inPlace;
    });
    CHECK(inPlace == 2);
    CHECK(f.host.run("PASTESPEC", {"CurrentLayer", "0,0"}).finished);
    bool onZero = false;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e); l && near(l->a, {0, 0}))
            onZero = e.common.layer == kLayerZero;
    });
    CHECK(onZero);
}

TEST_CASE("PASTESPEC makes text and a hyperlink from typed or pasted text, even with no clip") {
    Fixture f;
    CHECK(f.host.run("PASTESPEC", {"Text", "Hello\\Pworld", "5,5"}).finished);
    CHECK(f.count(EntityKind::MText) == 1);
    CHECK(f.host.run("PASTESPEC", {"Hyperlink", "https://example.org", "9,9"}).finished);
    CHECK(f.count(EntityKind::MText) == 2);
    std::size_t links = 0;
    for (const auto& kv : f.doc.header().custom)
        if (kv.first.rfind("tbcad.link.", 0) == 0)
            ++links;
    CHECK(links == 1);
    f.host.undo();
    CHECK(f.count(EntityKind::MText) == 1);
    CHECK(f.host.run("PASTESPEC", {"Image"}).finished);
}

TEST_CASE("a drawing kept in inches pastes into a millimetre drawing at the same real size") {
    Fixture src;
    {
        Transaction tx = src.doc.begin("units");
        (void)tx.modifyHeader([](Header& h) { h.units = LengthUnit::Inch; });
        tx.commit();
    }
    EntityId a = src.add(LineData{{0, 0}, {25.4, 0}});  // one inch, stored in millimetres
    src.host.run("COPYCLIP", {ScriptItem::select({a}), ScriptItem::enter()});
    Fixture dst;
    dst.host.run("PASTECLIP", {"0,0"});
    double length = 0;
    dst.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e))
            length = distance(l->a, l->b);
    });
    CHECK(length == doctest::Approx(25.4));
}

namespace {
// A block "Dyn" with one stretchable bar, an assembly binding to a nested insert, and one insert.
EntityId makeDynamicInsert(Fixture& f, bool padIds) {
    if (padIds) {  // the destination drawing already uses small ids, so the clip's ids clash
        for (int i = 0; i < 7; ++i)
            f.add(PointData{{double(i), 0}});
    }
    BlockId inner, blk;
    EntityId bar, nested;
    {
        Transaction tx = f.doc.begin("blocks");
        inner = tx.addBlock(BlockDef{.name = "Inner"}).value();
        blk = tx.addBlock(BlockDef{.name = "Dyn"}).value();
        Entity b;
        b.data = LineData{{0, 0}, {10, 0}};
        bar = tx.add(std::move(b), blk).value();
        Entity n;
        n.data = InsertData{inner, {20, 0}, {1, 1}, 0};
        nested = tx.add(std::move(n), blk).value();
        DynamicDef dd;
        DynParam w;
        w.name = "Length";
        w.base = {0, 0};
        w.end = {10, 0};
        dd.params.push_back(w);
        DynAction st;
        st.kind = DynAction::Kind::Stretch;
        st.frameMin = {9, -1};
        st.frameMax = {11, 1};
        st.objects = {bar};
        dd.actions.push_back(st);
        REQUIRE(tx.modifyBlock(blk, [&](BlockDef& d) {
                    d.dynamic = std::move(dd);
                    AssemblyDef as;
                    AsmBinding bind;
                    bind.kind = AsmBinding::Kind::Nested;
                    bind.entity = nested;
                    bind.target = "X";
                    bind.formula = "1";
                    as.bindings.push_back(bind);
                    d.assembly = std::move(as);
                }).ok());
        tx.commit();
    }
    return f.add(InsertData{blk, {50, 50}, {1, 1}, 0});
}
}  // namespace

TEST_CASE("a dynamic block pasted into another drawing keeps its action and binding targets") {
    Fixture src;
    const EntityId ins = makeDynamicInsert(src, false);
    src.host.run("COPYCLIP", {ScriptItem::select({ins}), ScriptItem::enter()});
    Fixture dst;
    for (int i = 0; i < 7; ++i)
        dst.add(PointData{{double(i), 0}});  // ids 1..7 are taken: the clip's ids would clash
    dst.host.run("PASTECLIP", {"0,0"});
    const BlockDef* b = dst.doc.blockByName("Dyn");
    REQUIRE(b != nullptr);
    REQUIRE(b->dynamic.has_value());
    REQUIRE(b->dynamic->actions.size() == 1);
    REQUIRE(b->dynamic->actions[0].objects.size() == 1);
    const EntityId target = b->dynamic->actions[0].objects[0];
    // The action still points at the bar inside the pasted block, not at some other object.
    CHECK(std::find(b->entities.begin(), b->entities.end(), target) != b->entities.end());
    const Entity* t = dst.doc.find(target);
    REQUIRE(t != nullptr);
    CHECK(entityAs<LineData>(*t) != nullptr);
    REQUIRE(b->assembly.has_value());
    REQUIRE(b->assembly->bindings.size() == 1);
    const Entity* nested = dst.doc.find(b->assembly->bindings[0].entity);
    REQUIRE(nested != nullptr);
    CHECK(entityAs<InsertData>(*nested) != nullptr);
    CHECK(std::find(b->entities.begin(), b->entities.end(), b->assembly->bindings[0].entity) !=
          b->entities.end());
}

TEST_CASE("a dimension and a multileader pasted elsewhere carry no stale links") {
    Fixture src;
    const EntityId line = src.add(LineData{{0, 0}, {10, 0}});
    src.host.run("DIMLINEAR", {"0,0", "10,0", "5,5"});
    EntityId dim{};
    src.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dim = e.common.id;
    });
    REQUIRE(dim);
    // Link the dimension to the line as an associative dimension would be.
    {
        Transaction tx = src.doc.begin("assoc");
        REQUIRE(tx.modify(dim, [&](Entity& e) {
                    auto* d = entityAs<DimensionData>(e);
                    d->assoc[0] = DimAssoc{DimAssoc::Kind::Vertex, line, 0};
                    d->assoc[1] = DimAssoc{DimAssoc::Kind::Vertex, line, 1};
                }).ok());
        tx.commit();
    }
    BlockId marker;
    {
        Transaction tx = src.doc.begin("blk");
        marker = tx.addBlock(BlockDef{.name = "MARK"}).value();
        Entity c;
        c.data = CircleData{{0, 0}, 1};
        REQUIRE(tx.add(std::move(c), marker).ok());
        tx.commit();
    }
    MLeaderData ml;
    ml.lines = {{{0, 20}, {10, 30}}};
    ml.landing = {10, 30};
    ml.contentPos = {15, 30};
    ml.content = MLeaderData::Content::Block;
    ml.block = marker;
    const EntityId mlid = src.add(Indirect<MLeaderData>(ml));
    src.host.run("COPYCLIP", {ScriptItem::select({dim, mlid}), ScriptItem::enter()});

    Fixture dst;
    for (int i = 0; i < 7; ++i)
        dst.add(LineData{{double(i), 5}, {double(i) + 1, 5}});  // lines that ids could hit
    dst.host.run("PASTECLIP", {"0,0"});
    bool seenDim = false, seenMl = false;
    dst.doc.forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<DimensionData>(e)) {
            seenDim = true;
            for (const DimAssoc& a : d->assoc)
                CHECK(a.kind == DimAssoc::Kind::None);  // the link to the source drawing is cut
        }
        if (const auto* m = entityAs<MLeaderData>(e)) {
            seenMl = true;
            const BlockDef* b = dst.doc.block(m->block);
            REQUIRE(b != nullptr);
            CHECK(b->name == "MARK");  // the block came along and is the destination's own row
        }
    });
    CHECK(seenDim);
    CHECK(seenMl);
}

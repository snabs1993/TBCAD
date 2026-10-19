#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

TEST_CASE("EXPLODE turns an insert back into its geometry - undo restores the insert") {
    Fixture f;
    EntityId a = f.addLine(10, 10, 20, 10);
    EntityId b = f.addCircle(15, 15, 2);
    f.host->setSelection({a, b});
    f.host->run("BLOCK", {"Door", "10,10", "C"});
    f.host->run("INSERT", {"Door", "100,100"});
    EntityId ins = f.modelSpace().back();
    f.host->setSelection({ins});
    auto r = f.host->run("EXPLODE", {});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 3);  // the first insert (from BLOCK) plus the two parts
    CHECK_FALSE(f.doc.contains(ins));
    bool line = false, circle = false;
    for (EntityId id : f.modelSpace()) {
        if (const auto* l = entityAs<LineData>(*f.doc.find(id))) {
            line = true;
            CHECK(near(l->a, {100, 100}));
            CHECK(near(l->b, {110, 100}));
        }
        if (const auto* c = entityAs<CircleData>(*f.doc.find(id))) {
            circle = true;
            CHECK(near(c->center, {105, 105}));
        }
    }
    CHECK(line);
    CHECK(circle);
    CHECK(f.host->undo());
    CHECK(f.doc.contains(ins));
    CHECK(f.modelSpace().size() == 2);
}

TEST_CASE("EXPLODE a polyline and report dropped widths") {
    Fixture f;
    Entity pl;
    pl.data = PolylineData{{{{0, 0}, 0, 1, 1}, {{10, 0}, 0, 1, 1}, {{10, 10}, 0, 0, 0}}, false, 0};
    EntityId id = f.add(pl);
    f.host->setSelection({id});
    f.host->run("EXPLODE", {});
    CHECK(f.modelSpace().size() == 2);
    CHECK(f.host->logContains("widths were dropped"));
}

TEST_CASE("EXPLODE ignores what cannot be exploded") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    auto r = f.host->run("EXPLODE", {cmd::ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.doc.contains(c));
}

TEST_CASE("EXPLODE breaks a pattern hatch into lines") {
    Fixture f;
    HatchLoop loop;
    loop.edges = {HatchLineEdge{{0, 0}, {10, 0}}, HatchLineEdge{{10, 0}, {10, 10}},
                  HatchLineEdge{{10, 10}, {0, 10}}, HatchLineEdge{{0, 10}, {0, 0}}};
    HatchData hd;
    hd.loops = {loop};
    hd.solid = false;
    hd.pattern = "LINE45";
    hd.patternScale = 1;
    Entity e;
    e.data = Indirect<HatchData>(hd);
    EntityId id = f.add(e);
    f.host->setSelection({id});
    f.host->run("EXPLODE", {});
    CHECK_FALSE(f.doc.contains(id));
    CHECK(f.modelSpace().size() > 1);
}

TEST_CASE("NCOPY copies the nested object nearest to the pick out of a block") {
    Fixture f;
    EntityId a = f.addLine(10, 10, 20, 10);
    EntityId b = f.addCircle(15, 15, 2);
    f.host->setSelection({a, b});
    f.host->run("BLOCK", {"Door", "10,10", "C"});
    f.host->run("INSERT", {"Door", "100,100"});
    EntityId ins = f.modelSpace().back();
    const std::size_t before = f.modelSpace().size();
    // Pick the circle (now centred at 105,105) and move the copy by 0,50.
    auto r = f.host->run("NCOPY", {cmd::ScriptItem::pick(ins, {107, 105}), "0,0", "0,50"});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == before + 1);
    const auto* c = entityAs<CircleData>(*f.doc.find(f.modelSpace().back()));
    REQUIRE(c);
    CHECK(near(c->center, {105, 155}));
    CHECK(f.doc.contains(ins));  // the block itself is untouched
    CHECK(f.host->undo());
    CHECK(f.modelSpace().size() == before);

    // Picking near the line takes the line; Enter at the base point copies in place.
    auto r2 =
        f.host->run("NCOPY", {cmd::ScriptItem::pick(ins, {105, 100}), cmd::ScriptItem::enter()});
    CHECK(r2.finished);
    REQUIRE(f.modelSpace().size() == before + 1);
    const auto* l = entityAs<LineData>(*f.doc.find(f.modelSpace().back()));
    REQUIRE(l);
    CHECK(near(l->a, {100, 100}));
}

TEST_CASE("XPLODE explodes and gives the parts the chosen colour") {
    Fixture f;
    EntityId a = f.addLine(10, 10, 20, 10);
    f.host->setSelection({a});
    f.host->run("BLOCK", {"Door", "10,10", "C"});
    f.host->run("INSERT", {"Door", "100,100"});
    EntityId ins = f.modelSpace().back();
    f.host->setSelection({ins});
    CHECK(f.host->run("XPLODE", {"Color", "5", "Explode"}).finished);
    CHECK_FALSE(f.doc.contains(ins));
    bool found = false;
    for (EntityId id : f.modelSpace())
        if (entityAs<LineData>(*f.doc.find(id))) {
            found = true;
            CHECK(f.doc.find(id)->common.color == Color::index(5));
        }
    CHECK(found);
}

TEST_CASE("BURST breaks an insert and keeps its attribute values as text and leaves polylines alone") {
    Fixture f;
    BlockId b{};
    {
        Transaction tx = f.doc.begin("block");
        b = tx.addBlock(BlockDef{.name = "Tag", .base = {0, 0}}).value();
        Entity l;
        l.data = LineData{{0, 0}, {5, 0}};
        REQUIRE(tx.add(l, b));
        TextData td;
        td.position = {0, 2};
        td.utf8 = "NO";
        td.attdef = AttDefInfo{"NO", "", "none", false, false, false, false};
        Entity t;
        t.data = Indirect<TextData>(std::move(td));
        REQUIRE(tx.add(t, b));
        InsertData ins{b, {50, 50}, {1, 1}, 0};
        ins.attribs = {AttribValue{"NO", "A-17", {}}};
        Entity e;
        e.data = Indirect<InsertData>(std::move(ins));
        REQUIRE(tx.add(e));
        PolylineData pl;
        pl.v = {{{0, 0}, 0, 0, 0}, {{5, 0}, 0, 0, 0}, {{5, 5}, 0, 0, 0}};
        Entity p;
        p.data = pl;
        REQUIRE(tx.add(p));
        tx.commit();
    }
    std::vector<EntityId> all = f.modelSpace();
    REQUIRE(all.size() == 2);
    f.host->setSelection(all);
    CHECK(f.host->run("BURST", {}).finished);
    bool value = false, polyline = false, line = false;
    for (EntityId id : f.modelSpace()) {
        const Entity& e = *f.doc.find(id);
        if (const auto* t = entityAs<TextData>(e)) {
            value = t->utf8 == "A-17" && !t->attdef;
        } else if (entityAs<PolylineData>(e)) {
            polyline = true;
        } else if (const auto* l = entityAs<LineData>(e)) {
            line = true;
            CHECK(near(l->a, {50, 50}));
        }
    }
    CHECK(value);
    CHECK(polyline);  // BURST does not touch it
    CHECK(line);
    CHECK(f.modelSpace().size() == 3);
    CHECK(f.host->undo());
    CHECK(f.modelSpace().size() == 2);
}

TEST_CASE("EXPLODE turns attributes back into definitions that show their tags") {
    Fixture f;
    BlockId b{};
    {
        Transaction tx = f.doc.begin("block");
        b = tx.addBlock(BlockDef{.name = "Tag", .base = {0, 0}}).value();
        Entity l;
        l.data = LineData{{0, 0}, {5, 0}};
        REQUIRE(tx.add(l, b));
        TextData td;
        td.position = {0, 2};
        td.utf8 = "NO";
        td.attdef = AttDefInfo{"NO", "", "none", false, false, false, false};
        Entity t;
        t.data = Indirect<TextData>(std::move(td));
        REQUIRE(tx.add(t, b));
        InsertData ins{b, {50, 50}, {1, 1}, 0};
        ins.attribs = {AttribValue{"NO", "A-17", {}}};
        Entity e;
        e.data = Indirect<InsertData>(std::move(ins));
        REQUIRE(tx.add(e));
        PolylineData pl;
        pl.v = {{{0, 0}, 0, 0, 0}, {{5, 0}, 0, 0, 0}, {{5, 5}, 0, 0, 0}};
        Entity p;
        p.data = pl;
        REQUIRE(tx.add(p));
        tx.commit();
    }
    std::vector<EntityId> all = f.modelSpace();
    REQUIRE(all.size() == 2);
    f.host->setSelection(all);
    CHECK(f.host->run("EXPLODE", {}).finished);
    bool def = false, polyline = false, line = false;
    for (EntityId id : f.modelSpace()) {
        const Entity& e = *f.doc.find(id);
        if (const auto* t = entityAs<TextData>(e)) {
            def = t->attdef && t->attdef->tag == "NO" && t->utf8 == "NO";
        } else if (entityAs<PolylineData>(e)) {
            polyline = true;
        } else if (entityAs<LineData>(e)) {
            line = true;
        }
    }
    CHECK(def);
    CHECK(line);
    CHECK(f.modelSpace().size() == 4);  // line, definition, and the polyline's two segments
    (void)polyline;
    CHECK(f.host->undo());
    CHECK(f.modelSpace().size() == 2);
}

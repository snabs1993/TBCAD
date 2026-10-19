#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "tbcad/core/core.h"

using namespace tbcad;

namespace {

Entity line(double x1, double y1, double x2, double y2) {
    return Entity{{}, LineData{{x1, y1}, {x2, y2}}};
}

struct CountingObserver : DocumentObserver {
    int calls = 0;
    std::size_t lastEntityChanges = 0;
    void onChanged(const ChangeSet& cs) override {
        ++calls;
        lastEntityChanges = cs.entities.size();
    }
};

EntityId addLine(Document& doc, double x = 0) {
    Transaction tx = doc.begin("add");
    auto id = tx.add(line(x, 0, x + 1, 1));
    REQUIRE(id);
    tx.commit();
    return id.value();
}

}  // namespace

TEST_CASE("a new document has the built-in rows") {
    Document doc;
    REQUIRE(doc.layer(kLayerZero));
    CHECK(doc.layer(kLayerZero)->name == "0");
    CHECK(doc.layerByName("0") == doc.layer(kLayerZero));
    REQUIRE(doc.linetype(kLinetypeContinuous));
    CHECK(doc.linetype(kLinetypeContinuous)->name == "Continuous");
    CHECK(doc.linetype(kLinetypeContinuous)->pattern.empty());
    CHECK(doc.textStyleByName("standard") != nullptr);
    CHECK(doc.dimStyleByName("STANDARD") != nullptr);
    CHECK(doc.header().workArea == 1e8);
    CHECK(doc.header().units == LengthUnit::Millimeter);
    CHECK(doc.header().currentLayer == kLayerZero);
    CHECK(doc.entityCount() == 0);
    CHECK(doc.dimStyle(kDimStyleStandard)->textHeight == 2.5);
    CHECK(doc.dimStyle(kDimStyleStandard)->arrowSize == 2.5);
    CHECK(doc.dimStyle(kDimStyleStandard)->precision == 2);
}

TEST_CASE("add modify erase") {
    Document doc;
    Transaction tx = doc.begin("edit");
    auto a = tx.add(line(0, 0, 10, 0));
    auto b = tx.add(line(0, 5, 10, 5));
    REQUIRE((a && b));
    CHECK(a.value() != b.value());
    CHECK(bool(a.value()));
    CHECK(doc.entityCount() == 2);  // visible inside the transaction
    REQUIRE(tx.modify(a.value(), [](Entity& e) { std::get<LineData>(e.data).b.x = 20; }));
    REQUIRE(tx.erase(b.value()));
    ChangeSet cs = tx.commit();
    CHECK(doc.entityCount() == 1);
    REQUIRE(doc.find(a.value()));
    CHECK(entityAs<LineData>(*doc.find(a.value()))->b.x == 20);
    CHECK(doc.find(b.value()) == nullptr);
    // a was added in this transaction, b added and erased: one net change.
    CHECK(cs.entities.size() == 1);
    CHECK(cs.name == "edit");
    // New entities get the current layer.
    CHECK(doc.find(a.value())->common.layer == kLayerZero);
}

TEST_CASE("ids are never reused") {
    Document doc;
    EntityId first = addLine(doc);
    {
        Transaction tx = doc.begin("erase");
        REQUIRE(tx.erase(first));
        tx.commit();
    }
    EntityId second = addLine(doc);
    CHECK(second.value > first.value);
    Transaction rolled = doc.begin("rolled back");
    auto third = rolled.add(line(0, 0, 1, 1));
    rolled.rollback();
    EntityId fourth = addLine(doc);
    CHECK(fourth.value > third.value().value);
}

TEST_CASE("undo and redo restore a byte-identical state") {
    Document doc;
    EntityId keep = addLine(doc, 5);
    EntityId gone = addLine(doc, 10);
    std::string before = canonicalDump(doc);

    ChangeSet cs;
    {
        Transaction tx = doc.begin("mixed");
        auto added = tx.add(Entity{{}, CircleData{{1, 1}, 2}});
        REQUIRE(added);
        REQUIRE(tx.modify(keep, [](Entity& e) {
            e.common.color = Color::index(1);
            std::get<LineData>(e.data).a = {-3, -3};
        }));
        REQUIRE(tx.erase(gone));
        REQUIRE(tx.addLayer(Layer{.name = "Walls", .color = Color::index(3)}));
        REQUIRE(tx.modifyHeader([](Header& h) { h.createdBy = "test"; }));
        REQUIRE(tx.bringToFront(keep));
        cs = tx.commit();
    }
    std::string after = canonicalDump(doc);
    CHECK(before != after);

    doc.apply(cs, Direction::Undo);
    CHECK(canonicalDump(doc) == before);
    doc.apply(cs, Direction::Redo);
    CHECK(canonicalDump(doc) == after);
    doc.apply(cs, Direction::Undo);
    CHECK(canonicalDump(doc) == before);
    doc.apply(cs, Direction::Redo);
    CHECK(canonicalDump(doc) == after);
}

TEST_CASE("an uncommitted transaction rolls back in its destructor") {
    Document doc;
    EntityId keep = addLine(doc);
    EntityId erased = addLine(doc, 3);
    std::string before = canonicalDump(doc);
    CountingObserver obs;
    doc.addObserver(&obs);
    {
        Transaction tx = doc.begin("abandoned");
        REQUIRE(tx.add(line(7, 7, 8, 8)));
        REQUIRE(tx.modify(keep, [](Entity& e) { e.common.visible = false; }));
        REQUIRE(tx.erase(erased));
        REQUIRE(tx.addLayer(Layer{.name = "Temp"}));
        REQUIRE(tx.addBlock(BlockDef{.name = "B"}));
        REQUIRE(tx.modifyHeader([](Header& h) { h.ltScale = 5; }));
        CHECK(doc.entityCount() == 2);
    }
    CHECK(canonicalDump(doc) == before);
    CHECK(obs.calls == 0);
    CHECK(doc.layerByName("Temp") == nullptr);
    // A new transaction can start afterwards.
    Transaction again = doc.begin("again");
    again.commit();
    doc.removeObserver(&obs);
}

TEST_CASE("explicit rollback") {
    Document doc;
    std::string before = canonicalDump(doc);
    Transaction tx = doc.begin("x");
    REQUIRE(tx.add(line(0, 0, 1, 1)));
    tx.rollback();
    CHECK(canonicalDump(doc) == before);
    CHECK_THROWS_AS((void)tx.add(line(0, 0, 1, 1)), std::logic_error);
}

TEST_CASE("observers are called exactly once per commit undo and redo") {
    Document doc;
    CountingObserver obs;
    doc.addObserver(&obs);
    ChangeSet cs;
    {
        Transaction tx = doc.begin("three");
        for (int i = 0; i < 3; ++i)
            REQUIRE(tx.add(line(i, 0, i + 1, 1)));
        CHECK(obs.calls == 0);
        cs = tx.commit();
    }
    CHECK(obs.calls == 1);
    CHECK(obs.lastEntityChanges == 3);
    doc.apply(cs, Direction::Undo);
    CHECK(obs.calls == 2);
    CHECK(doc.entityCount() == 0);
    doc.apply(cs, Direction::Redo);
    CHECK(obs.calls == 3);
    CHECK(doc.entityCount() == 3);
    // An empty commit changes nothing and tells nobody.
    doc.begin("empty").commit();
    CHECK(obs.calls == 3);
    doc.removeObserver(&obs);
    addLine(doc);
    CHECK(obs.calls == 3);
}

TEST_CASE("observers see the transition that happened") {
    struct Watcher : DocumentObserver {
        Document* doc = nullptr;
        std::size_t countSeenInCallback = 0;
        void onChanged(const ChangeSet& cs) override {
            countSeenInCallback = doc->entityCount();
            (void)cs;
        }
    } w;
    Document doc;
    w.doc = &doc;
    doc.addObserver(&w);
    Transaction tx = doc.begin("a");
    REQUIRE(tx.add(line(0, 0, 1, 1)));
    ChangeSet cs = tx.commit();
    CHECK(w.countSeenInCallback == 1);  // already applied when called
    doc.apply(cs, Direction::Undo);
    CHECK(w.countSeenInCallback == 0);
    doc.removeObserver(&w);
}

TEST_CASE("only one transaction at a time") {
    Document doc;
    Transaction tx = doc.begin("first");
    CHECK_THROWS_AS(doc.begin("second"), std::logic_error);
    ChangeSet cs;
    CHECK_THROWS_AS(doc.apply(cs, Direction::Undo), std::logic_error);
    tx.commit();
    CHECK_NOTHROW(doc.begin("third").commit());
}

TEST_CASE("a moved transaction stays the open one") {
    Document doc;
    Transaction a = doc.begin("a");
    Transaction b = std::move(a);
    CHECK_THROWS_AS(doc.begin("c"), std::logic_error);
    REQUIRE(b.add(line(0, 0, 1, 1)));
    b.commit();
    CHECK(doc.entityCount() == 1);
}

TEST_CASE("modify that fails leaves the entity untouched") {
    Document doc;
    EntityId id = addLine(doc);
    std::string before = canonicalDump(doc);
    Transaction tx = doc.begin("bad");
    auto r = tx.modify(id, [](Entity& e) {
        std::get<LineData>(e.data).b.x = std::numeric_limits<double>::quiet_NaN();
    });
    CHECK_FALSE(r);
    CHECK(canonicalDump(doc) == before);
    auto r2 = tx.modify(id, [](Entity& e) { e.common.layer = LayerId{999}; });
    CHECK_FALSE(r2);
    CHECK_FALSE(tx.modify(EntityId{12345}, [](Entity&) {}));
    CHECK_FALSE(tx.erase(EntityId{12345}));
    ChangeSet cs = tx.commit();
    CHECK(cs.empty());
    CHECK(canonicalDump(doc) == before);
}

TEST_CASE("modify cannot change the id") {
    Document doc;
    EntityId id = addLine(doc);
    Transaction tx = doc.begin("m");
    REQUIRE(tx.modify(id, [](Entity& e) { e.common.id = EntityId{777}; }));
    tx.commit();
    CHECK(doc.find(id) != nullptr);
    CHECK(doc.find(EntityId{777}) == nullptr);
    CHECK(doc.find(id)->common.id == id);
}

TEST_CASE("change sets carry before and after snapshots") {
    Document doc;
    EntityId id = addLine(doc);
    Transaction tx = doc.begin("move");
    REQUIRE(tx.modify(id, [](Entity& e) { std::get<LineData>(e.data).a.x = 42; }));
    REQUIRE(tx.modify(id, [](Entity& e) { std::get<LineData>(e.data).a.y = 43; }));
    ChangeSet cs = tx.commit();
    REQUIRE(cs.entities.size() == 1);  // two edits of one entity merge
    const EntityChange& c = cs.entities[0];
    REQUIRE(cs.before(c));
    REQUIRE(cs.after(c));
    CHECK(entityAs<LineData>(*cs.before(c))->a == Vec2{0, 0});
    CHECK(entityAs<LineData>(*cs.after(c))->a == Vec2{42, 43});
    CHECK(cs.idOf(c) == id);
    ChangeSet inv = cs.inverted();
    CHECK(entityAs<LineData>(*inv.after(inv.entities[0]))->a == Vec2{0, 0});
}

TEST_CASE("layer 0 cannot be deleted or renamed") {
    Document doc;
    Transaction tx = doc.begin("layer0");
    auto del = tx.eraseLayer(kLayerZero);
    CHECK_FALSE(del);
    CHECK(del.error().code == ErrorCode::Conflict);
    auto ren = tx.modifyLayer(kLayerZero, [](Layer& l) { l.name = "Zero"; });
    CHECK_FALSE(ren);
    CHECK(doc.layer(kLayerZero)->name == "0");
    // Other properties of layer 0 can change.
    CHECK(tx.modifyLayer(kLayerZero, [](Layer& l) { l.color = Color::index(2); }));
    CHECK(doc.layer(kLayerZero)->color == Color::index(2));
}

TEST_CASE("deleting a layer in use fails") {
    Document doc;
    LayerId walls;
    {
        Transaction tx = doc.begin("layers");
        auto l = tx.addLayer(Layer{.name = "Walls"});
        REQUIRE(l);
        walls = l.value();
        Entity e = line(0, 0, 1, 1);
        e.common.layer = walls;
        REQUIRE(tx.add(e));
        tx.commit();
    }
    Transaction tx = doc.begin("del");
    auto r = tx.eraseLayer(walls);
    CHECK_FALSE(r);
    CHECK(r.error().code == ErrorCode::Conflict);
    CHECK(doc.layer(walls) != nullptr);
    tx.commit();

    // Empty layers can be deleted, and a deleted layer is not in use anywhere.
    LayerId unused;
    {
        Transaction t2 = doc.begin("add");
        unused = t2.addLayer(Layer{.name = "Unused"}).value();
        t2.commit();
    }
    Transaction t3 = doc.begin("del");
    CHECK(t3.eraseLayer(unused));
    CHECK(doc.layer(unused) == nullptr);
    CHECK(doc.layerByName("unused") == nullptr);
    t3.commit();
}

TEST_CASE("the current layer cannot be deleted") {
    Document doc;
    Transaction tx = doc.begin("cur");
    LayerId l = tx.addLayer(Layer{.name = "Cur"}).value();
    REQUIRE(tx.modifyHeader([&](Header& h) { h.currentLayer = l; }));
    CHECK_FALSE(tx.eraseLayer(l));
    // New entities now land on it.
    EntityId e = tx.add(line(0, 0, 1, 1)).value();
    CHECK(doc.find(e)->common.layer == l);
    tx.commit();
}

TEST_CASE("table names are unique case-insensitive and validated") {
    Document doc;
    Transaction tx = doc.begin("names");
    REQUIRE(tx.addLayer(Layer{.name = "Walls"}));
    auto dup = tx.addLayer(Layer{.name = "WALLS"});
    CHECK_FALSE(dup);
    CHECK(dup.error().code == ErrorCode::Conflict);
    CHECK_FALSE(tx.addLayer(Layer{.name = ""}));
    CHECK_FALSE(tx.addLayer(Layer{.name = "bad\nname"}));
    LayerId other = tx.addLayer(Layer{.name = "Doors"}).value();
    CHECK_FALSE(tx.modifyLayer(other, [](Layer& l) { l.name = "walls"; }));
    CHECK(tx.modifyLayer(other, [](Layer& l) { l.name = "DOORS"; }));  // same row, new case
    CHECK(doc.layerByName("doors")->name == "DOORS");
    CHECK_FALSE(tx.addLayer(Layer{.name = "x", .color = Color::byLayer()}));
    CHECK_FALSE(tx.addLayer(Layer{.name = "y", .lineweight = LineWeight::byBlock()}));
    CHECK_FALSE(tx.addLayer(Layer{.name = "z", .linetype = LinetypeId{99}}));
    tx.commit();
}

TEST_CASE("layers default to the Continuous linetype") {
    Document doc;
    Transaction tx = doc.begin("l");
    LayerId id = tx.addLayer(Layer{.name = "A"}).value();
    tx.commit();
    CHECK(doc.layer(id)->linetype == kLinetypeContinuous);
}

TEST_CASE("linetypes Continuous is protected patterns are validated") {
    Document doc;
    Transaction tx = doc.begin("lt");
    CHECK_FALSE(tx.eraseLinetype(kLinetypeContinuous));
    CHECK_FALSE(tx.modifyLinetype(kLinetypeContinuous, [](Linetype& l) { l.name = "X"; }));
    CHECK_FALSE(tx.modifyLinetype(kLinetypeContinuous, [](Linetype& l) { l.pattern = {1, -1}; }));
    CHECK_FALSE(tx.addLinetype(Linetype{.name = "Zero", .pattern = {0, 0}}));
    auto dashed =
        tx.addLinetype(Linetype{.name = "Dashed", .description = "d", .pattern = {6, -3}});
    REQUIRE(dashed);
    // An entity using it prevents deletion.
    Entity e = line(0, 0, 1, 1);
    e.common.linetype = dashed.value();
    REQUIRE(tx.add(e));
    CHECK_FALSE(tx.eraseLinetype(dashed.value()));
    tx.commit();
    CHECK(doc.linetypeByName("dashed")->pattern == std::vector<double>{6, -3});
}

TEST_CASE("text and dimension styles") {
    Document doc;
    Transaction tx = doc.begin("styles");
    CHECK_FALSE(tx.eraseTextStyle(kTextStyleStandard));
    CHECK_FALSE(tx.eraseDimStyle(kDimStyleStandard));
    CHECK_FALSE(tx.addTextStyle(TextStyle{.name = "Bad", .widthFactor = 0}));
    TextStyle thai;
    thai.name = "Thai";
    thai.fontFamily = "Noto Sans Thai";
    TextStyleId ts = tx.addTextStyle(thai).value();
    DimStyle ds;
    ds.name = "Arch";
    ds.arrowType = ArrowType::ArchTick;
    ds.textStyle = ts;
    DimStyleId dsid = tx.addDimStyle(ds).value();
    CHECK_FALSE(tx.eraseTextStyle(ts));  // used by the dim style
    Entity t{{}, TextData{.height = 3, .style = ts, .utf8 = "\xE0\xB8\xAA"}};
    REQUIRE(tx.add(t));
    Entity bad{{}, TextData{.height = 3, .style = TextStyleId{77}}};
    CHECK_FALSE(tx.add(bad));
    Entity dim{{}, DimensionData{.style = dsid}};
    EntityId dimId = tx.add(dim).value();
    CHECK_FALSE(tx.eraseDimStyle(dsid));
    REQUIRE(tx.erase(dimId));
    CHECK(tx.eraseDimStyle(dsid));
    tx.commit();
}

TEST_CASE("coordinates beyond the hard cap are rejected") {
    Document doc;
    Transaction tx = doc.begin("cap");
    auto r = tx.add(line(0, 0, 1.0000001e10, 0));
    CHECK_FALSE(r);
    CHECK(r.error().code == ErrorCode::OutOfRange);
    CHECK(tx.add(line(0, 0, 1e10, 0)));                          // exactly at the cap is fine
    CHECK_FALSE(tx.add(Entity{{}, CircleData{{9e9, 0}, 2e9}}));  // reaches beyond via the radius
    CHECK_FALSE(tx.add(line(0, 0, std::numeric_limits<double>::infinity(), 0)));
    CHECK_FALSE(tx.add(line(std::nan(""), 0, 1, 1)));
    // modify is guarded too
    EntityId id = tx.add(line(0, 0, 1, 1)).value();
    CHECK_FALSE(tx.modify(id, [](Entity& e) { std::get<LineData>(e.data).b = {0, -2e10}; }));
    CHECK(entityAs<LineData>(*doc.find(id))->b == Vec2{1, 1});
    tx.commit();
}

TEST_CASE("geometry beyond the work area is accepted and listed") {
    Document doc;
    Transaction tx = doc.begin("area");
    EntityId inside = tx.add(line(0, 0, 9e7, 0)).value();
    EntityId outside = tx.add(line(0, 0, 2e8, 0)).value();
    EntityId movedOut = tx.add(line(0, 0, 1, 1)).value();
    EntityId movedBack = tx.add(line(0, 0, 3e8, 0)).value();
    REQUIRE(tx.modify(movedOut, [](Entity& e) { std::get<LineData>(e.data).b = {0, 5e9}; }));
    REQUIRE(tx.modify(movedBack, [](Entity& e) { std::get<LineData>(e.data).b = {1, 1}; }));
    ChangeSet cs = tx.commit();
    CHECK(cs.outOfArea == std::vector<EntityId>{outside, movedOut});
    CHECK(doc.contains(inside));
    CHECK(doc.contains(outside));

    // The work area can be changed in the header; it is validated.
    Transaction t2 = doc.begin("hdr");
    CHECK_FALSE(t2.modifyHeader([](Header& h) { h.workArea = 2e10; }));
    CHECK_FALSE(t2.modifyHeader([](Header& h) { h.workArea = 0; }));
    CHECK(t2.modifyHeader([](Header& h) { h.workArea = 1e6; }));
    EntityId e = t2.add(line(0, 0, 2e6, 0)).value();
    ChangeSet cs2 = t2.commit();
    CHECK(cs2.outOfArea == std::vector<EntityId>{e});
}

TEST_CASE("arcs are normalized and never degenerate to circles") {
    Document doc;
    Transaction tx = doc.begin("arc");
    auto ok = tx.add(Entity{{}, ArcData{{0, 0}, 5, -kPi / 2, 3 * kPi}});
    REQUIRE(ok);
    const ArcData* a = entityAs<ArcData>(*doc.find(ok.value()));
    CHECK(a->startAngle == doctest::Approx(1.5 * kPi));
    CHECK(a->endAngle == doctest::Approx(kPi));
    CHECK_FALSE(tx.add(Entity{{}, ArcData{{0, 0}, 5, 1.0, 1.0}}));
    CHECK_FALSE(tx.add(Entity{{}, ArcData{{0, 0}, 5, 0.0, kTwoPi}}));
    CHECK_FALSE(tx.add(Entity{{}, ArcData{{0, 0}, 0, 0.0, 1}}));
    CHECK_FALSE(tx.add(Entity{{}, CircleData{{0, 0}, -1}}));
    tx.commit();
}

TEST_CASE("shape validation of the other entity kinds") {
    Document doc;
    Transaction tx = doc.begin("v");
    CHECK_FALSE(tx.add(Entity{{}, PolylineData{}}));  // no vertices
    CHECK(tx.add(Entity{{}, PolylineData{{{{0, 0}, 0.5, 0, 0}, {{1, 0}}}, false, 0}}));
    CHECK_FALSE(tx.add(Entity{{}, EllipseData{{0, 0}, {0, 0}, 0.5, 0, kTwoPi}}));
    CHECK_FALSE(tx.add(Entity{{}, EllipseData{{0, 0}, {2, 0}, 1.5, 0, kTwoPi}}));
    CHECK(tx.add(Entity{{}, EllipseData{{0, 0}, {2, 0}, 0.5, 0, kTwoPi}}));
    CHECK_FALSE(tx.add(Entity{{}, SplineData{.degree = 3}}));  // nothing to draw
    SplineData sp;
    sp.degree = 2;
    sp.control = {{0, 0}, {1, 1}, {2, 0}};
    sp.knots = {0, 0, 0, 1, 1, 1};
    CHECK(tx.add(Entity{{}, sp}));
    sp.knots = {0, 0, 1, 1};
    CHECK_FALSE(tx.add(Entity{{}, sp}));
    sp.knots = {0, 0, 0, 1, 1, 1};
    sp.weights = {1, 2};
    CHECK_FALSE(tx.add(Entity{{}, sp}));
    CHECK_FALSE(tx.add(Entity{{}, TextData{.height = 0}}));
    CHECK(tx.add(Entity{{}, MTextData{.height = 2.5, .width = 100, .utf8 = "x"}}));
    CHECK(tx.add(Entity{{}, PointData{{1, 2}}}));
    Entity badColor = line(0, 0, 1, 1);
    badColor.common.color = Color::index(0);
    CHECK_FALSE(tx.add(badColor));
    Entity badLt = line(0, 0, 1, 1);
    badLt.common.linetype = LinetypeId{55};
    CHECK_FALSE(tx.add(badLt));
    Entity byBlock = line(0, 0, 1, 1);
    byBlock.common.linetype = kLinetypeByBlock;
    byBlock.common.color = Color::byBlock();
    byBlock.common.lineweight = LineWeight::byBlock();
    CHECK(tx.add(byBlock));
    Entity badScale = line(0, 0, 1, 1);
    badScale.common.linetypeScale = 0;
    CHECK_FALSE(tx.add(badScale));
    tx.commit();
}

TEST_CASE("every entity kind round-trips through the document") {
    Document doc;
    Transaction tx = doc.begin("all");
    BlockId blk = tx.addBlock(BlockDef{.name = "B"}).value();
    std::vector<Entity> all;
    all.push_back(line(0, 0, 1, 1));
    all.push_back(Entity{{}, CircleData{{1, 2}, 3}});
    all.push_back(Entity{{}, ArcData{{1, 2}, 3, 0.5, 2.5}});
    all.push_back(
        Entity{{}, PolylineData{{{{0, 0}, 0.5, 1, 2}, {{1, 0}, 0, 0, 0}}, true, false, 0.1}});
    all.push_back(Entity{{}, PointData{{4, 5}}});
    all.push_back(Entity{{}, EllipseData{{0, 0}, {3, 1}, 0.4, 0.1, 3}});
    SplineData sp;
    sp.control = {{0, 0}, {1, 2}, {2, 2}, {3, 0}};
    sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    sp.fit = {{0, 0}, {3, 0}};
    all.push_back(Entity{{}, sp});
    all.push_back(Entity{{},
                         TextData{{1, 1},
                                  {2, 2},
                                  2.5,
                                  0.3,
                                  0.9,
                                  0.1,
                                  {},
                                  HAlign::Center,
                                  VAlign::Middle,
                                  "\xE0\xB8\x81\xE0\xB8\xB3"}});
    all.push_back(Entity{{}, MTextData{{0, 0}, 2.5, 50, 0.1, Attachment::MiddleCenter, {}, "m"}});
    HatchData h;
    h.solid = false;
    h.pattern = "ANSI31";
    h.loops.push_back(HatchLoop{{HatchLineEdge{{0, 0}, {1, 0}}, HatchArcEdge{{0, 0}, 1, 0, 1, true},
                                 HatchPolylineEdge{{{{0, 0}, 0.2, 0, 0}}, true}},
                                true});
    all.push_back(Entity{{}, h});
    all.push_back(Entity{{}, InsertData{blk, {1, 1}, {2, 2}, 0.5}});
    all.push_back(Entity{{},
                         DimensionData{DimKind::Aligned,
                                       0,
                                       {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}},
                                       {1, 1},
                                       true,
                                       "<>",
                                       {},
                                       12.5}});
    all.push_back(Entity{{}, LeaderData{{{0, 0}, {1, 1}}, {}, "note", {1, 1}}});
    ProxyData pr;
    pr.originalType = "WIPEOUT";
    pr.raw = {1, 2, 3};
    pr.graphics.items.push_back(DisplayPolyline{{{0, 0}, {1, 1}}, false, {}});
    all.push_back(Entity{{}, pr});

    for (Entity& e : all) {
        e.common.color = Color::rgb(10, 20, 30);
        e.common.lineweight = LineWeight::mm(0.35);
        e.common.linetypeScale = 2;
        auto id = tx.add(e);
        if (!id)
            FAIL(id.error().message);
    }
    ChangeSet cs = tx.commit();
    CHECK(doc.entityCount() == all.size());
    std::string dump = canonicalDump(doc);
    doc.apply(cs, Direction::Undo);
    CHECK(doc.entityCount() == 0);
    doc.apply(cs, Direction::Redo);
    CHECK(canonicalDump(doc) == dump);

    // Every kind shows up in the dump.
    for (const char* tag : {"LINE", "CIRCLE", "ARC", "POLYLINE", "POINT", "ELLIPSE", "SPLINE",
                            "TEXT", "MTEXT", "HATCH", "INSERT", "DIM", "LEADER", "PROXY"}) {
        CAPTURE(tag);
        CHECK(dump.find(std::string(tag) + " ") != std::string::npos);
    }
    // dumpEntity differs when any field differs.
    Entity a = line(0, 0, 1, 1);
    Entity b = a;
    entityAs<LineData>(b)->b.y = 1.0000000001;
    CHECK(dumpEntity(a) != dumpEntity(b));
}

TEST_CASE("blocks entities live in the same store owned by the block") {
    Document doc;
    BlockId blk;
    EntityId inBlock, model;
    {
        Transaction tx = doc.begin("blk");
        blk = tx.addBlock(BlockDef{.name = "Door", .base = {1, 1}}).value();
        inBlock = tx.add(line(0, 0, 1, 0), blk).value();
        REQUIRE(tx.add(Entity{{}, CircleData{{0, 0}, 1}}, blk));
        model = tx.add(Entity{{}, InsertData{blk, {10, 10}, {1, 1}, 0}}).value();
        tx.commit();
    }
    CHECK(doc.entityCount() == 3);
    CHECK(doc.modelSpaceCount() == 1);
    CHECK(doc.modelSpace() == std::vector<EntityId>{model});
    CHECK(doc.ownerBlock(inBlock) == blk);
    CHECK(doc.ownerBlock(model) == BlockId{});
    CHECK(doc.block(blk)->entities.size() == 2);
    CHECK(doc.blockByName("DOOR") == doc.block(blk));
    CHECK(doc.block(blk)->base == Vec2{1, 1});

    Transaction tx = doc.begin("del");
    auto r = tx.eraseBlock(blk);  // an insert uses it
    CHECK_FALSE(r);
    CHECK(r.error().code == ErrorCode::Conflict);
    REQUIRE(tx.erase(model));
    std::string before = canonicalDump(doc);
    REQUIRE(tx.eraseBlock(blk));
    CHECK(doc.entityCount() == 0);
    ChangeSet cs = tx.commit();
    CHECK(doc.block(blk) == nullptr);
    doc.apply(cs, Direction::Undo);
    CHECK(doc.entityCount() == 3);
    CHECK(doc.ownerBlock(inBlock) == blk);
    CHECK(doc.modelSpaceCount() == 1);
    doc.apply(cs, Direction::Redo);
    CHECK(doc.entityCount() == 0);
    CHECK(doc.modelSpaceCount() == 0);
}

TEST_CASE("block edits roll back and undo cleanly") {
    Document doc;
    BlockId blk;
    EntityId a;
    {
        Transaction tx = doc.begin("blk");
        blk = tx.addBlock(BlockDef{.name = "B"}).value();
        a = tx.add(line(0, 0, 1, 1), blk).value();
        tx.commit();
    }
    std::string before = canonicalDump(doc);
    {
        Transaction tx = doc.begin("abandoned");
        REQUIRE(tx.add(line(5, 5, 6, 6), blk));
        REQUIRE(tx.erase(a));
        // modifyBlock cannot rewrite the member list
        REQUIRE(tx.modifyBlock(blk, [](BlockDef& b) {
            b.entities.clear();
            b.name = "Renamed";
        }));
        CHECK(doc.block(blk)->name == "Renamed");
        CHECK(doc.block(blk)->entities.size() == 1);  // `a` is gone, the new line is the member
    }
    CHECK(canonicalDump(doc) == before);
    CHECK(doc.ownerBlock(a) == blk);
}

TEST_CASE("inserts must not create block cycles or reference unknown blocks") {
    Document doc;
    Transaction tx = doc.begin("cycle");
    BlockId a = tx.addBlock(BlockDef{.name = "A"}).value();
    BlockId b = tx.addBlock(BlockDef{.name = "B"}).value();
    CHECK(tx.add(Entity{{}, InsertData{b, {0, 0}, {1, 1}, 0}}, a));        // A contains B
    CHECK_FALSE(tx.add(Entity{{}, InsertData{a, {0, 0}, {1, 1}, 0}}, b));  // B contains A: cycle
    CHECK_FALSE(tx.add(Entity{{}, InsertData{a, {0, 0}, {1, 1}, 0}}, a));  // direct recursion
    CHECK_FALSE(tx.add(Entity{{}, InsertData{BlockId{99}, {0, 0}, {1, 1}, 0}}));
    CHECK_FALSE(tx.add(line(0, 0, 1, 1), BlockId{99}));
    tx.commit();
}

TEST_CASE("draw order inside a block is the order of its list, and undo restores it") {
    Document doc;
    BlockId blk;
    EntityId a, b, c;
    {
        Transaction tx = doc.begin("block");
        blk = tx.addBlock(BlockDef{.name = "B"}).value();
        Entity e;
        e.data = LineData{{0, 0}, {1, 0}};
        a = tx.add(e, blk).value();
        b = tx.add(e, blk).value();
        c = tx.add(e, blk).value();
        tx.commit();
    }
    CHECK(doc.block(blk)->entities == std::vector<EntityId>{a, b, c});
    ChangeSet front;
    {
        Transaction tx = doc.begin("front");
        REQUIRE(tx.bringToFront(a));
        REQUIRE(tx.sendToBack(c));
        front = tx.commit();
    }
    CHECK(doc.block(blk)->entities == std::vector<EntityId>{c, b, a});
    CHECK(doc.modelSpace().empty());
    doc.apply(front, Direction::Undo);
    CHECK(doc.block(blk)->entities == std::vector<EntityId>{a, b, c});
    {
        Transaction tx = doc.begin("order");
        REQUIRE(tx.setDrawOrder({b, c, a}, blk));
        REQUIRE_FALSE(tx.setDrawOrder({b, c}, blk));
        tx.commit();
    }
    CHECK(doc.block(blk)->entities == std::vector<EntityId>{b, c, a});
}

TEST_CASE("draw order creation order bring to front send to back") {
    Document doc;
    EntityId a = addLine(doc, 0);
    EntityId b = addLine(doc, 1);
    EntityId c = addLine(doc, 2);
    CHECK(doc.modelSpace() == std::vector<EntityId>{a, b, c});
    ChangeSet front, back;
    {
        Transaction tx = doc.begin("front");
        REQUIRE(tx.bringToFront(a));
        front = tx.commit();
    }
    CHECK(doc.modelSpace() == std::vector<EntityId>{b, c, a});
    EntityId d = addLine(doc, 3);  // new entities do not jump over a front-most one
    CHECK(doc.modelSpace() == std::vector<EntityId>{b, c, d, a});
    {
        Transaction tx = doc.begin("back");
        REQUIRE(tx.sendToBack(c));
        back = tx.commit();
    }
    CHECK(doc.modelSpace() == std::vector<EntityId>{c, b, d, a});
    std::string dump = canonicalDump(doc);
    CHECK(dump.find("draworder") != std::string::npos);
    doc.apply(back, Direction::Undo);
    CHECK(doc.modelSpace() == std::vector<EntityId>{b, c, d, a});  // c is back at its id
    doc.apply(front, Direction::Undo);
    CHECK(doc.modelSpace() == std::vector<EntityId>{a, b, c, d});
    doc.apply(front, Direction::Redo);
    doc.apply(back, Direction::Redo);
    CHECK(canonicalDump(doc) == dump);

    Transaction tx = doc.begin("erase front");
    REQUIRE(tx.erase(a));
    tx.rollback();
    CHECK(doc.modelSpace() == std::vector<EntityId>{c, b, d, a});
}

TEST_CASE("header edits are transactional") {
    Document doc;
    ChangeSet cs;
    {
        Transaction tx = doc.begin("hdr");
        REQUIRE(tx.modifyHeader([](Header& h) {
            h.units = LengthUnit::Inch;
            h.createdBy = "tbcad test";
            h.custom["snap"] = "on";
        }));
        CHECK_FALSE(tx.modifyHeader([](Header& h) { h.currentLayer = LayerId{42}; }));
        CHECK_FALSE(tx.modifyHeader([](Header& h) { h.currentTextStyle = TextStyleId{42}; }));
        CHECK_FALSE(tx.modifyHeader([](Header& h) { h.currentDimStyle = DimStyleId{42}; }));
        CHECK_FALSE(tx.modifyHeader([](Header& h) { h.ltScale = -1; }));
        cs = tx.commit();
    }
    CHECK(doc.header().units == LengthUnit::Inch);
    CHECK(doc.header().custom.at("snap") == "on");
    doc.apply(cs, Direction::Undo);
    CHECK(doc.header().units == LengthUnit::Millimeter);
    CHECK(doc.header().custom.empty());
    doc.apply(cs, Direction::Redo);
    CHECK(doc.header().createdBy == "tbcad test");
}

TEST_CASE("table rows undo and redo with stable ids") {
    Document doc;
    ChangeSet cs;
    LayerId id;
    {
        Transaction tx = doc.begin("layers");
        id = tx.addLayer(Layer{.name = "A", .color = Color::index(1)}).value();
        REQUIRE(tx.modifyLayer(id, [](Layer& l) { l.locked = true; }));
        cs = tx.commit();
    }
    CHECK(doc.layer(id)->locked);
    doc.apply(cs, Direction::Undo);
    CHECK(doc.layer(id) == nullptr);
    CHECK(doc.layerByName("a") == nullptr);
    doc.apply(cs, Direction::Redo);
    REQUIRE(doc.layer(id));
    CHECK(doc.layerByName("A") == doc.layer(id));
    // The id counter moved on, so the next layer does not collide.
    Transaction tx = doc.begin("next");
    CHECK(tx.addLayer(Layer{.name = "B"}).value() != id);
    tx.commit();
}

TEST_CASE("revision counts commits undos and redos") {
    Document doc;
    auto r0 = doc.revision();
    ChangeSet cs;
    {
        Transaction tx = doc.begin("a");
        REQUIRE(tx.add(line(0, 0, 1, 1)));
        cs = tx.commit();
    }
    CHECK(doc.revision() == r0 + 1);
    doc.apply(cs, Direction::Undo);
    doc.apply(cs, Direction::Redo);
    CHECK(doc.revision() == r0 + 3);
}

TEST_CASE("modifying an entity inside a block keeps it owned") {
    Document doc;
    BlockId blk;
    EntityId e;
    {
        Transaction tx = doc.begin("b");
        blk = tx.addBlock(BlockDef{.name = "B"}).value();
        e = tx.add(line(0, 0, 1, 1), blk).value();
        tx.commit();
    }
    Transaction tx = doc.begin("m");
    REQUIRE(tx.modify(e, [](Entity& x) { std::get<LineData>(x.data).b = {9, 9}; }));
    tx.commit();
    CHECK(doc.ownerBlock(e) == blk);
    CHECK(doc.modelSpaceCount() == 0);
}

TEST_CASE("sub-layers: names unique per parent and no cycles") {
    Document doc;
    Transaction tx = doc.begin("sub");
    LayerId floor = tx.addLayer(Layer{.name = "S-FLOOR"}).value();
    LayerId other = tx.addLayer(Layer{.name = "S-WALL"}).value();
    LayerId edge = tx.addLayer(Layer{.name = "edge", .parent = floor}).value();
    LayerId edge2 = tx.addLayer(Layer{.name = "EDGE", .parent = other}).value();  // other parent
    CHECK_FALSE(tx.addLayer(Layer{.name = "Edge", .parent = floor}));
    CHECK_FALSE(tx.addLayer(Layer{.name = "x", .parent = LayerId{999}}));
    CHECK_FALSE(tx.addLayer(Layer{.name = "S-FLOOR"}));  // same name at top level
    LayerId sub = tx.addLayer(Layer{.name = "hidden", .parent = edge}).value();  // deeper
    CHECK_FALSE(tx.modifyLayer(floor, [&](Layer& l) { l.parent = sub; }));       // cycle
    CHECK_FALSE(tx.modifyLayer(floor, [&](Layer& l) { l.parent = floor; }));
    CHECK_FALSE(tx.modifyLayer(kLayerZero, [&](Layer& l) { l.parent = floor; }));
    CHECK_FALSE(tx.eraseLayer(floor));                                        // has sub-layers
    CHECK_FALSE(tx.modifyLayer(edge2, [&](Layer& l) { l.parent = floor; }));  // name clash
    CHECK(tx.modifyLayer(edge2, [&](Layer& l) {
        l.name = "other";
        l.parent = floor;
    }));
    ChangeSet cs = tx.commit();
    CHECK(doc.layerByName(floor, "EDGE") == doc.layer(edge));
    CHECK(doc.layerByName("edge") == nullptr);
    CHECK(doc.layerByName("s-floor") == doc.layer(floor));
    doc.apply(cs, Direction::Undo);
    CHECK(doc.layerByName(floor, "edge") == nullptr);
    doc.apply(cs, Direction::Redo);
    CHECK(doc.layerByName(floor, "edge") == doc.layer(edge));
}

TEST_CASE("effective layer state follows the ancestors") {
    Document doc;
    Transaction tx = doc.begin("eff");
    LayerId floor =
        tx.addLayer(
              Layer{.name = "S-FLOOR", .color = Color::index(3), .lineweight = LineWeight::mm(0.5)})
            .value();
    LayerId edge = tx.addLayer(Layer{.name = "edge",
                                     .parent = floor,
                                     .color = Color::byParent(),
                                     .linetype = kLinetypeByParent,
                                     .lineweight = LineWeight::byParent()})
                       .value();
    LayerId own =
        tx.addLayer(Layer{.name = "own", .parent = floor, .color = Color::index(1)}).value();
    tx.commit();

    EffectiveLayer e = doc.effective(edge);
    CHECK(e.color == Color::index(3));
    CHECK(e.lineweight == LineWeight::mm(0.5));
    CHECK(e.linetype == kLinetypeContinuous);
    CHECK(doc.effective(own).color == Color::index(1));
    CHECK(e.visible());

    // Parent off hides the branch, each child's own switch survives.
    Transaction t2 = doc.begin("off");
    REQUIRE(t2.modifyLayer(floor, [](Layer& l) { l.on = false; }));
    REQUIRE(t2.modifyLayer(own, [](Layer& l) { l.on = false; }));
    REQUIRE(t2.modifyLayer(edge, [](Layer& l) {
        l.locked = true;
        l.plot = false;
    }));
    ChangeSet cs = t2.commit();
    CHECK_FALSE(doc.effective(edge).on);  // cache was dropped
    CHECK_FALSE(doc.effective(edge).visible());
    CHECK(doc.effective(edge).locked);
    CHECK_FALSE(doc.effective(edge).plot);
    CHECK(doc.layer(edge)->on);  // own switch kept
    Transaction t3 = doc.begin("on");
    REQUIRE(t3.modifyLayer(floor, [](Layer& l) {
        l.on = true;
        l.frozen = true;
    }));
    t3.commit();
    CHECK(doc.effective(edge).on);
    CHECK(doc.effective(edge).frozen);
    CHECK_FALSE(doc.effective(own).on);  // its own switch is still off
    CHECK(doc.effective(LayerId{999}).id == LayerId{});

    doc.apply(cs, Direction::Undo);
    CHECK(doc.effective(edge).on);
    CHECK_FALSE(doc.effective(edge).locked);
}

TEST_CASE("ByParent needs a parent and is for layers only") {
    Document doc;
    Transaction tx = doc.begin("bp");
    CHECK_FALSE(tx.addLayer(Layer{.name = "top", .color = Color::byParent()}));
    CHECK_FALSE(tx.addLayer(Layer{.name = "top", .lineweight = LineWeight::byParent()}));
    CHECK_FALSE(tx.addLayer(Layer{.name = "top", .linetype = kLinetypeByParent}));
    LayerId p = tx.addLayer(Layer{.name = "P"}).value();
    CHECK(tx.addLayer(Layer{.name = "c", .parent = p, .color = Color::byParent()}));
    Entity e = line(0, 0, 1, 1);
    e.common.color = Color::byParent();
    CHECK_FALSE(tx.add(e));
    CHECK_FALSE(tx.addLayer(Layer{.name = "k", .parent = kLayerZero}));
    tx.commit();
}

TEST_CASE("sub-layer depth is limited to 8 levels") {
    Document doc;
    Transaction tx = doc.begin("deep");
    LayerId parent{};
    std::vector<LayerId> chain;
    for (int i = 0; i < 8; ++i) {
        auto l = tx.addLayer(Layer{.name = "L" + std::to_string(i), .parent = parent});
        REQUIRE(l);
        parent = l.value();
        chain.push_back(parent);
    }
    CHECK_FALSE(tx.addLayer(Layer{.name = "L8", .parent = parent}));
    // Re-parenting a subtree that would end up too deep is refused too.
    LayerId side = tx.addLayer(Layer{.name = "side"}).value();
    LayerId side2 = tx.addLayer(Layer{.name = "side2", .parent = side}).value();
    CHECK_FALSE(tx.modifyLayer(chain[0], [&](Layer& l) { l.parent = side2; }));
    CHECK(tx.modifyLayer(chain[7], [&](Layer& l) { l.parent = chain[6]; }));
    tx.commit();
}

TEST_CASE("current colour, linetype and lineweight are validated and undoable") {
    Document doc;
    CHECK(doc.header().currentColor == Color::byLayer());
    CHECK(doc.header().currentLinetype == kLinetypeByLayer);
    CHECK(doc.header().currentLineweight == LineWeight::byLayer());

    Linetype lt;
    lt.name = "Dashed";
    lt.pattern = {12, -6};
    LinetypeId id;
    {
        auto tx = doc.begin("setup");
        id = tx.addLinetype(lt).value();
        REQUIRE(tx.modifyHeader([&](Header& h) {
            h.currentColor = Color::index(3);
            h.currentLinetype = id;
        }));
        tx.commit();
    }
    CHECK(doc.header().currentLinetype == id);
    {
        auto tx = doc.begin("bad");
        CHECK_FALSE(tx.modifyHeader([](Header& h) { h.currentColor = Color::byParent(); }));
        CHECK_FALSE(tx.modifyHeader([](Header& h) { h.currentLinetype = LinetypeId{999}; }));
        CHECK_FALSE(tx.eraseLinetype(id));  // it is the current linetype
        tx.rollback();
    }
    CHECK(doc.header().currentColor == Color::index(3));
}

TEST_CASE("touchedEntities lists modified and erased pre-existing entities only") {
    Document doc;
    EntityId a = addLine(doc, 0), b = addLine(doc, 10), c = addLine(doc, 20);
    Transaction tx = doc.begin("edit");
    CHECK(tx.touchedEntities().empty());
    EntityId fresh = tx.add(line(0, 0, 1, 1)).value();
    REQUIRE(tx.modify(fresh, [](Entity& e) { std::get<LineData>(e.data).b.x = 5; }));
    CHECK(tx.touchedEntities().empty());  // added in this transaction
    REQUIRE(tx.modify(c, [](Entity& e) { std::get<LineData>(e.data).b.x = 5; }));
    REQUIRE(tx.erase(a));
    REQUIRE(tx.modify(c, [](Entity& e) { std::get<LineData>(e.data).b.y = 7; }));
    CHECK(tx.touchedEntities() == std::vector<EntityId>{a, c});  // sorted, each once, b untouched
    (void)b;
    tx.rollback();
}

TEST_CASE("reserveIds only raises the next free ids") {
    Document doc;
    NextIds n = doc.nextIds();
    NextIds want = n;
    want.entity = 100;
    want.layer = 50;
    REQUIRE(doc.reserveIds(want));
    CHECK(doc.nextIds().entity == 100);
    CHECK(doc.nextIds().layer == 50);
    NextIds low;  // all ones: never lowers
    REQUIRE(doc.reserveIds(low));
    CHECK(doc.nextIds().entity == 100);
    Transaction tx = doc.begin("t");
    Entity e;
    e.data = PointData{};
    CHECK(tx.add(e).value().value == 100);
    CHECK(tx.addLayer(Layer{.name = "L"}).value().value == 50);
    CHECK_FALSE(doc.reserveIds(want));  // refused while a transaction is open
    tx.commit();
}

TEST_CASE("bulk erase takes everything out in one commit and undo brings it back") {
    Document doc;
    std::vector<EntityId> ids;
    {
        auto tx = doc.begin("gen");
        for (int i = 0; i < 1000; ++i)
            ids.push_back(tx.add(Entity{{}, LineData{{double(i), 0}, {double(i), 5}}}).value());
        tx.commit();
    }
    // every third object, plus an id that is not there
    std::vector<EntityId> some;
    for (std::size_t i = 0; i < ids.size(); i += 3)
        some.push_back(ids[i]);
    some.push_back(EntityId{999999});
    auto tx = doc.begin("erase");
    CHECK(tx.eraseMany(some) == 334);
    ChangeSet cs = tx.commit();
    CHECK(doc.entityCount() == 666);
    CHECK(cs.entities.size() == 334);
    CHECK(doc.find(ids[0]) == nullptr);
    REQUIRE(doc.find(ids[1]) != nullptr);
    CHECK(std::get<LineData>(doc.find(ids[1])->data).a.x == 1.0);
    CHECK(doc.find(ids[999]) == nullptr);  // 999 is a multiple of 3
    CHECK(doc.find(ids[998]) != nullptr);
    doc.apply(cs, Direction::Undo);
    CHECK(doc.entityCount() == 1000);
    CHECK(std::get<LineData>(doc.find(ids[0])->data).b.y == 5.0);
}

TEST_CASE(
    "eraseMany handles block members, sent-to-back and edited objects together with the rest") {
    Document doc;
    std::vector<EntityId> ids;
    {
        auto tx = doc.begin("gen");
        for (int i = 0; i < 40; ++i)
            ids.push_back(tx.add(Entity{{}, LineData{{double(i), 0}, {double(i), 1}}}).value());
        tx.commit();
    }
    {
        auto tx = doc.begin("order");
        REQUIRE(tx.sendToBack(ids[7]));
        REQUIRE(tx.sendToBack(ids[30]));
        tx.commit();
    }
    const std::string before = canonicalDump(doc);
    auto tx = doc.begin("erase");
    REQUIRE(tx.modify(ids[3], [](Entity& e) { std::get<LineData>(e.data).b = {3, 9}; }));
    auto added = tx.add(Entity{{}, LineData{{0, 5}, {1, 5}}}).value();
    ids.push_back(added);
    CHECK(tx.eraseMany(ids) == 41);
    const ChangeSet cs = tx.commit();
    CHECK(doc.modelSpaceCount() == 0);
    doc.apply(cs, Direction::Undo);
    CHECK(canonicalDump(doc) == before);
    doc.apply(cs, Direction::Redo);
    CHECK(doc.modelSpaceCount() == 0);
}

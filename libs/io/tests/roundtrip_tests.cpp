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

TEST_CASE("round trip: attribute definitions and attribute values") {
    Document doc;
    BlockId blk;
    {
        Transaction tx = doc.begin("attrs");
        blk = tx.addBlock(BlockDef{.name = "Tag"}).value();
        TextData t;
        t.utf8 = "TAG1";
        t.attdef = AttDefInfo{"ROOM", "Room name?", "ห้องนอน", true, false, true, true};
        Entity e;
        e.data = Indirect<TextData>(t);
        (void)tx.add(std::move(e), blk);
        Entity plain;
        plain.data = Indirect<TextData>(TextData{.utf8 = "plain"});
        (void)tx.add(std::move(plain));
        Entity ins;
        ins.data = Indirect<InsertData>(InsertData{
            blk, {5, 5}, {1, 1}, 0, {AttribValue{"ROOM", "ห้องรับแขก"}, AttribValue{"AREA", "12"}}});
        (void)tx.add(std::move(ins));
        Entity bare;
        bare.data = Indirect<InsertData>(InsertData{blk, {1, 1}, {1, 1}, 0, {}});
        (void)tx.add(std::move(bare));
        tx.commit();
    }
    std::string before = canonicalDump(doc);
    auto back = readOrFail(writeOrFail(doc));
    CHECK(canonicalDump(*back) == before);
    const BlockDef* def = back->blockByName("Tag");
    REQUIRE(def != nullptr);
    const auto* t = entityAs<TextData>(*back->find(def->entities[0]));
    REQUIRE((t && t->attdef));
    CHECK(t->attdef->tag == "ROOM");
    CHECK(t->attdef->defaultValue == "ห้องนอน");
    CHECK(t->attdef->invisible);
    CHECK_FALSE(t->attdef->constant);
    CHECK(t->attdef->verify);
    CHECK(t->attdef->preset);
    int withAttribs = 0;
    back->forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e); i && !i->attribs.empty()) {
            ++withAttribs;
            CHECK(i->attribs.size() == 2);
            CHECK(i->attribs[0].value == "ห้องรับแขก");
        }
    });
    CHECK(withAttribs == 1);
}

TEST_CASE("round trip: associative arrays of every kind") {
    Document doc;
    {
        Transaction tx = doc.begin("arrays");
        BlockId blk = tx.addBlock(BlockDef{.name = "*A1", .anonymous = true}).value();
        Entity c;
        c.data = CircleData{{0, 0}, 1};
        (void)tx.add(std::move(c), blk);
        auto put = [&](ArrayInfo a, Vec2 at) {
            Entity e;
            e.data = Indirect<InsertData>(InsertData{blk, at, {1, 1}, 0, {}, std::move(a)});
            (void)tx.add(std::move(e));
        };
        ArrayInfo rect;
        rect.rows = 3;
        rect.columns = 4;
        rect.rowSpacing = -7.5;
        rect.columnSpacing = 6;
        rect.angle = 0.25;
        put(rect, {0, 0});
        ArrayInfo polar;
        polar.kind = ArrayInfo::Kind::Polar;
        polar.center = {5, 5};
        polar.count = 7;
        polar.fillAngle = 3;
        polar.rotateItems = false;
        polar.reference = {1, 2};
        put(polar, {1, 1});
        ArrayInfo path;
        path.kind = ArrayInfo::Kind::Path;
        path.count = 9;
        path.path = {PolyVertex{{0, 0}, 0.5}, PolyVertex{{10, 0}}, PolyVertex{{10, 10}}};
        path.pathClosed = true;
        path.pathSpacing = 2.5;
        path.reference = {1, 1};
        put(path, {2, 2});
        tx.commit();
    }
    std::string before = canonicalDump(doc);
    auto back = readOrFail(writeOrFail(doc));
    CHECK(canonicalDump(*back) == before);
    int arrays = 0;
    back->forEachEntity([&](const Entity& e) {
        const auto* i = entityAs<InsertData>(e);
        if (!i || !i->array)
            return;
        ++arrays;
        if (i->array->kind == ArrayInfo::Kind::Rectangular) {
            CHECK(i->array->rows == 3);
            CHECK(i->array->columns == 4);
            CHECK(i->array->rowSpacing == -7.5);
            CHECK(i->array->angle == 0.25);
        } else if (i->array->kind == ArrayInfo::Kind::Polar) {
            CHECK(i->array->count == 7);
            CHECK_FALSE(i->array->rotateItems);
        } else {
            CHECK(i->array->path.size() == 3);
            CHECK(i->array->path[0].bulge == 0.5);
            CHECK(i->array->pathClosed);
            CHECK(i->array->pathSpacing == 2.5);
        }
    });
    CHECK(arrays == 3);
}

TEST_CASE("round trip: dynamic block definitions and values") {
    Document doc;
    {
        Transaction tx = doc.begin("dynamic");
        BlockId blk = tx.addBlock(BlockDef{.name = "Door"}).value();
        Entity a;
        a.data = LineData{{0, 0}, {10, 0}};
        EntityId first = tx.add(std::move(a), blk).value();
        Entity b;
        b.data = CircleData{{5, 5}, 1};
        EntityId second = tx.add(std::move(b), blk).value();
        DynamicDef def;
        DynParam w;
        w.name = "Width";
        w.base = {0, 0};
        w.end = {10, 0};
        w.min = 5;
        w.max = 30;
        def.params.push_back(w);
        DynParam v;
        v.kind = DynParam::Kind::Visibility;
        v.name = "Style";
        v.states = {"Open", "ปิด"};
        v.stateObjects = {{first}, {second}};
        def.params.push_back(v);
        DynAction act;
        act.kind = DynAction::Kind::Stretch;
        act.param = 0;
        act.frameMin = {9, -1};
        act.frameMax = {11, 1};
        act.objects = {first};
        def.actions.push_back(act);
        REQUIRE(tx.modifyBlock(blk, [&](BlockDef& d) { d.dynamic = std::move(def); }).ok());
        Entity ins;
        InsertData data{blk, {3, 3}, {1, 1}, 0};
        data.dynValues = {14, 1};
        ins.data = Indirect<InsertData>(std::move(data));
        (void)tx.add(std::move(ins));
        tx.commit();
    }
    std::string before = canonicalDump(doc);
    auto back = readOrFail(writeOrFail(doc));
    CHECK(canonicalDump(*back) == before);
    const BlockDef* def = back->blockByName("Door");
    REQUIRE((def && def->dynamic));
    CHECK(def->dynamic->params.size() == 2);
    CHECK(def->dynamic->params[1].states[1] == "ปิด");
    CHECK(def->dynamic->params[0].max == 30);
    REQUIRE(def->dynamic->actions.size() == 1);
    CHECK(def->dynamic->actions[0].objects.size() == 1);
    int values = 0;
    back->forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e)) {
            ++values;
            CHECK(i->dynValues == std::vector<double>{14, 1});
        }
    });
    CHECK(values == 1);
}

TEST_CASE("round trip: external reference fields of a block") {
    Document doc;
    {
        Transaction tx = doc.begin("blocks");
        BlockId plain = tx.addBlock(BlockDef{.name = "plain"}).value();
        BlockId ref = tx.addBlock(BlockDef{.name = "ref"}).value();
        REQUIRE(tx.modifyBlock(ref, [](BlockDef& d) {
                      d.xrefPath = "..\\plans\\site.tbcad";
                      d.xrefStamp = 1760000000123456789;
                  }).ok());
        (void)plain;
        tx.commit();
    }
    auto bytes = io::writeTbcadToMemory(doc);
    REQUIRE(bytes.ok());
    io::IoReport rep;
    auto back = io::readTbcad(bytes.value(), rep);
    REQUIRE(back.ok());
    CHECK(back.value()->blockByName("plain")->xrefPath.empty());
    const BlockDef* ref = back.value()->blockByName("ref");
    REQUIRE(ref);
    CHECK(ref->xrefPath == "..\\plans\\site.tbcad");
    CHECK(ref->xrefStamp == 1760000000123456789);
    CHECK(rep.unknownChunks == 0);
}

TEST_CASE("round trip: ids freed after the highest saved id are not reused after a reload") {
    Document doc;
    EntityId lastEntity;
    LayerId lastLayer;
    {
        Transaction tx = doc.begin("setup");
        Entity a;
        a.data = LineData{{0, 0}, {1, 0}};
        (void)tx.add(a);
        lastEntity = tx.add(a).value();
        lastLayer = tx.addLayer(Layer{.name = "temp"}).value();
        tx.commit();
    }
    {
        Transaction tx = doc.begin("delete");
        (void)tx.erase(lastEntity);
        (void)tx.eraseLayer(lastLayer);
        tx.commit();
    }
    NextIds before = doc.nextIds();
    auto back = readOrFail(writeOrFail(doc));
    CHECK(back->nextIds().entity == before.entity);
    CHECK(back->nextIds().layer == before.layer);
    Transaction tx = back->begin("more");
    Entity b;
    b.data = LineData{{0, 0}, {2, 0}};
    CHECK(tx.add(b).value().value > lastEntity.value);
    CHECK(tx.addLayer(Layer{.name = "again"}).value().value > lastLayer.value);
}

TEST_CASE("round trip: a gradient hatch keeps its gradient") {
    Document doc;
    {
        Transaction tx = doc.begin("hatch");
        HatchData h;
        HatchLoop loop;
        loop.edges.push_back(HatchLineEdge{{0, 0}, {10, 0}});
        loop.edges.push_back(HatchLineEdge{{10, 0}, {10, 10}});
        loop.edges.push_back(HatchLineEdge{{10, 10}, {0, 0}});
        h.loops.push_back(loop);
        GradientData g;
        g.kind = GradientKind::Cylinder;
        g.color1 = Color::index(5);
        g.color2 = Color::rgb(1, 2, 3);
        g.angle = 0.75;
        h.gradient = g;
        Entity e;
        e.data = std::move(h);
        (void)tx.add(std::move(e));
        Entity plain;
        plain.data = HatchData{};
        (void)tx.add(std::move(plain));
        tx.commit();
    }
    auto back = readOrFail(writeOrFail(doc));
    CHECK(canonicalDump(*back) == canonicalDump(doc));
    const HatchData* h = entityAs<HatchData>(*back->find(EntityId{1}));
    REQUIRE(h != nullptr);
    REQUIRE(h->gradient.has_value());
    CHECK(h->gradient->kind == GradientKind::Cylinder);
    CHECK(h->gradient->color1.kind == ColorKind::Index);
    CHECK(h->gradient->color2.b == 3);
    CHECK(h->gradient->angle == doctest::Approx(0.75));
    CHECK_FALSE(entityAs<HatchData>(*back->find(EntityId{2}))->gradient.has_value());
}

TEST_CASE("round trip: levels, entity levels, current level and the level view") {
    auto doc = makeRichDocument();
    LevelId l2, l3;
    {
        auto tx = doc->begin("levels");
        REQUIRE(tx.modifyLevel(kLevelDefault, [](Level& l) { l.name = "ชั้น 1"; }).ok());
        Level up;
        up.name = "Level 2";
        up.elevation = 3200.5;
        up.locked = true;
        l2 = tx.addLevel(up).value();
        Level hidden;
        hidden.name = "Roof";
        hidden.elevation = 6400;
        hidden.visible = false;
        l3 = tx.addLevel(hidden).value();
        REQUIRE(tx.erase(EntityId{6}).ok());  // keep ids sparse
        REQUIRE(tx.modify(EntityId{7}, [&](Entity& e) { e.common.level = l2; }).ok());
        REQUIRE(tx.modify(EntityId{9}, [&](Entity& e) { e.common.level = l3; }).ok());
        REQUIRE(tx.modifyHeader([&](Header& h) {
                      h.currentLevel = l2;
                      h.levelView = LevelView::ShowAll;
                  }).ok());
        tx.commit();
    }
    std::string before = canonicalDump(*doc);
    CHECK(before.find("level2") != std::string::npos);
    for (bool compress : {true, false}) {
        io::WriteOptions o;
        o.compress = compress;
        auto bytes = writeOrFail(*doc, o);
        io::IoReport report;
        auto back = readOrFail(bytes, &report);
        CHECK(canonicalDump(*back) == before);
        CHECK(report.warnings.empty());
        CHECK(back->level(l2)->name == "Level 2");
        CHECK(back->level(l2)->locked);
        CHECK_FALSE(back->level(l3)->visible);
        CHECK(back->find(EntityId{7})->common.level == l2);
        CHECK(back->find(EntityId{9})->common.level == l3);
        CHECK(back->find(EntityId{8})->common.level == kLevelDefault);
        CHECK(back->header().currentLevel == l2);
        CHECK(back->header().levelView == LevelView::ShowAll);
        CHECK(writeOrFail(*back, o) == bytes);
    }
}

TEST_CASE("round trip: a document with one pristine level has no level chunks") {
    auto doc = makeRichDocument();
    auto bytes = writeOrFail(*doc);
    auto chunks = io::listChunks(bytes);
    REQUIRE(chunks.ok());
    for (const auto& c : chunks.value()) {
        CHECK(c.type != "LVLS");
        CHECK(c.type != "ELVL");
    }
}

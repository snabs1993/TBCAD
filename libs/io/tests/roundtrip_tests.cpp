#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <random>
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
    CHECK(kinds.size() == kEntityKindCount);

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

TEST_CASE("round trip: text and shapes in linetypes") {
    Document doc;
    {
        Transaction tx = doc.begin("lt");
        Linetype lt;
        lt.name = "GAS";
        lt.pattern = {10, -5};
        LinetypeMark t;
        t.text = "GAS \xe0\xb8\x81";
        t.style = "Standard";
        t.scale = 1.5;
        t.rotation = 0.25;
        t.rotationKind = LinetypeMark::Rotation::Upright;
        t.offset = 10;
        t.shift = {-1, 0.5};
        LinetypeMark s;
        s.kind = LinetypeMark::Kind::Shape;
        s.text = "CIRC1";
        s.offset = 15;
        lt.marks = {t, s};
        REQUIRE(tx.addLinetype(lt).ok());
        tx.commit();
    }
    std::string before = canonicalDump(doc);
    CHECK(before.find("mark") != std::string::npos);
    auto bytes = writeOrFail(doc, {});
    auto back = readOrFail(bytes, nullptr);
    CHECK(canonicalDump(*back) == before);
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

TEST_CASE("round trip: the named UCS table and a rotated ordinate dimension survive") {
    Document doc;
    {
        auto tx = doc.begin("ucs");
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.custom["tbcad.ucs"] = "Tilt\t12.5\t-3\t0.5\n";
                  }).ok());
        DimensionData d;
        d.kind = DimKind::Ordinate;
        d.oblique = 0.5;
        d.defPts = {Vec2{1, 2}, Vec2{3, 4}, Vec2{0, 0}, Vec2{}};
        Entity e;
        e.data = d;
        REQUIRE(tx.add(std::move(e)).ok());
        tx.commit();
    }
    auto back = readOrFail(writeOrFail(doc));
    CHECK(back->header().custom.at("tbcad.ucs") == "Tilt\t12.5\t-3\t0.5\n");
    bool seen = false;
    back->forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<DimensionData>(e)) {
            seen = true;
            CHECK(d->oblique == 0.5);
        }
    });
    CHECK(seen);
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

TEST_CASE("round trip: assembly definitions and parameter values") {
    Document doc;
    {
        Transaction tx = doc.begin("assembly");
        BlockId blk = tx.addBlock(BlockDef{.name = "Shelf"}).value();
        Entity a;
        a.data = LineData{{0, 0}, {10, 0}};
        EntityId line = tx.add(std::move(a), blk).value();
        AssemblyDef def;
        AsmParam w;
        w.name = "ความยาว";
        w.defaultNumber = 10;
        w.min = 1;
        w.max = 99;
        def.params.push_back(w);
        AsmParam m;
        m.kind = AsmParam::Kind::Choice;
        m.name = "Material";
        m.choices = {"Oak", "Pine"};
        m.defaultText = "Oak";
        def.params.push_back(m);
        def.bindings.push_back(
            AsmBinding{AsmBinding::Kind::ArrayField, line, "columns", "ceil(ความยาว / 5)"});
        def.joinClass = "ชั้น";
        def.presets.push_back(AsmPreset{
            "long", {AsmValue{"ความยาว", false, 80, ""}, AsmValue{"Material", true, 0, "Pine"}}});
        REQUIRE(tx.modifyBlock(blk, [&](BlockDef& d) { d.assembly = std::move(def); }).ok());
        Entity ins;
        InsertData data{blk, {3, 3}, {1, 1}, 0};
        data.asmValues = {AsmValue{"ความยาว", false, 42, ""},
                          AsmValue{"Material", true, 0, "Pine"}};
        data.joinOff = true;
        ins.data = Indirect<InsertData>(std::move(data));
        (void)tx.add(std::move(ins));
        tx.commit();
    }
    std::string before = canonicalDump(doc);
    auto back = readOrFail(writeOrFail(doc));
    CHECK(canonicalDump(*back) == before);
    const BlockDef* def = back->blockByName("Shelf");
    REQUIRE((def && def->assembly));
    CHECK(def->assembly->params.size() == 2);
    CHECK(def->assembly->params[1].choices[1] == "Pine");
    REQUIRE(def->assembly->bindings.size() == 1);
    CHECK(def->assembly->bindings[0].formula == "ceil(ความยาว / 5)");
    REQUIRE(def->assembly->presets.size() == 1);
    CHECK(def->assembly->presets[0].values[1].text == "Pine");
    CHECK(def->assembly->joinClass == "ชั้น");
    int values = 0;
    back->forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e)) {
            ++values;
            REQUIRE(i->asmValues.size() == 2);
            CHECK(i->asmValues[0].number == 42);
            CHECK(i->asmValues[1].text == "Pine");
            CHECK(i->joinOff);
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

TEST_CASE("round trip: a double hatch stays double") {
    Document doc;
    {
        Transaction tx = doc.begin("hatch");
        HatchData h;
        HatchLoop loop;
        loop.edges.push_back(HatchLineEdge{{0, 0}, {10, 0}});
        loop.edges.push_back(HatchLineEdge{{10, 0}, {10, 10}});
        loop.edges.push_back(HatchLineEdge{{10, 10}, {0, 0}});
        h.loops.push_back(loop);
        h.solid = false;
        h.pattern = "ANSI31";
        h.doubled = true;
        Entity e;
        e.data = std::move(h);
        (void)tx.add(std::move(e));
        tx.commit();
    }
    auto back = readOrFail(writeOrFail(doc));
    const HatchData* h = entityAs<HatchData>(*back->find(EntityId{1}));
    REQUIRE(h != nullptr);
    CHECK(h->doubled);
    CHECK(canonicalDump(*back) == canonicalDump(doc));
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

TEST_CASE("round trip: insert clipping boundary") {
    Document doc;
    {
        Transaction tx = doc.begin("clip");
        BlockId blk = tx.addBlock(BlockDef{.name = "Plan"}).value();
        Entity a;
        a.data = LineData{{0, 0}, {10, 0}};
        (void)tx.add(std::move(a), blk);
        Entity ins;
        InsertData data{blk, {3, 3}, {1, 1}, 0};
        data.clip = {{0, 0}, {5, 0}, {5, 5}, {0, 5}};
        data.clipOn = false;
        ins.data = Indirect<InsertData>(std::move(data));
        (void)tx.add(std::move(ins));
        Entity plain;
        plain.data = Indirect<InsertData>(InsertData{blk, {50, 0}, {1, 1}, 0});
        (void)tx.add(std::move(plain));
        tx.commit();
    }
    std::string before = canonicalDump(doc);
    auto back = readOrFail(writeOrFail(doc));
    CHECK(canonicalDump(*back) == before);
    CHECK(before.find("CLIPoff") != std::string::npos);
}

TEST_CASE("round trip: layer, object and current transparency survive") {
    auto doc = makeRichDocument();
    const LayerId layer = doc->layerByName("A-WALL")->id;
    EntityId first, second;
    {
        std::vector<EntityId> ids;
        doc->forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
        REQUIRE(ids.size() >= 2);
        first = ids[0];
        second = ids[1];
    }
    {
        Transaction tx = doc->begin("transparency");
        REQUIRE(tx.modifyLayer(layer, [](Layer& l) { l.transparency = 35; }).ok());
        REQUIRE(tx.modify(first, [](Entity& e) { e.common.transparency = 70; }).ok());
        REQUIRE(tx.modify(second, [](Entity& e) {
                      e.common.transparency = kTransparencyByBlock;
                  }).ok());
        REQUIRE(tx.modifyHeader([](Header& h) { h.currentTransparency = 10; }).ok());
        tx.commit();
    }
    const std::string before = canonicalDump(*doc);
    auto back = readOrFail(writeOrFail(*doc));
    CHECK(canonicalDump(*back) == before);
    CHECK(back->layerByName("A-WALL")->transparency == 35);
    CHECK(back->find(first)->common.transparency == 70);
    CHECK(back->find(second)->common.transparency == kTransparencyByBlock);
    CHECK(back->header().currentTransparency == 10);
}

TEST_CASE("round trip: a layer description survives") {
    auto doc = makeRichDocument();
    const LayerId layer = doc->layerByName("A-WALL")->id;
    {
        Transaction tx = doc->begin("describe");
        REQUIRE(
            tx.modifyLayer(layer, [](Layer& l) { l.description = "Walls, \xE0\xB8\x81"; }).ok());
        tx.commit();
    }
    const std::string before = canonicalDump(*doc);
    auto back = readOrFail(writeOrFail(*doc));
    CHECK(canonicalDump(*back) == before);
    CHECK(back->layerByName("A-WALL")->description == "Walls, \xE0\xB8\x81");
}

namespace {
// Numbers a file must carry exactly: ordinary ones, tiny, huge, negative zero and powers of two.
double fuzzNumber(std::mt19937_64& rng) {
    switch (rng() % 8) {
        case 0:
            return 0.0;
        case 1:
            return -0.0;
        case 2:
            return std::numeric_limits<double>::denorm_min();
        case 3:
            return 1e9 * (static_cast<double>(rng() % 1000) / 999.0);
        case 4:
            return -static_cast<double>(rng() % 1000000) / 8.0;
        case 5:
            return std::ldexp(1.0, static_cast<int>(rng() % 60) - 30);
        default:
            return (static_cast<double>(rng() % 2000001) - 1000000.0) / 1000.0 + 1e-13;
    }
}
// A strictly positive number of any size the generator knows.
double fuzzPositive(std::mt19937_64& rng) {
    const double v = std::abs(fuzzNumber(rng));
    return v > 0 ? v : 1.0;
}
Vec2 fuzzPoint(std::mt19937_64& rng) {
    return {fuzzNumber(rng), fuzzNumber(rng)};
}
std::string fuzzString(std::mt19937_64& rng) {
    static const char* const parts[] = {
        "",
        "a",
        "Hello",
        "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5",
        "line\nbreak",
        "tab\there",
        "quote\"s",
        "\\P",
        "x y z"};
    std::string s;
    for (unsigned i = 0, n = rng() % 4; i < n; ++i)
        s += parts[rng() % (sizeof parts / sizeof *parts)];
    return s;
}
Entity fuzzEntity(std::mt19937_64& rng) {
    Entity e;
    EntityCommon& c = e.common;
    c.linetypeScale = static_cast<float>(fuzzPositive(rng));
    if (!std::isfinite(c.linetypeScale) || c.linetypeScale <= 0)
        c.linetypeScale = 1;
    switch (rng() % 5) {
        case 0:
            c.color = Color::byLayer();
            break;
        case 1:
            c.color = Color::byBlock();
            break;
        case 2:
            c.color = Color::index(static_cast<std::uint8_t>(1 + rng() % 255));
            break;
        default:
            c.color = Color::rgb(static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng()),
                                 static_cast<std::uint8_t>(rng()));
            break;
    }
    c.lineweight = rng() % 3 == 0 ? LineWeight::byBlock()
                                  : LineWeight::hundredths(static_cast<std::int16_t>(rng() % 212));
    c.transparency = rng() % 3 == 0 ? kTransparencyByLayer
                                    : static_cast<std::int8_t>(rng() % (kTransparencyMax + 1));
    c.visible = rng() % 4 != 0;
    c.annotative = rng() % 5 == 0;
    c.wipeout = rng() % 7 == 0;
    if (c.annotative && rng() % 2)
        c.annoScales = ScaleList(std::vector<double>{static_cast<double>(1 + rng() % 200), 50.0});
    if (rng() % 4 == 0) {
        ExtData x;
        static const int textCodes[] = {1000, 1002, 1003, 1005};
        static const int intCodes[] = {1070, 1071};
        static const int realCodes[] = {1010, 1020, 1030, 1040, 1041};
        for (unsigned a = 0, apps = rng() % 3; a < apps; ++a) {
            XdataApp app;
            app.app = "APP" + std::to_string(a) + fuzzString(rng);
            for (unsigned i = 0, n = rng() % 8; i < n; ++i) {
                DataItem it;
                switch (rng() % 3) {
                    case 0:
                        it.code = textCodes[rng() % 4];
                        it.text = fuzzString(rng);
                        break;
                    case 1:
                        it.code = intCodes[rng() % 2];
                        it.integer = static_cast<std::int64_t>(rng() % 100000) - 50000;
                        break;
                    default:
                        it.code = realCodes[rng() % 5];
                        it.real = fuzzNumber(rng);
                        break;
                }
                app.items.push_back(std::move(it));
            }
            x.xdata.push_back(std::move(app));
        }
        for (unsigned d = 0, recs = rng() % 3; d < recs; ++d) {
            DictRecord rec;
            rec.name = "KEY" + std::to_string(d);
            DataItem a;
            a.code = 1;
            a.text = fuzzString(rng);
            DataItem b;
            b.code = 90;
            b.integer = static_cast<std::int64_t>(rng() % 1000);
            DataItem c;
            c.code = 40;
            c.real = fuzzNumber(rng);
            rec.items = {a, b, c};
            x.dict.push_back(std::move(rec));
        }
        e.common.ext = ExtDataPtr(std::move(x));
    }
    switch (rng() % 11) {
        case 0:
            e.data = LineData{fuzzPoint(rng), fuzzPoint(rng)};
            break;
        case 1:
            e.data = CircleData{fuzzPoint(rng), fuzzPositive(rng)};
            break;
        case 2:
            e.data = ArcData{fuzzPoint(rng), fuzzPositive(rng), 1.0, 2.5};
            break;
        case 3: {
            PolylineData p;
            for (unsigned i = 0, n = 1 + rng() % 12; i < n; ++i)
                p.v.push_back(
                    PolyVertex{fuzzPoint(rng), fuzzNumber(rng), fuzzNumber(rng), fuzzNumber(rng)});
            p.closed = rng() % 2;
            p.restartLinetype = rng() % 3 == 0;
            e.data = std::move(p);
            break;
        }
        case 4:
            e.data = PointData{fuzzPoint(rng)};
            break;
        case 5: {
            EllipseData d;
            d.center = fuzzPoint(rng);
            d.majorAxis = {fuzzPositive(rng), fuzzNumber(rng)};
            d.ratio = 0.5;
            d.startParam = fuzzNumber(rng);
            d.endParam = fuzzNumber(rng);
            e.data = Indirect<EllipseData>(std::move(d));
            break;
        }
        case 6: {
            SplineData d;
            d.degree = 1 + static_cast<int>(rng() % 5);
            for (unsigned i = 0, n = 2 + rng() % 8; i < n; ++i) {
                d.control.push_back(fuzzPoint(rng));
                if (rng() % 2)
                    d.weights.push_back(1.0 + (rng() % 100) / 7.0);
            }
            if (d.weights.size() != d.control.size())
                d.weights.clear();
            for (std::size_t i = 0; i < d.control.size() + d.degree + 1; ++i)
                d.knots.push_back(static_cast<double>(i) * 0.5);
            for (unsigned i = 0, n = rng() % 4; i < n; ++i)
                d.fit.push_back(fuzzPoint(rng));
            d.closed = rng() % 2;
            d.periodic = rng() % 2;
            e.data = Indirect<SplineData>(std::move(d));
            break;
        }
        case 7: {
            TextData d;
            d.position = fuzzPoint(rng);
            d.alignPoint = fuzzPoint(rng);
            d.height = fuzzPositive(rng);
            d.rotation = fuzzNumber(rng);
            d.widthFactor = fuzzNumber(rng);
            d.oblique = fuzzNumber(rng);
            d.h = static_cast<HAlign>(rng() % 6);
            d.v = static_cast<VAlign>(rng() % 4);
            d.utf8 = fuzzString(rng);
            e.data = Indirect<TextData>(std::move(d));
            break;
        }
        case 8: {
            RayData d;
            d.origin = fuzzPoint(rng);
            d.dir = {0.6, 0.8};
            d.infinite = rng() % 2;
            e.data = Indirect<RayData>(std::move(d));
            break;
        }
        case 9: {
            MlineData d;
            d.style = fuzzString(rng);
            d.scale = fuzzPositive(rng);
            d.justification = static_cast<int>(rng() % 3);
            d.closed = rng() % 2;
            for (unsigned i = 0, n = rng() % 6; i < n; ++i)
                d.v.push_back(fuzzPoint(rng));
            e.data = Indirect<MlineData>(std::move(d));
            break;
        }
        default: {
            TableData d;
            d.origin = fuzzPoint(rng);
            d.rotation = fuzzNumber(rng);
            const unsigned cols = 1 + rng() % 4, rows = 1 + rng() % 4;
            for (unsigned i = 0; i < cols; ++i)
                d.colWidths.push_back(1 + (rng() % 100) / 3.0);
            for (unsigned i = 0; i < rows; ++i)
                d.rowHeights.push_back(1 + (rng() % 100) / 3.0);
            d.cells.resize(cols * rows);
            for (TableCell& cell : d.cells)
                cell.utf8 = fuzzString(rng);
            d.titleRow = rng() % 2;
            e.data = Indirect<TableData>(std::move(d));
            break;
        }
    }
    return e;
}
}  // namespace

TEST_CASE("round trip fuzz: random entities, properties and extended data survive exactly") {
    std::mt19937_64 rng(20261018);
    int withXdata = 0, withDict = 0;
    for (int round = 0; round < 40; ++round) {
        Document doc;
        {
            Transaction tx = doc.begin("fuzz");
            for (int i = 0, n = 1 + static_cast<int>(rng() % 60); i < n; ++i)
                if (auto r = tx.add(fuzzEntity(rng)); !r.ok())
                    FAIL_CHECK("rejected: " << r.error().message);
            tx.commit();
        }
        const std::string before = canonicalDump(doc);
        withXdata += before.find("xdata{") != std::string::npos;
        withDict += before.find("xdict{") != std::string::npos;
        for (bool compress : {true, false}) {
            io::WriteOptions o;
            o.compress = compress;
            auto bytes = writeOrFail(doc, o);
            io::IoReport report;
            auto back = readOrFail(bytes, &report);
            CAPTURE(round);
            CAPTURE(compress);
            CHECK(canonicalDump(*back) == before);
            CHECK(report.warnings.empty());
            CHECK(writeOrFail(*back, o) == bytes);
        }
    }
    CHECK(withXdata > 5);
    CHECK(withDict > 5);
}

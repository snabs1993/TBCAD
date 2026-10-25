#include <doctest/doctest.h>

#include <cstdio>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/properties.h"

using namespace tbcad;
using namespace tbcad::layers;

namespace {

EntityId addEntity(Document& doc, EntityData data, LayerId layer = {}) {
    auto tx = doc.begin("add");
    Entity e{{}, std::move(data)};
    e.common.layer = layer;
    auto id = tx.add(std::move(e));
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}

}  // namespace

TEST_CASE("common properties: shared values and 'varies'") {
    Document doc;
    EntityId a = addEntity(doc, LineData{{0, 0}, {1, 0}});
    EntityId b = addEntity(doc, CircleData{{0, 0}, 5});
    {
        auto tx = doc.begin("tweak");
        REQUIRE(tx.modify(b, [](Entity& e) { e.common.color = Color::index(1); }).ok());
        tx.commit();
    }
    CommonProps p = readCommon(doc, {a, b});
    CHECK(p.count == 2);
    CHECK(p.kinds.at(EntityKind::Line) == 1);
    CHECK(p.kinds.at(EntityKind::Circle) == 1);
    CHECK_FALSE(p.layer.varies);
    CHECK(p.layer.value == kLayerZero);
    CHECK(p.color.varies);
    CHECK_FALSE(p.lineweight.varies);
    CHECK_FALSE(p.linetypeScale.varies);

    CommonProps only = readCommon(doc, {a, b}, EntityKind::Circle);
    CHECK(only.count == 1);
    CHECK_FALSE(only.color.varies);
    CHECK(only.color.value == Color::index(1));
    CHECK(only.kinds.size() == 2);  // the breakdown still lists every type
}

TEST_CASE("editing the colour of 1,000 mixed entities is one undo step") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    std::vector<EntityId> ids;
    {
        auto tx = doc.begin("populate");
        for (int i = 0; i < 1000; ++i) {
            Entity e;
            switch (i % 4) {
                case 0:
                    e.data = LineData{{0, 0}, {double(i + 1), 0}};
                    break;
                case 1:
                    e.data = CircleData{{0, double(i)}, 2};
                    break;
                case 2:
                    e.data = ArcData{{0, 0}, 3, 0, 1};
                    break;
                default:
                    e.data = PolylineData{{{{0, 0}, 0, 0, 0}, {{1, double(i)}, 0, 0, 0}}, false, 0};
                    break;
            }
            ids.push_back(tx.add(std::move(e)).value());
        }
        tx.commit();
    }
    host.setSelection(ids);
    auto r = host.run("-PROP", {"Color", "red", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    for (EntityId id : ids)
        CHECK(doc.find(id)->common.color == Color::index(1));

    // Exactly one new undo entry.
    CHECK(host.undoStack().undoLabel() == "PROPERTIES");
    CHECK(host.undo());
    for (EntityId id : ids)
        CHECK(doc.find(id)->common.color == Color::byLayer());
    CHECK_FALSE(host.undoStack().canUndo());  // nothing else was recorded
    CHECK(host.redo());
    CHECK(doc.find(ids[17])->common.color == Color::index(1));
}

TEST_CASE(
    "-PROP: several properties in one command, Only narrows the target, locked layers are "
    "skipped") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    LayerId lockedLayer;
    {
        auto tx = doc.begin("layer");
        lockedLayer = createLayer(doc, tx, "Locked").value();
        REQUIRE(setLocked(doc, tx, lockedLayer, true).ok());
        tx.commit();
    }
    EntityId line = addEntity(doc, LineData{{0, 0}, {1, 0}});
    EntityId circle = addEntity(doc, CircleData{{0, 0}, 1});
    EntityId frozenOut = addEntity(doc, CircleData{{0, 0}, 1}, lockedLayer);
    host.setSelection({line, circle, frozenOut});
    host.run("-PROP", {"Only", "circle", "LTScale", "2.5", "Lineweight", "0.5", "Layer", "Locked",
                       "Only", "*", "Color", "10,20,30", cmd::ScriptItem::enter()});
    CHECK(doc.find(circle)->common.linetypeScale == doctest::Approx(2.5));
    CHECK(doc.find(circle)->common.lineweight == LineWeight::mm(0.5));
    CHECK(doc.find(circle)->common.layer == lockedLayer);
    CHECK(doc.find(line)->common.linetypeScale == doctest::Approx(1));
    CHECK(doc.find(line)->common.color == Color::rgb(10, 20, 30));
    CHECK(doc.find(frozenOut)->common.color == Color::byLayer());  // locked layer untouched
    CHECK(host.logContains("locked"));

    host.clearLog();
    host.run("-PROP", {"Color", "ByParent", "LTScale", "0", cmd::ScriptItem::enter()});
    CHECK(host.logContains("layers only"));
    CHECK(host.logContains("greater than zero"));
}

TEST_CASE("geometry fields of a line") {
    Document doc;
    EntityId id = addEntity(doc, LineData{{0, 0}, {30, 40}});
    auto fields = geometryFields(*doc.find(id));
    auto get = [&](const char* f) {
        for (auto& g : fields)
            if (g.id == f)
                return g.value;
        FAIL("missing field");
        return 0.0;
    };
    CHECK(get("length") == doctest::Approx(50));
    CHECK(get("angle") == doctest::Approx(53.130102354));
    {
        auto tx = doc.begin("set");
        REQUIRE(setGeometryField(tx, id, "length", 100).ok());
        tx.commit();
    }
    const auto* l = entityAs<LineData>(*doc.find(id));
    CHECK(l->b.x == doctest::Approx(60));
    CHECK(l->b.y == doctest::Approx(80));
    {
        auto tx = doc.begin("angle");
        REQUIRE(setGeometryField(tx, id, "angle", 90).ok());
        tx.commit();
    }
    l = entityAs<LineData>(*doc.find(id));
    CHECK(l->b.x == doctest::Approx(0).epsilon(1e-9));
    CHECK(l->b.y == doctest::Approx(100));
    auto tx = doc.begin("bad");
    CHECK_FALSE(setGeometryField(tx, id, "length", -1).ok());
    CHECK_FALSE(setGeometryField(tx, id, "radius", 1).ok());
    tx.rollback();
}

TEST_CASE("geometry fields of circle, arc and polyline") {
    Document doc;
    EntityId c = addEntity(doc, CircleData{{1, 2}, 5});
    EntityId a = addEntity(doc, ArcData{{0, 0}, 10, 0, kPi / 2});
    EntityId p = addEntity(
        doc, PolylineData{
                 {{{0, 0}, 0, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 10}, 0, 0, 0}, {{0, 10}, 0, 0, 0}},
                 true,
                 0});
    auto find = [](const std::vector<GeometryField>& fs, const char* id) {
        for (auto& f : fs)
            if (f.id == id)
                return f;
        FAIL("missing field");
        return GeometryField{};
    };
    auto cf = geometryFields(*doc.find(c));
    CHECK(find(cf, "area").value == doctest::Approx(kPi * 25));
    CHECK_FALSE(find(cf, "area").editable);
    CHECK(find(cf, "diameter").value == doctest::Approx(10));
    auto af = geometryFields(*doc.find(a));
    CHECK(find(af, "length").value == doctest::Approx(10 * kPi / 2));
    CHECK(find(af, "totalangle").value == doctest::Approx(90));
    auto pf = geometryFields(*doc.find(p));
    CHECK(find(pf, "vertices").value == 4);
    CHECK(find(pf, "area").value == doctest::Approx(100));
    CHECK(find(pf, "length").value == doctest::Approx(40));
    CHECK(find(pf, "closed").value == 1);

    auto tx = doc.begin("edit");
    REQUIRE(setGeometryField(tx, c, "diameter", 20).ok());
    REQUIRE(setGeometryField(tx, a, "endangle", 180).ok());
    REQUIRE(setGeometryField(tx, p, "closed", 0).ok());
    REQUIRE(setGeometryField(tx, p, "width", 2).ok());
    CHECK_FALSE(setGeometryField(tx, c, "area", 5).ok());
    CHECK_FALSE(setGeometryField(tx, c, "radius", 0).ok());
    tx.commit();
    CHECK(entityAs<CircleData>(*doc.find(c))->radius == doctest::Approx(10));
    CHECK(entityAs<ArcData>(*doc.find(a))->endAngle == doctest::Approx(kPi));
    CHECK_FALSE(entityAs<PolylineData>(*doc.find(p))->closed);
    CHECK(entityAs<PolylineData>(*doc.find(p))->constantWidth == doctest::Approx(2));
}

TEST_CASE("-PROP Set edits geometry fields of every selected object that has them") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    EntityId c1 = addEntity(doc, CircleData{{0, 0}, 1});
    EntityId c2 = addEntity(doc, CircleData{{5, 0}, 2});
    host.setSelection({c1, c2});
    host.run("-PROP", {"Set", "radius", "100/4", cmd::ScriptItem::enter()});
    CHECK(entityAs<CircleData>(*doc.find(c1))->radius == doctest::Approx(25));
    CHECK(entityAs<CircleData>(*doc.find(c2))->radius == doctest::Approx(25));
    host.clearLog();
    host.run("-PROP", {"Set", "area", "5", cmd::ScriptItem::enter()});
    CHECK(host.logContains("read-only"));
    host.run("-PROP", {"Set", "radius", "2m", cmd::ScriptItem::enter()});
    CHECK(entityAs<CircleData>(*doc.find(c1))->radius == doctest::Approx(2000));
    CHECK(host.undo());
    CHECK(entityAs<CircleData>(*doc.find(c1))->radius == doctest::Approx(25));
}

TEST_CASE("number expressions and units") {
    CHECK(parseNumberExpr("100/3").value() == doctest::Approx(100.0 / 3));
    CHECK(parseNumberExpr("2*(3+4)").value() == doctest::Approx(14));
    CHECK(parseNumberExpr("-5").value() == doctest::Approx(-5));
    CHECK(parseNumberExpr("2.5m").value() == doctest::Approx(2500));
    CHECK(parseNumberExpr("30cm").value() == doctest::Approx(300));
    CHECK(parseNumberExpr("1in").value() == doctest::Approx(25.4));
    CHECK(parseNumberExpr("1ft").value() == doctest::Approx(304.8));
    CHECK(parseNumberExpr("45", true).value() == doctest::Approx(45));
    CHECK_FALSE(parseNumberExpr("45mm", true).ok());
    CHECK_FALSE(parseNumberExpr("").ok());
    CHECK_FALSE(parseNumberExpr("1/0").ok());
    CHECK_FALSE(parseNumberExpr("abc").ok());
    CHECK_FALSE(parseNumberExpr("(1+2").ok());
    CHECK_FALSE(parseNumberExpr("5 furlongs").ok());
}

TEST_CASE("geometry fields of point, ellipse, text, insert and ray are readable and editable") {
    Document doc;
    EntityId pt = addEntity(doc, PointData{{3, 4}});
    EllipseData el;
    el.center = {5, 5};
    el.majorAxis = {6, 0};
    el.ratio = 0.5;
    EntityId e = addEntity(doc, Indirect<EllipseData>(el));
    TextData td;
    td.position = {1, 2};
    td.alignPoint = {1, 2};
    td.utf8 = "Hi";
    td.height = 2.5;
    EntityId t = addEntity(doc, Indirect<TextData>(td));
    RayData rd;
    rd.origin = {0, 0};
    rd.dir = {1, 0};
    EntityId r = addEntity(doc, Indirect<RayData>(rd));
    auto val = [&](EntityId id, const char* f) {
        for (auto& g : geometryFields(*doc.find(id)))
            if (g.id == f)
                return g.value;
        FAIL("missing field");
        return 0.0;
    };
    CHECK(val(pt, "x") == doctest::Approx(3));
    CHECK(val(e, "majorradius") == doctest::Approx(6));
    CHECK(val(e, "minorradius") == doctest::Approx(3));
    CHECK(val(t, "height") == doctest::Approx(2.5));
    CHECK(val(r, "angle") == doctest::Approx(0));

    auto tx = doc.begin("edit");
    REQUIRE(setGeometryField(tx, pt, "y", 9).ok());
    REQUIRE(setGeometryField(tx, e, "minorradius", 2).ok());
    CHECK_FALSE(setGeometryField(tx, e, "minorradius", 7).ok());
    REQUIRE(setGeometryField(tx, t, "x", 10).ok());
    CHECK_FALSE(setGeometryField(tx, t, "height", 0).ok());
    REQUIRE(setGeometryField(tx, r, "angle", 90).ok());
    tx.commit();
    CHECK(entityAs<PointData>(*doc.find(pt))->p.y == doctest::Approx(9));
    CHECK(entityAs<EllipseData>(*doc.find(e))->ratio == doctest::Approx(1.0 / 3));
    CHECK(entityAs<TextData>(*doc.find(t))->position.x == doctest::Approx(10));
    CHECK(entityAs<TextData>(*doc.find(t))->alignPoint.x == doctest::Approx(10));
    CHECK(entityAs<RayData>(*doc.find(r))->dir.y == doctest::Approx(1));
}

namespace {

// One of every kind the palette has fields for.
std::vector<EntityId> sampleKinds(Document& doc) {
    std::vector<EntityId> ids;
    ids.push_back(addEntity(doc, LineData{{0, 0}, {10, 5}}));
    ids.push_back(addEntity(doc, CircleData{{3, 3}, 4}));
    ids.push_back(addEntity(doc, ArcData{{0, 0}, 5, 0.2, 2.0}));
    ids.push_back(addEntity(doc, PolylineData{{{{0, 0}, 0, 0, 0}, {{4, 1}, 0, 0, 0}}, false, 0}));
    ids.push_back(addEntity(doc, PointData{{3, 4}}));
    EllipseData el;
    el.center = {5, 5};
    el.majorAxis = {6, 0};
    el.ratio = 0.5;
    el.endParam = 6.0;
    ids.push_back(addEntity(doc, Indirect<EllipseData>(el)));
    TextData td;
    td.position = td.alignPoint = {1, 2};
    td.utf8 = "Hello";
    ids.push_back(addEntity(doc, Indirect<TextData>(td)));
    MTextData md;
    md.position = {2, 2};
    md.utf8 = "Multi";
    md.width = 30;
    ids.push_back(addEntity(doc, Indirect<MTextData>(md)));
    RayData rd;
    rd.dir = {1, 0};
    ids.push_back(addEntity(doc, Indirect<RayData>(rd)));
    DimensionData dd;
    dd.kind = DimKind::Aligned;
    dd.defPts = {Vec2{0, 0}, Vec2{40, 0}, Vec2{20, 10}, Vec2{}};
    dd.textPos = {20, 10};
    ids.push_back(addEntity(doc, Indirect<DimensionData>(dd)));
    TableData tb;
    tb.origin = {0, 50};
    tb.colWidths = {20, 30};
    tb.rowHeights = {8, 8, 8};
    tb.cells.resize(6);
    ids.push_back(addEntity(doc, Indirect<TableData>(tb)));
    MLeaderData mld;
    mld.lines = {{Vec2{0, 0}, Vec2{10, 10}}};
    mld.landing = {10, 10};
    mld.contentPos = {12, 10};
    mld.utf8 = "Note";
    ids.push_back(addEntity(doc, Indirect<MLeaderData>(mld)));
    return ids;
}

}  // namespace

TEST_CASE("every editable geometry field of every kind changes with -PROP Set and undoes") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    const auto ids = sampleKinds(doc);
    std::size_t edited = 0;
    for (EntityId id : ids) {
        host.setSelection({id});
        for (const GeometryField& f : geometryFields(*doc.find(id))) {
            if (!f.editable || f.type == FieldType::Count)
                continue;
            const char* nv = f.type == FieldType::Flag ? (f.value != 0 ? "No" : "Yes") : nullptr;
            double target = f.type == FieldType::Angle ? f.value + 10 : f.value * 0.7 + 3.3;
            if (f.id == "ratio")
                target = 0.25;
            if (f.id == "width" || f.id == "scale")
                target = 2;
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.9g", target);
            host.clearLog();
            host.run("-PROP", {"Set", f.id, nv ? nv : buf, cmd::ScriptItem::enter()});
            double now = -1e30;
            for (const GeometryField& g : geometryFields(*doc.find(id)))
                if (g.id == f.id)
                    now = g.value;
            INFO(kindLabel(kind(*doc.find(id))), " ", f.id);
            CHECK(now != doctest::Approx(f.value));
            CHECK(host.undo());
            for (const GeometryField& g : geometryFields(*doc.find(id)))
                if (g.id == f.id)
                    CHECK(g.value == doctest::Approx(f.value));
            ++edited;
        }
    }
    CHECK(edited > 40);
}

TEST_CASE("text, multiline text: contents, style and justification edit and undo") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    const auto ids = sampleKinds(doc);
    const EntityId text = ids[6], mtext = ids[7];

    auto field = [&](EntityId id, const char* f) {
        for (const StringField& s : stringFields(doc, *doc.find(id)))
            if (s.id == f)
                return s;
        FAIL("missing field");
        return StringField{};
    };
    CHECK(field(text, "contents").value == "Hello");
    CHECK(field(text, "justify").value == "Left");
    CHECK(field(mtext, "attachment").value == "Top Left");
    CHECK(field(text, "style").choices.size() >= 1);

    host.setSelection({text, mtext});
    host.run("-PROP", {"Set", "contents", "Changed", cmd::ScriptItem::enter()});
    CHECK(entityAs<TextData>(*doc.find(text))->utf8 == "Changed");
    CHECK(entityAs<MTextData>(*doc.find(mtext))->utf8 == "Changed");
    CHECK(host.undo());
    CHECK(entityAs<TextData>(*doc.find(text))->utf8 == "Hello");
    CHECK(entityAs<MTextData>(*doc.find(mtext))->utf8 == "Multi");

    host.setSelection({text});
    host.run("-PROP", {"Set", "justify", "middle center", cmd::ScriptItem::enter()});
    CHECK(field(text, "justify").value == "Middle Center");
    host.clearLog();
    host.run("-PROP", {"Set", "justify", "Sideways", cmd::ScriptItem::enter()});
    CHECK(host.logContains("not a choice"));
    CHECK(field(text, "justify").value == "Middle Center");
    CHECK(host.undo());
    CHECK(field(text, "justify").value == "Left");

    host.setSelection({mtext});
    host.run("-PROP", {"Set", "attachment", "Bottom Right", cmd::ScriptItem::enter()});
    CHECK(field(mtext, "attachment").value == "Bottom Right");
    CHECK(host.undo());
    CHECK(field(mtext, "attachment").value == "Top Left");

    host.clearLog();
    host.run("-PROP", {"Set", "style", "NoSuchStyle", cmd::ScriptItem::enter()});
    CHECK(host.logContains("not a choice"));
    host.clearLog();
    host.run("-PROP", {"Set", "contents", " ", cmd::ScriptItem::enter()});
}

TEST_CASE("mixed selection: common values vary, Only narrows the edit to one type") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    const auto ids = sampleKinds(doc);
    {
        auto tx = doc.begin("colour");
        REQUIRE(tx.modify(ids[0], [](Entity& e) { e.common.color = Color::index(1); }).ok());
        tx.commit();
    }
    host.setSelection(ids);
    CommonProps all = readCommon(doc, ids);
    CHECK(all.count == ids.size());
    CHECK(all.kinds.size() == ids.size());
    CHECK(all.color.varies);
    CHECK_FALSE(all.layer.varies);
    CommonProps circles = readCommon(doc, ids, EntityKind::Circle);
    CHECK(circles.count == 1);
    CHECK_FALSE(circles.color.varies);

    host.run("-PROP", {"Only", "Circle", "Color", "blue", cmd::ScriptItem::enter()});
    CHECK(doc.find(ids[1])->common.color == Color::index(5));
    CHECK(doc.find(ids[2])->common.color == Color::byLayer());
    CHECK(doc.find(ids[0])->common.color == Color::index(1));
    CHECK(host.undo());
    CHECK(doc.find(ids[1])->common.color == Color::byLayer());
    // Resetting the whole mixed selection removes the "varies" state.
    host.run("-PROP", {"Color", "ByLayer", cmd::ScriptItem::enter()});
    CHECK_FALSE(readCommon(doc, ids).color.varies);
}

TEST_CASE("read-only fields: deltas of a line, area of an arc and ellipse") {
    Document doc;
    const auto ids = sampleKinds(doc);
    auto val = [&](EntityId id, const char* f) {
        for (auto& g : geometryFields(*doc.find(id)))
            if (g.id == f) {
                CHECK_FALSE(g.editable);
                return g.value;
            }
        FAIL("missing field");
        return 0.0;
    };
    CHECK(val(ids[0], "deltax") == doctest::Approx(10));
    CHECK(val(ids[0], "deltay") == doctest::Approx(5));
    CHECK(val(ids[2], "area") == doctest::Approx(0.5 * 25 * 1.8));
    CHECK(val(ids[5], "area") == doctest::Approx(kPi * 36 * 0.5));
    CHECK(val(ids[2], "endx") == doctest::Approx(5 * std::cos(2.0)));
}

TEST_CASE("polyline Current vertex: vertex X and Y and segment widths edit one vertex") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    EntityId pl = addEntity(
        doc, PolylineData{
                 {{{0, 0}, 0, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 5}, 0, 0, 0}}, false, false, 0.5});
    auto field = [&](const std::string& id, int vertex) {
        for (const GeometryField& f : geometryFields(*doc.find(pl), vertex))
            if (f.id == id)
                return f.value;
        return -1e30;
    };
    CHECK(field("vertex", 1) == 2);
    CHECK(field("vertexx", 1) == 10);
    CHECK(field("vertexy", 2) == 5);
    CHECK(field("vertex", 9) == 3);  // clamped to the last vertex
    host.setSelection({pl});
    host.run("-PROP", {"Set", "vertexy@2", "7", cmd::ScriptItem::enter()});
    const auto* p = entityAs<PolylineData>(*doc.find(pl));
    CHECK(p->v[1].p.y == 7);
    CHECK(p->v[0].p.y == 0);
    CHECK(host.undo());
    CHECK(entityAs<PolylineData>(*doc.find(pl))->v[1].p.y == 0);
    // a segment width takes over from the global width, the other segments keep 0.5
    host.run("-PROP", {"Set", "endwidth@1", "2", cmd::ScriptItem::enter()});
    p = entityAs<PolylineData>(*doc.find(pl));
    CHECK(p->constantWidth == 0);
    CHECK(p->v[0].startWidth == 0.5);
    CHECK(p->v[0].endWidth == 2);
    CHECK(p->v[1].startWidth == 0.5);
    host.clearLog();
    host.run("-PROP", {"Set", "vertexx@4", "1", cmd::ScriptItem::enter()});
    CHECK(host.logContains("3 vertices"));
    host.clearLog();
    host.run("-PROP", {"Set", "vertexx@0", "1", cmd::ScriptItem::enter()});
    CHECK(host.logContains("counted from 1"));
}

TEST_CASE("Properties show hatch area, dimension measurement, table size and multileader rows") {
    Document doc;
    const auto ids = sampleKinds(doc);
    auto field = [&](EntityId id, const std::string& name) {
        for (const GeometryField& f : geometryFields(*doc.find(id)))
            if (f.id == name)
                return f.value;
        return -1e30;
    };
    const EntityId dimId = ids[9], table = ids[10], mleader = ids[11];
    CHECK(field(dimId, "measurement") == doctest::Approx(40));
    CHECK(field(table, "tablewidth") == doctest::Approx(50));
    CHECK(field(table, "tableheight") == doctest::Approx(24));
    CHECK(field(table, "rows") == 3);
    CHECK(field(table, "columns") == 2);
    CHECK(field(mleader, "landingx") == doctest::Approx(10));
    HatchData h;
    HatchLoop loop;
    loop.edges.push_back(HatchPolylineEdge{
        {{{0, 0}, 0, 0, 0}, {{4, 0}, 0, 0, 0}, {{4, 4}, 0, 0, 0}, {{0, 4}, 0, 0, 0}}, true});
    h.loops.push_back(loop);
    EntityId hatchId = addEntity(doc, Indirect<HatchData>(h));
    CHECK(field(hatchId, "area") == doctest::Approx(16));

    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    host.setSelection({mleader});
    host.run("-PROP", {"Set", "landingx", "20", cmd::ScriptItem::enter()});
    const auto* ml = entityAs<MLeaderData>(*doc.find(mleader));
    CHECK(ml->landing.x == 20);
    CHECK(ml->contentPos.x == 22);  // the note goes with the landing
    host.setSelection({dimId});
    host.run("-PROP", {"Set", "textrotation", "30", cmd::ScriptItem::enter()});
    const auto* d = entityAs<DimensionData>(*doc.find(dimId));
    CHECK(d->textRotated);
    CHECK(d->textRotation == doctest::Approx(kPi / 6));
}

TEST_CASE("Properties edit a hatch pattern, scale, angle and origin - and undo") {
    Document doc;
    HatchData h;
    h.solid = false;
    h.pattern = "LINE45";
    HatchLoop loop;
    loop.edges.push_back(HatchPolylineEdge{
        {{{0, 0}, 0, 0, 0}, {{4, 0}, 0, 0, 0}, {{4, 4}, 0, 0, 0}, {{0, 4}, 0, 0, 0}}, true});
    h.loops.push_back(loop);
    const EntityId id = addEntity(doc, Indirect<HatchData>(h));
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    host.setSelection({id});
    host.run("-PROP", {"Set", "patternscale", "2.5", cmd::ScriptItem::enter()});
    host.run("-PROP", {"Set", "patternangle", "30", cmd::ScriptItem::enter()});
    host.run("-PROP", {"Set", "originx", "1", cmd::ScriptItem::enter()});
    host.run("-PROP", {"Set", "pattern", "CROSS45", cmd::ScriptItem::enter()});
    const auto* now = entityAs<HatchData>(*doc.find(id));
    CHECK(now->patternScale == doctest::Approx(2.5));
    CHECK(now->patternAngle == doctest::Approx(kPi / 6));
    CHECK(now->patternOrigin.x == doctest::Approx(1));
    CHECK(now->pattern == "CROSS45");
    CHECK(host.undo());
    CHECK(entityAs<HatchData>(*doc.find(id))->pattern == "LINE45");
    host.run("-PROP", {"Set", "patternscale", "0", cmd::ScriptItem::enter()});
    CHECK(entityAs<HatchData>(*doc.find(id))->patternScale == doctest::Approx(2.5));
}

TEST_CASE("Properties edit the line spacing of multiline text - range checked - and undo") {
    Document doc;
    MTextData m;
    m.utf8 = "a\\Pb";
    const EntityId id = addEntity(doc, Indirect<MTextData>(m));
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    host.setSelection({id});
    host.run("-PROP", {"Set", "linespacing", "1.5", cmd::ScriptItem::enter()});
    CHECK(entityAs<MTextData>(*doc.find(id))->lineSpacing == doctest::Approx(1.5));
    // A refused value leaves no step behind: one undo takes back the 1.5.
    host.run("-PROP", {"Set", "linespacing", "9", cmd::ScriptItem::enter()});
    CHECK(entityAs<MTextData>(*doc.find(id))->lineSpacing == doctest::Approx(1.5));
    CHECK(host.undo());
    CHECK(entityAs<MTextData>(*doc.find(id))->lineSpacing == doctest::Approx(1.0));
}

TEST_CASE("Properties edit attribute values, dimension override and style, table text style") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    const auto ids = sampleKinds(doc);
    const EntityId dimId = ids[9], table = ids[10], mleader = ids[11];
    auto word = [&](EntityId id, const std::string& name) {
        for (const StringField& f : stringFields(doc, *doc.find(id)))
            if (f.id == name)
                return f.value;
        return std::string("<none>");
    };
    host.setSelection({dimId});
    host.run("-PROP", {"Set", "textoverride", "<> TYP.", cmd::ScriptItem::enter()});
    CHECK(entityAs<DimensionData>(*doc.find(dimId))->textOverride == "<> TYP.");
    host.clearLog();
    host.run("-PROP", {"Set", "dimstyle", "NoSuchStyle", cmd::ScriptItem::enter()});
    CHECK(host.logContains("not a choice"));
    CHECK(word(dimId, "dimstyle") == word(mleader, "dimstyle"));
    host.setSelection({mleader});
    host.run("-PROP", {"Set", "contents", "Changed note", cmd::ScriptItem::enter()});
    CHECK(entityAs<MLeaderData>(*doc.find(mleader))->utf8 == "Changed note");
    CHECK(word(table, "style") == "Standard");

    // an insert with two attributes
    BlockId blk;
    {
        auto tx = doc.begin("block");
        BlockDef def;
        def.name = "TAG";
        blk = tx.addBlock(std::move(def)).value();
        tx.commit();
    }
    InsertData in;
    in.block = blk;
    in.attribs = {{"ROOM", "101", {}}, {"Area", "12", {}}};
    EntityId ins = addEntity(doc, Indirect<InsertData>(in));
    CHECK(word(ins, "att:room") == "101");
    CHECK(word(ins, "att:area") == "12");
    host.setSelection({ins});
    host.run("-PROP", {"Set", "att:ROOM", "102", cmd::ScriptItem::enter()});
    CHECK(entityAs<InsertData>(*doc.find(ins))->attribs[0].value == "102");
    CHECK(entityAs<InsertData>(*doc.find(ins))->attribs[1].value == "12");
    CHECK(host.undo());
    CHECK(entityAs<InsertData>(*doc.find(ins))->attribs[0].value == "101");

    // a multileader whose block has an attribute
    MLeaderData ml;
    ml.lines = {{{0, 0}, {5, 5}}};
    ml.landing = {5, 5};
    ml.contentPos = {7, 5};
    ml.content = MLeaderData::Content::Block;
    ml.block = blk;
    ml.attribs = {{"ROOM", "7", {}}};
    EntityId tagged = addEntity(doc, Indirect<MLeaderData>(ml));
    CHECK(word(tagged, "att:room") == "7");
    CHECK(word(tagged, "contents") == "<none>");
    host.setSelection({tagged});
    host.run("-PROP", {"Set", "att:room", "8", cmd::ScriptItem::enter()});
    CHECK(entityAs<MLeaderData>(*doc.find(tagged))->attribs[0].value == "8");
}

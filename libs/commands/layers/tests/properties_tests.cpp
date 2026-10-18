#include <doctest/doctest.h>

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

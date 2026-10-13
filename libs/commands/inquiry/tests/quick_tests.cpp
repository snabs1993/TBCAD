#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/inquiry/inquiry.h"

using namespace tbcad;
using namespace tbcad::inquiry;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    Fixture() { registerAllCommands(reg); }

    EntityId add(EntityData d) {
        Transaction tx = doc.begin("setup");
        EntityId id = tx.add(Entity{{}, std::move(d)}).value();
        tx.commit();
        return id;
    }
    std::size_t count() const {
        std::size_t n = 0;
        doc.forEachEntity([&](const Entity&) { ++n; });
        return n;
    }
};

PolylineData rect(Vec2 o, double w, double h) {
    PolylineData pl;
    pl.closed = true;
    pl.v = {{{o.x, o.y}}, {{o.x + w, o.y}}, {{o.x + w, o.y + h}}, {{o.x, o.y + h}}};
    return pl;
}

}  // namespace

TEST_CASE("quickMeasure: length of a line and the gap to a parallel line") {
    Fixture f;
    f.add(LineData{{0, 0}, {10, 0}});
    f.add(LineData{{0, 4}, {10, 4}});
    std::vector<const Entity*> near;
    f.doc.forEachEntity([&](const Entity& e) { near.push_back(&e); });
    auto m = quickMeasure(f.doc, near, {5, 1}, 2);
    REQUIRE(m.size() == 3);
    CHECK(m[0].kind == Measurement::Kind::Length);
    CHECK(m[0].value == doctest::Approx(10));
    CHECK(m[2].kind == Measurement::Kind::Distance);
    CHECK(m[2].value == doctest::Approx(4));
    CHECK(m[2].text == "Distance 4.00 mm");
}

TEST_CASE("quickMeasure: radius, and no distance for touching objects") {
    Fixture f;
    f.add(CircleData{{0, 0}, 5});
    f.add(LineData{{5, -3}, {5, 3}});  // touches the circle
    std::vector<const Entity*> near;
    f.doc.forEachEntity([&](const Entity& e) { near.push_back(&e); });
    auto m = quickMeasure(f.doc, near, {0, 5}, 2);
    REQUIRE(m.size() == 2);
    CHECK(m[0].kind == Measurement::Kind::Radius);
    CHECK(m[0].value == doctest::Approx(5));
}

TEST_CASE("quickMeasure: separate circles give the gap between their rims") {
    Fixture f;
    f.add(CircleData{{0, 0}, 5});
    f.add(CircleData{{20, 0}, 5});
    std::vector<const Entity*> near;
    f.doc.forEachEntity([&](const Entity& e) { near.push_back(&e); });
    auto m = quickMeasure(f.doc, near, {10, 0}, 2);
    REQUIRE(m.size() == 3);
    CHECK(m.back().value == doctest::Approx(10).epsilon(1e-3));
}

TEST_CASE("quickMeasure: nothing near gives nothing") {
    Fixture f;
    CHECK(quickMeasure(f.doc, {}, {0, 0}, 2).empty());
}

TEST_CASE("QUICKMEASURE lists what is under a click and never edits the drawing") {
    Fixture f;
    f.add(LineData{{0, 0}, {10, 0}});
    f.add(LineData{{0, 4}, {10, 4}});
    cmd::ScriptHost host(f.doc, f.reg);
    host.setSetting("inquiry.precision", "1");
    host.run("QM", {"5,1"});
    CHECK(host.logContains("Length 10.0 mm"));
    CHECK(host.logContains("Distance 4.0 mm"));
    host.feed({cmd::ScriptItem::esc()});
    CHECK_FALSE(host.undoStack().canUndo());
}

TEST_CASE("QUICKMEASURE says so when nothing is there") {
    Fixture f;
    cmd::ScriptHost host(f.doc, f.reg);
    host.run("QM", {"5,1"});
    CHECK(host.logContains("nothing to measure"));
    host.feed({cmd::ScriptItem::esc()});
}

TEST_CASE("buildAreaTable: header, rows and a total") {
    Fixture f;
    auto a = f.add(rect({0, 0}, 10, 5));
    auto b = f.add(CircleData{{50, 0}, 2});
    auto open = f.add(LineData{{0, 0}, {1, 1}});
    std::vector<const Entity*> shapes{f.doc.find(a), f.doc.find(b), f.doc.find(open)};
    auto parts = buildAreaTable(f.doc, shapes, {100, 100}, {});
    std::size_t lines = 0;
    std::vector<std::string> texts;
    for (const auto& d : parts) {
        if (std::holds_alternative<LineData>(d))
            ++lines;
        else {
            const Entity e{{}, d};
            if (const auto* t = entityAs<TextData>(e))
                texts.push_back(t->utf8);
        }
    }
    CHECK(lines == 10);  // 4 rows -> 5 horizontal lines, 4 columns -> 5 vertical lines
    auto has = [&](const std::string& t) {
        return std::find(texts.begin(), texts.end(), t) != texts.end();
    };
    CHECK(has("Area (mm^2)"));
    CHECK(has("Rectangle") == false);
    CHECK(has("Polyline"));
    CHECK(has("Circle"));
    CHECK(has("50.00"));
    CHECK(has("12.57"));
    CHECK(has("62.57"));  // total
    CHECK(has("30.00"));  // rectangle perimeter
    CHECK_FALSE(has("Line"));
}

TEST_CASE("buildAreaTable: nothing closed gives no table") {
    Fixture f;
    auto open = f.add(LineData{{0, 0}, {1, 1}});
    CHECK(buildAreaTable(f.doc, {f.doc.find(open)}, {0, 0}, {}).empty());
}

TEST_CASE("AREATABLE adds lines and text in one undo step") {
    Fixture f;
    f.add(rect({0, 0}, 10, 5));
    f.add(CircleData{{50, 0}, 2});
    cmd::ScriptHost host(f.doc, f.reg);
    const std::size_t before = f.count();
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    auto r = host.run("AT", {cmd::ScriptItem::select(ids), "", "0,-20"});
    CHECK(r.finished);
    CHECK(host.logContains("Area table created for 2 shape(s)"));
    CHECK(f.count() > before + 10);
    host.undo();
    CHECK(f.count() == before);
}

TEST_CASE("AREATABLE refuses a selection without closed shapes") {
    Fixture f;
    auto l = f.add(LineData{{0, 0}, {1, 1}});
    cmd::ScriptHost host(f.doc, f.reg);
    auto r = host.run("AT", {cmd::ScriptItem::select({l}), ""});
    CHECK(r.finished);
    CHECK(host.logContains("closed shape"));
    CHECK(f.count() == 1);
}

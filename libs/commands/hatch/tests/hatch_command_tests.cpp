#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/hatchcmd/hatchcmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

// Test command: stretches the rectangle made by Fixture::rect({0,0},{100,60}) to x = 150.
class Widen final : public Command {
public:
    explicit Widen(const std::vector<EntityId>& ids) : ids_(ids) {}
    std::string_view name() const override { return "WIDEN"; }
    Task run(CommandContext& ctx) override {
        auto edit = [&](std::size_t i, auto fn) {
            (void)ctx.tx().modify(ids_[i], [&](Entity& e) { fn(*entityAs<LineData>(e)); });
        };
        edit(0, [](LineData& l) { l.b = {150, 0}; });
        edit(1, [](LineData& l) { l.a = {150, 0}, l.b = {150, 60}; });
        edit(2, [](LineData& l) { l.a = {150, 60}; });
        co_return;
    }

private:
    const std::vector<EntityId>& ids_;
};

struct Fixture {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;
    std::vector<EntityId> ids;

    Fixture() {
        registerBuiltinCommands(reg);
        hatchcmd::registerCommands(reg);
        (void)reg.add(std::make_unique<Widen>(ids));
        host = std::make_unique<ScriptHost>(doc, reg);
    }

    EntityId addEntity(EntityData d) {
        Transaction tx = doc.begin("setup");
        Entity e;
        e.data = std::move(d);
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        ids.push_back(id);
        return id;
    }
    void rect(Vec2 lo, Vec2 hi) {
        Transaction tx = doc.begin("setup");
        auto line = [&](Vec2 a, Vec2 b) {
            Entity e;
            e.data = LineData{a, b};
            ids.push_back(tx.add(std::move(e)).value());
        };
        line(lo, {hi.x, lo.y});
        line({hi.x, lo.y}, hi);
        line(hi, {lo.x, hi.y});
        line({lo.x, hi.y}, lo);
        tx.commit();
    }
    std::vector<const HatchData*> hatches() const {
        std::vector<const HatchData*> out;
        doc.forEachEntity([&](const Entity& e) {
            if (const HatchData* h = entityAs<HatchData>(e))
                out.push_back(h);
        });
        return out;
    }
};

}  // namespace

TEST_CASE("HATCH and HATCHEDIT are registered with their aliases, and the display generator") {
    Fixture f;
    REQUIRE(f.reg.find("HATCH") != nullptr);
    CHECK(f.reg.find("H") == f.reg.find("HATCH"));
    CHECK(f.reg.find("he") == f.reg.find("HATCHEDIT"));
    CHECK(geom::DisplayRegistry::has(EntityKind::Hatch));
}

TEST_CASE("HATCH: a pick point inside a closed shape adds one hatch with the current settings") {
    Fixture f;
    f.rect({0, 0}, {100, 60});
    auto r = f.host->run("HATCH", {"50,30", ""});
    CHECK(r.finished);
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    CHECK(hs[0]->loops.size() == 1);
    CHECK(hs[0]->loops[0].edges.size() == 4);
    CHECK(hs[0]->loops[0].outer);
    CHECK(hs[0]->pattern == "LINE45");
    CHECK_FALSE(hs[0]->solid);
    CHECK(hs[0]->patternScale == doctest::Approx(1));
    CHECK(hs[0]->sourceBoundary.size() == 4);
    CHECK(f.host->logContains("Hatch created (1 loop)"));
    CHECK(f.host->setting("ui.showPanel") == std::optional<std::string>("hatch"));
    CHECK(f.host->undoStack().undoLabel() == "HATCH");
    // The hatch is on the current layer.
    const Entity* e = nullptr;
    f.doc.forEachEntity([&](const Entity& x) {
        if (kind(x) == EntityKind::Hatch)
            e = &x;
    });
    REQUIRE(e != nullptr);
    CHECK(e->common.layer == f.doc.header().currentLayer);
    // One undo removes it.
    CHECK(f.host->undo());
    CHECK(f.hatches().empty());
}

TEST_CASE("HATCH: a big area gets a bigger pattern scale until the user sets one") {
    Fixture f;
    f.rect({0, 0}, {1200, 800});
    f.host->run("HATCH", {"600,400", ""});
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    CHECK(hs[0]->patternScale == doctest::Approx(10));  // 1200 / 100 = 12 -> 10
    CHECK_FALSE(f.host->setting("hatch.scale").has_value());
    f.host->run("HATCH", {"S", "3", "600,400", ""});
    hs = f.hatches();
    REQUIRE(hs.size() == 2);
    CHECK(hs[1]->patternScale == doctest::Approx(3));
    CHECK(f.host->setting("hatch.scale").value() == "3");
}

TEST_CASE("HATCH: options change pattern, scale, angle and base - the choices are remembered") {
    Fixture f;
    f.rect({0, 0}, {100, 100});
    auto r =
        f.host->run("HATCH", {"P", "cross45", "S", "2.5", "A", "30", "B", "10,20", "50,50", ""});
    CHECK(r.finished);
    CHECK(r.rejected == 0);
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    CHECK(hs[0]->pattern == "CROSS45");
    CHECK(hs[0]->patternScale == doctest::Approx(2.5));
    CHECK(hs[0]->patternAngle == doctest::Approx(30 * kPi / 180));
    CHECK(hs[0]->patternOrigin == Vec2{10, 20});

    // The next run starts with the same settings.
    f.host->run("HATCH", {"50,50", ""});
    hs = f.hatches();
    REQUIRE(hs.size() == 2);
    CHECK(hs[1]->pattern == "CROSS45");
    CHECK(hs[1]->patternScale == doctest::Approx(2.5));
    CHECK(f.host->setting("hatch.pattern").value() == "CROSS45");
}

TEST_CASE("HATCH: SOLID makes a solid hatch - unknown patterns are refused - ? lists them") {
    Fixture f;
    f.rect({0, 0}, {10, 10});
    f.host->run("HATCH", {"P", "?", "P", "NOPE", "P", "solid", "5,5", ""});
    CHECK(f.host->logContains("Patterns: SOLID, LINE45"));
    CHECK(f.host->logContains("Unknown pattern \"NOPE\""));
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    CHECK(hs[0]->solid);
    CHECK(hs[0]->pattern == "SOLID");
}

TEST_CASE("HATCH: an outline with a small gap says how big the gap is") {
    Fixture f;
    f.addEntity(LineData{{0, 0}, {10, 0}});
    f.addEntity(LineData{{10, 0}, {10, 10}});
    f.addEntity(LineData{{10, 10}, {0, 10}});
    f.addEntity(LineData{{0, 10}, {0, 0.4}});  // stops 0.4 short of the first corner
    f.host->run("HATCH", {"5,5", ScriptItem::esc()});
    CHECK(f.hatches().empty());
    CHECK(f.host->logContains("Set Gap to 0.4"));
}

TEST_CASE(
    "HATCH: a point with no closed boundary reports it and asks again; Esc keeps earlier work") {
    Fixture f;
    f.rect({0, 0}, {10, 10});
    f.addEntity(LineData{{100, 0}, {110, 0}});  // an open line elsewhere
    auto r = f.host->run("HATCH", {"105,5", "5,5", ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.host->logContains("closed boundary"));
    CHECK(f.hatches().size() == 1);
    CHECK(f.host->undoStack().canUndo());

    // Esc with nothing made leaves no undo step.
    Fixture g;
    g.rect({0, 0}, {10, 10});
    g.host->run("HATCH", {"500,500", ScriptItem::esc()});
    CHECK(g.hatches().empty());
    CHECK_FALSE(g.host->undoStack().canUndo());
}

TEST_CASE("HATCH: several picks in one run, and the Undo option removes only the last") {
    Fixture f;
    f.rect({0, 0}, {10, 10});
    f.rect({20, 0}, {30, 10});
    f.rect({40, 0}, {50, 10});
    auto r = f.host->run("HATCH", {"5,5", "25,5", "45,5", "U", ""});
    CHECK(r.finished);
    // Separate is off: the areas of one run make one hatch, less the undone one.
    REQUIRE(f.hatches().size() == 1);
    CHECK(f.hatches()[0]->loops.size() == 2);
    CHECK(f.host->logContains("Last hatch removed"));
    // The whole run is one undo step.
    CHECK(f.host->undo());
    CHECK(f.hatches().empty());
    CHECK(f.host->redo());
    CHECK(f.hatches().size() == 1);

    // SEparate on: one hatch per area.
    Fixture g;
    g.rect({0, 0}, {10, 10});
    g.rect({20, 0}, {30, 10});
    g.rect({40, 0}, {50, 10});
    g.host->run("HATCH", {"SE", "5,5", "25,5", "45,5", "U", ""});
    CHECK(g.hatches().size() == 2);
    CHECK(g.host->setting("hatch.separate").value() == "1");
}

TEST_CASE("HATCH: islands come along, nested rings alternate") {
    Fixture f;
    f.rect({0, 0}, {100, 100});
    f.addEntity(CircleData{{50, 50}, 30});
    f.addEntity(CircleData{{50, 50}, 15});
    f.host->run("HATCH", {"5,5", ""});
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    REQUIRE(hs[0]->loops.size() == 3);
    CHECK(hs[0]->loops[0].outer);
    CHECK_FALSE(hs[0]->loops[1].outer);
    CHECK(hs[0]->loops[2].outer);
}

TEST_CASE("HATCH: hidden objects and objects on off layers are not boundaries") {
    Fixture f;
    f.rect({0, 0}, {10, 10});
    {
        Transaction tx = f.doc.begin("hide");
        REQUIRE(tx.modify(f.ids[0], [](Entity& e) { e.common.visible = false; }));
        tx.commit();
    }
    f.host->run("HATCH", {"5,5", ScriptItem::esc()});
    CHECK(f.hatches().empty());

    Fixture g;
    g.rect({0, 0}, {10, 10});
    {
        Transaction tx = g.doc.begin("off");
        REQUIRE(tx.modifyLayer(kLayerZero, [](Layer& l) { l.on = false; }));
        tx.commit();
    }
    g.host->run("HATCH", {"5,5", ScriptItem::esc()});
    CHECK(g.hatches().empty());
}

TEST_CASE("HATCH: only what is in the current view counts") {
    Fixture f;
    f.rect({0, 0}, {1000, 1000});
    ViewState v;
    v.center = {500, 500};
    v.scale = 1;
    v.sizePx = {200, 200};  // sees x, y in [400, 600]: no side of the square is visible
    f.host->setView(v);
    f.host->run("HATCH", {"500,500", ScriptItem::esc()});
    CHECK(f.hatches().empty());
    CHECK(f.host->logContains("No objects in view"));

    v.sizePx = {1200, 1200};
    f.host->setView(v);
    f.host->run("HATCH", {"500,500", ""});
    CHECK(f.hatches().size() == 1);
}

TEST_CASE("HATCH: Objects mode hatches the selected shapes") {
    Fixture f;
    EntityId circle = f.addEntity(CircleData{{0, 0}, 10});
    f.addEntity(CircleData{{100, 0}, 10});  // not selected
    f.host->run("HATCH", {"O", ScriptItem::select({circle}), "", ""});
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    CHECK(hs[0]->loops.size() == 1);
    CHECK(hs[0]->sourceBoundary == std::vector<EntityId>{circle});
}

TEST_CASE("HATCHEDIT: changes pattern, scale, angle and origin in one undo step") {
    Fixture f;
    f.rect({0, 0}, {100, 100});
    f.host->run("HATCH", {"50,50", ""});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Hatch)
            id = e.common.id;
    });
    REQUIRE(id);
    f.host->setSelection({id});
    auto r = f.host->run("HATCHEDIT", {"P", "brick", "S", "4", "A", "90", "B", "3,4", "D"});
    CHECK(r.finished);
    CHECK(r.rejected == 0);
    const HatchData* h = entityAs<HatchData>(*f.doc.find(id));
    CHECK(h->pattern == "BRICK");
    CHECK(h->patternScale == doctest::Approx(4));
    CHECK(h->patternAngle == doctest::Approx(kPi / 2));
    CHECK(h->patternOrigin == Vec2{3, 4});
    CHECK(f.host->undoStack().undoLabel() == "Edit hatch");

    CHECK(f.host->undo());
    h = entityAs<HatchData>(*f.doc.find(id));
    CHECK(h->pattern == "LINE45");
    CHECK(h->patternScale == doctest::Approx(1));

    // Esc discards the edits made so far.
    f.host->setSelection({id});
    f.host->run("HATCHEDIT", {"S", "9", ScriptItem::esc()});
    CHECK(entityAs<HatchData>(*f.doc.find(id))->patternScale == doctest::Approx(1));
}

TEST_CASE("HATCHEDIT: switching to SOLID sets the solid flag - non-hatch selections are ignored") {
    Fixture f;
    f.rect({0, 0}, {10, 10});
    f.host->run("H", {"5,5", ""});
    EntityId hatch{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Hatch)
            hatch = e.common.id;
    });
    f.host->setSelection({f.ids[0], hatch});
    f.host->run("HE", {"P", "SOLID", ""});
    CHECK(entityAs<HatchData>(*f.doc.find(hatch))->solid);

    f.host->setSelection({f.ids[0]});
    auto r = f.host->run("HATCHEDIT", {ScriptItem::select({f.ids[1]}), ""});
    CHECK(r.finished);
    CHECK(f.host->logContains("No hatch objects selected"));
}

TEST_CASE("HATCHEDIT on 1,000 hatches is one undo step") {
    Fixture f;
    f.rect({0, 0}, {10, 10});
    f.host->run("HATCH", {"5,5", ""});
    const Entity* src = nullptr;
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Hatch)
            src = &e;
    });
    REQUIRE(src != nullptr);
    Entity copy = *src;
    std::vector<EntityId> ids;
    {
        Transaction tx = f.doc.begin("many");
        for (int i = 0; i < 1000; ++i)
            ids.push_back(tx.add(copy).value());
        tx.commit();
    }
    f.host->setSelection(ids);
    f.host->run("HATCHEDIT", {"S", "7", "D"});
    for (EntityId id : ids)
        REQUIRE(entityAs<HatchData>(*f.doc.find(id))->patternScale == doctest::Approx(7));
    CHECK(f.host->undo());
    for (EntityId id : ids)
        REQUIRE(entityAs<HatchData>(*f.doc.find(id))->patternScale == doctest::Approx(1));
}

TEST_CASE(
    "HATCH: hovering previews the region under the cursor, and the cache follows the drawing") {
    Fixture f;
    f.rect({0, 0}, {100, 60});
    f.host->run("HATCH", {});  // waits for a point
    f.host->runner().pointerMoved(RawInput{{50, 30}, {}, 1});
    const DisplayList& pv = f.host->preview();
    REQUIRE_FALSE(pv.items.empty());
    CHECK(std::holds_alternative<DisplayFill>(pv.items.front()));
    bool outline = false;
    for (const DisplayItem& it : pv.items)
        if (auto* pl = std::get_if<DisplayPolyline>(&it))
            outline = outline || pl->closed;
    CHECK(outline);
    // Outside the shape there is nothing to preview.
    f.host->runner().pointerMoved(RawInput{{500, 500}, {}, 1});
    CHECK(f.host->preview().items.empty());
    f.host->feed({ScriptItem::esc()});
    CHECK(f.hatches().empty());
}

TEST_CASE("HATCH: the host's window query supplies the candidates") {
    Fixture f;
    f.rect({0, 0}, {100, 60});
    ViewState v;
    v.center = {50, 30};
    v.scale = 1;
    v.sizePx = {400, 400};
    f.host->setView(v);
    int calls = 0;
    std::vector<EntityId> answer;
    f.host->setWindowQuery([&](const BBox2&) {
        ++calls;
        return answer;
    });
    // The index says nothing is there, so there is no boundary even though the document has one.
    f.host->run("HATCH", {"50,30", ScriptItem::esc()});
    CHECK(calls > 0);
    CHECK(f.hatches().empty());
    answer = f.ids;
    f.host->run("HATCH", {"50,30", ""});
    CHECK(f.hatches().size() == 1);
}

TEST_CASE("HATCH: Gap closes a small opening, Link off makes the hatch independent") {
    Fixture f;
    f.addEntity(LineData{{0, 0}, {100, 0}});
    f.addEntity(LineData{{100, 0}, {100, 60}});
    f.addEntity(LineData{{100, 60}, {0, 60}});
    f.addEntity(LineData{{0, 60}, {0, 0.5}});
    f.host->run("HATCH", {"50,30", ScriptItem::esc()});
    CHECK(f.hatches().empty());
    f.host->run("HATCH", {"G", "1", "L", "50,30", ""});
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    CHECK(hs[0]->loops[0].edges.size() == 5);
    CHECK(hs[0]->sourceBoundary.empty());  // Link was turned off
    CHECK(f.host->setting("hatch.gap").value() == "1");
    CHECK(f.host->setting("hatch.associative").value() == "0");
    f.host->run("HATCH", {"L", "50,30", ""});  // back on
    hs = f.hatches();
    REQUIRE(hs.size() == 2);
    CHECK_FALSE(hs[1]->sourceBoundary.empty());
}

TEST_CASE("an associative hatch follows its boundary in the same undo step") {
    Fixture f;
    f.rect({0, 0}, {100, 60});
    f.host->run("HATCH", {"50,30", ""});
    REQUIRE(f.hatches().size() == 1);
    CHECK(hatch::hatchBounds(*f.hatches()[0]).max.x == doctest::Approx(100));
    f.host->run("WIDEN", {});
    CHECK(hatch::hatchBounds(*f.hatches()[0]).max.x == doctest::Approx(150));
    CHECK(f.hatches()[0]->sourceBoundary.size() == 4);
    CHECK(f.host->undo());
    CHECK(hatch::hatchBounds(*f.hatches()[0]).max.x == doctest::Approx(100));
    CHECK(f.host->redo());
    CHECK(hatch::hatchBounds(*f.hatches()[0]).max.x == doctest::Approx(150));
}

TEST_CASE("a hatch made with Link off stays put when the boundary moves") {
    Fixture f;
    f.rect({0, 0}, {100, 60});
    f.host->run("HATCH", {"L", "50,30", ""});
    f.host->run("WIDEN", {});
    CHECK(hatch::hatchBounds(*f.hatches()[0]).max.x == doctest::Approx(100));
}

TEST_CASE("GRADIENT: makes a solid hatch with a gradient from the chosen colours") {
    Fixture f;
    f.rect({0, 0}, {100, 60});
    REQUIRE(f.reg.find("GR") == f.reg.find("GRADIENT"));
    auto r = f.host->run("GRADIENT",
                         {"K", "cylinder", "F", "255,0,0", "S", "#0000ff", "A", "90", "50,30", ""});
    CHECK(r.finished);
    CHECK(r.rejected == 0);
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    REQUIRE(hs[0]->gradient.has_value());
    CHECK(hs[0]->solid);
    CHECK(hs[0]->gradient->kind == GradientKind::Cylinder);
    CHECK(hs[0]->gradient->color1.r == 255);
    CHECK(hs[0]->gradient->color2.b == 255);
    CHECK(hs[0]->gradient->angle == doctest::Approx(kPi / 2));
    CHECK(f.host->setting("hatch.gradient.color2").value() == "0,0,255");
    // A bad colour is refused and the old one stays.
    f.host->run("GRADIENT", {"F", "banana", ScriptItem::esc()});
    CHECK(f.host->logContains("Not a colour"));
}

TEST_CASE("HATCHEDIT: choosing a pattern drops the gradient") {
    Fixture f;
    f.rect({0, 0}, {100, 60});
    f.host->run("GRADIENT", {"50,30", ""});
    REQUIRE(f.hatches().size() == 1);
    REQUIRE(f.hatches()[0]->gradient.has_value());
    EntityId id;
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Hatch)
            id = e.common.id;
    });
    f.host->setSelection({id});
    f.host->run("HATCHEDIT", {"P", "LINE45", "D"});
    REQUIRE(f.hatches().size() == 1);
    CHECK_FALSE(f.hatches()[0]->gradient.has_value());
    CHECK_FALSE(f.hatches()[0]->solid);
}

TEST_CASE("HATCH island detection: Normal alternates, Outer cuts one level, Ignore fills it all") {
    for (const char* style : {"Normal", "Outer", "Ignore"}) {
        Fixture f;
        f.rect({0, 0}, {100, 100});
        f.addEntity(CircleData{{50, 50}, 40});
        f.addEntity(CircleData{{50, 50}, 20});
        f.host->run("HATCH", {"I", style, "50,95", ""});  // between the rectangle and the circle
        auto hs = f.hatches();
        REQUIRE(hs.size() == 1);
        const std::string s = style;
        const std::size_t expected = s == "Normal" ? 3 : s == "Outer" ? 2 : 1;
        CHECK(hs[0]->loops.size() == expected);
        CHECK(hs[0]->sourceBoundary.size() == (s == "Normal" ? 6 : s == "Outer" ? 5 : 4));
    }
}

TEST_CASE("HATCH: an ellipse and a spline loop make closed boundaries") {
    Fixture f;
    EllipseData e;
    e.center = {0, 0};
    e.majorAxis = {30, 0};
    e.ratio = 0.5;
    f.addEntity(std::move(e));
    f.host->run("HATCH", {"0,0", ""});
    REQUIRE(f.hatches().size() == 1);
    CHECK(f.hatches()[0]->loops.size() == 1);

    SplineData sp;
    sp.degree = 3;
    sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    sp.control = {{100, 0}, {140, 60}, {170, 60}, {200, 0}};
    f.addEntity(std::move(sp));
    f.addEntity(LineData{{200, 0}, {100, 0}});  // closes the spline with a chord
    f.host->run("HATCH", {"150,10", ""});
    CHECK(f.hatches().size() == 2);
}

TEST_CASE("HATCH pick point answers quickly in a drawing of 50 000 objects") {
    Fixture f;
    {
        Transaction tx = f.doc.begin("fill");
        auto line = [&](Vec2 a, Vec2 b) {
            Entity e;
            e.data = LineData{a, b};
            REQUIRE(tx.add(std::move(e)).ok());
        };
        for (int i = 0; i < 12500; ++i) {  // 12 500 squares of four lines
            const double x = (i % 125) * 30.0, y = (i / 125) * 30.0;
            line({x, y}, {x + 20, y});
            line({x + 20, y}, {x + 20, y + 20});
            line({x + 20, y + 20}, {x, y + 20});
            line({x, y + 20}, {x, y});
        }
        tx.commit();
    }
    REQUIRE(f.doc.modelSpace().size() == 50000);
    auto t0 = std::chrono::steady_clock::now();
    f.host->run("HATCH", {"3010,1510", ""});  // inside one of the squares
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("50k objects: HATCH pick " << ms << " ms");
    REQUIRE(f.hatches().size() == 1);
    CHECK(f.hatches()[0]->loops.size() == 1);
    CHECK(f.hatches()[0]->loops[0].edges.size() == 4);
#ifdef NDEBUG
    CHECK(ms < 200);
#else
    CHECK(ms < 2000);
#endif
}

// A door block closes the doorway: its lines bound the hatch. The hatch is not linked, because a
// block reference has no curves of its own to re-derive the loops from.
TEST_CASE("HATCH: lines inside a block reference bound the area, by pick point and as objects") {
    Fixture f;
    f.addEntity(LineData{{0, 0}, {100, 0}});
    f.addEntity(LineData{{100, 0}, {100, 60}});
    f.addEntity(LineData{{100, 60}, {0, 60}});
    f.addEntity(LineData{{0, 60}, {0, 40}});
    f.addEntity(LineData{{0, 20}, {0, 0}});
    f.host->run("HATCH", {"50,30", ""});
    CHECK(f.hatches().empty());

    Transaction tx = f.doc.begin("block");
    const BlockId door = tx.addBlock(BlockDef{.name = "DOOR"}).value();
    REQUIRE(tx.add(Entity{{}, LineData{{0, 0}, {20, 0}}}, door));
    REQUIRE(tx.add(Entity{{}, ArcData{{0, 0}, 20, 0, kPi / 2}}, door));  // the swing
    tx.commit();
    const EntityId ins =
        f.addEntity(Indirect<InsertData>(InsertData{door, {0, 40}, {1, 1}, -kPi / 2}));
    f.host->run("HATCH", {"50,30", ""});
    auto hs = f.hatches();
    REQUIRE(hs.size() == 1);
    CHECK(hs[0]->sourceBoundary.empty());
    CHECK(hatch::hatchArea(*hs[0]) == doctest::Approx(6000).epsilon(1e-6));

    // Objects mode: the walls and the door block.
    CHECK(f.host->undo());
    std::vector<EntityId> picked(f.ids.begin(), f.ids.begin() + 5);
    picked.push_back(ins);
    f.host->run("HATCH", {"O", ScriptItem::select(picked), "", ""});
    hs = f.hatches();
    REQUIRE(hs.size() == 1);
    CHECK(hatch::hatchArea(*hs[0]) == doctest::Approx(6000).epsilon(1e-6));
}

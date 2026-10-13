#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/hatchcmd/hatchcmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

struct Fixture {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;
    std::vector<EntityId> ids;

    Fixture() {
        registerBuiltinCommands(reg);
        hatchcmd::registerCommands(reg);
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

TEST_CASE("HATCH: options change pattern, scale, angle and base; the choices are remembered") {
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

TEST_CASE("HATCH: SOLID makes a solid hatch; unknown patterns are refused; ? lists them") {
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
    CHECK(f.hatches().size() == 2);
    CHECK(f.host->logContains("Last hatch removed"));
    // The whole run is one undo step.
    CHECK(f.host->undo());
    CHECK(f.hatches().empty());
    CHECK(f.host->redo());
    CHECK(f.hatches().size() == 2);
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

TEST_CASE("HATCHEDIT: switching to SOLID sets the solid flag; non-hatch selections are ignored") {
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

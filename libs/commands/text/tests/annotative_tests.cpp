#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/geom/entity.h"

using namespace tbcad;

namespace {
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}
};

double textHeightOnScreen(const Document& doc, const Entity& e, double annoScale = 0) {
    geom::DisplayContext ctx;
    ctx.annoScale = annoScale;
    DisplayList d;
    geom::buildDisplay(doc, e, ctx, d);
    double top = -1e30, bot = 1e30;
    auto take = [&](const std::vector<Vec2>& pts) {
        for (Vec2 v : pts) {
            top = std::max(top, v.y);
            bot = std::min(bot, v.y);
        }
    };
    for (const auto& item : d.items) {
        if (const auto* p = std::get_if<DisplayPolyline>(&item))
            take(p->pts);
        else if (const auto* f = std::get_if<DisplayFill>(&item))
            for (const auto& r : f->rings)
                take(r);
    }
    return top - bot;
}
}  // namespace

TEST_CASE("annotation scale list and current scale are stored in the drawing") {
    Fixture f;
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(1));
    f.host.run("CANNOSCALE", {"50"});
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(50));
    f.host.undo();
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(1));
    f.host.run("CANNOSCALE", {"1:20"});  // the form people actually type
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(20));
    f.host.run("CANNOSCALE", {"abc"});  // rejected, unchanged
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(20));
    f.host.undo();
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(1));

    f.host.run("SCALELISTEDIT", {"Add", "40", "Delete", "1000", "List", "Done"});
    auto list = annotationScaleList(f.doc.header());
    CHECK(std::find(list.begin(), list.end(), 40.0) != list.end());
    CHECK(std::find(list.begin(), list.end(), 1000.0) == list.end());
    f.host.run("SCALELISTEDIT", {"Reset", "Done"});
    CHECK(annotationScaleList(f.doc.header()).size() == 13);
}

TEST_CASE("annotative text grows with the scale - ordinary text does not") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "Hi"});
    f.host.run("TEXT", {"0,10", "2.5", "0", "Hi"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(ids[0], {0, 0}), cmd::ScriptItem::enter(), "On"});
    CHECK(f.doc.find(ids[0])->common.annotative);
    CHECK_FALSE(f.doc.find(ids[1])->common.annotative);

    const double h1 = textHeightOnScreen(f.doc, *f.doc.find(ids[0]));
    f.host.run("CANNOSCALE", {"100"});
    const double h100 = textHeightOnScreen(f.doc, *f.doc.find(ids[0]));
    CHECK(h100 == doctest::Approx(h1 * 100).epsilon(0.01));
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[1])) == doctest::Approx(h1).epsilon(0.01));
    // A viewport overrides the current scale.
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[0]), 10) ==
          doctest::Approx(h1 * 10).epsilon(0.01));
}

TEST_CASE("an object keeps its own scales - new objects follow ANNONEW") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "Hi"});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) { id = e.common.id; });
    const double h1 = textHeightOnScreen(f.doc, *f.doc.find(id));
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(id, {0, 0}), cmd::ScriptItem::enter(), "Scales", "50,100"});
    const Entity* e = f.doc.find(id);
    REQUIRE(e);
    CHECK(e->common.annotative);
    REQUIRE(e->common.annoScales.size() == 2);
    // With ANNOAUTOSCALE off a scale it does not list leaves it at its first scale.
    f.host.run("ANNOAUTOSCALE", {"0"});
    f.host.run("CANNOSCALE", {"200"});
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(id)) == doctest::Approx(h1 * 50).epsilon(0.01));
    f.host.undo();
    f.host.undo();

    f.host.run("ANNONEW", {"On"});
    f.host.run("CANNOSCALE", {"20"});
    f.host.run("TEXT", {"0,10", "2.5", "0", "New"});
    EntityId made{};
    f.doc.forEachEntity([&](const Entity& x) {
        if (x.common.id != id)
            made = x.common.id;
    });
    REQUIRE(made != EntityId{});
    const Entity* n = f.doc.find(made);
    CHECK(n->common.annotative);
    REQUIRE(n->common.annoScales.size() == 1);
    CHECK(n->common.annoScales.front() == doctest::Approx(20));
}

TEST_CASE("a symbol is rescaled when its annotation scale changes") {
    Fixture f;
    f.host.run("LINE", {"0,0", "1,0", ""});
    EntityId line{};
    f.doc.forEachEntity([&](const Entity& e) { line = e.common.id; });
    f.host.setSelection({line});
    f.host.run("BLOCK", {"Mark", "0,0", "D"});
    f.host.run("INSERT", {"Mark", "10,0"});
    EntityId ins{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Insert)
            ins = e.common.id;
    });
    REQUIRE(ins != EntityId{});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(1));
    f.host.setSelection({ins});
    f.host.run("ANNOTATIVE", {"Scales", "50"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(50));
    f.host.setSelection({ins});
    f.host.run("ANNOTATIVE", {"Scales", "100"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(100));
    f.host.setSelection({ins});
    f.host.run("ANNOTATIVE", {"Off"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(1));
}

TEST_CASE("ANNOTATIVE warns when text already sized for the drawing would be multiplied again") {
    Fixture f;
    f.host.run("CANNOSCALE", {"100"});
    EntityId big;
    {
        auto tx = f.doc.begin("t");
        Entity e;
        TextData t;
        t.height = 250;
        t.utf8 = "Big";
        e.data = Indirect<TextData>(t);
        big = tx.add(std::move(e)).value();
        tx.commit();
    }
    f.host.setSelection({big});
    f.host.run("ANNOTATIVE", {"On"});
    CHECK(f.host.logContains("drawn very large"));
}

TEST_CASE("OBJECTSCALE adds and removes scales but keeps at least one") {
    Fixture f;
    f.host.run("LINE", {"0,0", "1,0", ""});
    EntityId line{};
    f.doc.forEachEntity([&](const Entity& e) { line = e.common.id; });
    f.host.setSelection({line});
    f.host.run("BLOCK", {"Mark", "0,0", "D"});
    f.host.run("INSERT", {"Mark", "10,0"});
    EntityId ins{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Insert)
            ins = e.common.id;
    });
    f.host.setSelection({ins});
    f.host.run("ANNOTATIVE", {"Scales", "50"});
    f.host.setSelection({ins});
    f.host.run("OBJECTSCALE", {"Add", "100"});
    CHECK(f.doc.find(ins)->common.annoScales.get() == std::vector<double>{50, 100});
    f.host.setSelection({ins});
    f.host.run("OBJECTSCALE", {"Delete", "50"});
    CHECK(f.doc.find(ins)->common.annoScales.get() == std::vector<double>{100});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(100));
    f.host.setSelection({ins});
    f.host.run("OBJECTSCALE", {"Delete", "1:100"});
    CHECK(f.doc.find(ins)->common.annoScales.get() == std::vector<double>{100});
    CHECK(f.host.undo());
}

TEST_CASE("an annotative object is drawn at the current scale when it lists it") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "Hi"});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) { id = e.common.id; });
    const double h1 = textHeightOnScreen(f.doc, *f.doc.find(id));
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(id, {0, 0}), cmd::ScriptItem::enter(), "Scales", "50,100"});
    f.host.run("CANNOSCALE", {"50"});
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(id)) == doctest::Approx(h1 * 50).epsilon(0.01));
    f.host.run("CANNOSCALE", {"100"});
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(id)) == doctest::Approx(h1 * 100).epsilon(0.01));
    CHECK(f.doc.find(id)->common.annoScales.size() == 2);  // it already had 100
}

TEST_CASE("ANNOAUTOSCALE adds the new scale to annotative objects that list scales") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "A"});
    f.host.run("TEXT", {"0,10", "2.5", "0", "B"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    // A lists 50; B lists nothing of its own (follows the current scale).
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(ids[0], {0, 0}), cmd::ScriptItem::enter(), "Scales", "50"});
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(ids[1], {0, 10}), cmd::ScriptItem::enter(), "On"});
    CHECK(annoAutoScale(f.doc.header()));
    f.host.run("CANNOSCALE", {"100"});
    REQUIRE(f.doc.find(ids[0])->common.annoScales.size() == 2);
    CHECK(f.doc.find(ids[0])->common.annoScales.get() == std::vector<double>{50, 100});
    CHECK(f.doc.find(ids[1])->common.annoScales.empty());
    f.host.undo();  // one step takes the scale and the additions back
    CHECK(f.doc.find(ids[0])->common.annoScales.get() == std::vector<double>{50});

    f.host.run("ANNOAUTOSCALE", {"0"});
    CHECK_FALSE(annoAutoScale(f.doc.header()));
    f.host.run("CANNOSCALE", {"100"});
    CHECK(f.doc.find(ids[0])->common.annoScales.get() == std::vector<double>{50});
}

TEST_CASE("ANNOALLVISIBLE off hides annotative objects made for other scales") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "A"});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) { id = e.common.id; });
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(id, {0, 0}), cmd::ScriptItem::enter(), "Scales", "50"});
    f.host.run("ANNOAUTOSCALE", {"0"});
    f.host.run("CANNOSCALE", {"100"});
    CHECK(f.doc.effective(f.doc.find(id)->common).visible());  // shown by default
    f.host.run("ANNOALLVISIBLE", {"0"});
    CHECK_FALSE(f.doc.effective(f.doc.find(id)->common).visible());
    f.host.run("CANNOSCALE", {"50"});
    CHECK(f.doc.effective(f.doc.find(id)->common).visible());
    f.host.run("ANNOALLVISIBLE", {"1"});
    f.host.run("CANNOSCALE", {"100"});
    CHECK(f.doc.effective(f.doc.find(id)->common).visible());
}

TEST_CASE("a selected annotative object shows its other scales (SELECTIONANNODISPLAY)") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "A"});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) { id = e.common.id; });
    const double h1 = textHeightOnScreen(f.doc, *f.doc.find(id));
    f.host.run("ANNOTATIVE", {cmd::ScriptItem::pick(id, {0, 0}), cmd::ScriptItem::enter(), "Scales",
                              "20,50,100"});
    f.host.run("CANNOSCALE", {"50"});
    geom::DisplayContext ctx;
    const EntityId sel[] = {id};
    auto ghosts = geom::annotationGhosts(f.doc, sel, ctx);
    REQUIRE(ghosts.size() == 2);  // 20 and 100; 50 is the one drawn
    std::vector<double> scales{ghosts[0].scale, ghosts[1].scale};
    std::sort(scales.begin(), scales.end());
    CHECK(scales == std::vector<double>{20, 100});
    for (const auto& g : ghosts) {
        DisplayList d = g.display;
        double top = -1e30, bot = 1e30;
        for (const auto& item : d.items)
            if (const auto* fl = std::get_if<DisplayFill>(&item))
                for (const auto& r : fl->rings)
                    for (Vec2 v : r) {
                        top = std::max(top, v.y);
                        bot = std::min(bot, v.y);
                    }
        CHECK(top - bot == doctest::Approx(h1 * g.scale).epsilon(0.02));
    }
    f.host.run("SELECTIONANNODISPLAY", {"0"});
    CHECK(geom::annotationGhosts(f.doc, sel, ctx).empty());
}

TEST_CASE("ANNORESET makes the current scale the drawn one, ANNOUPDATE follows the text style") {
    Fixture f;
    f.host.run("LINE", {"0,0", "1,0", ""});
    EntityId line{};
    f.doc.forEachEntity([&](const Entity& e) { line = e.common.id; });
    f.host.setSelection({line});
    f.host.run("BLOCK", {"Mark", "0,0", "D"});
    f.host.run("INSERT", {"Mark", "10,0"});
    EntityId ins{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Insert)
            ins = e.common.id;
    });
    f.host.setSelection({ins});
    f.host.run("ANNOTATIVE", {"Scales", "50,100"});
    f.host.run("CANNOSCALE", {"100"});
    f.host.setSelection({ins});
    f.host.run("ANNORESET", {});
    CHECK(f.doc.find(ins)->common.annoScales.get() == std::vector<double>{100, 50});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(100));
    f.host.run("CANNOSCALE", {"20"});
    f.host.setSelection({ins});
    f.host.run("ANNORESET", {});
    CHECK(f.host.logContains("do not list that scale"));
    CHECK(f.doc.find(ins)->common.annoScales.get() == std::vector<double>{100, 50});
    CHECK(f.host.undo());

    f.host.run("TEXT", {"0,0", "2.5", "0", "Hi"});
    EntityId txt{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Text)
            txt = e.common.id;
    });
    CHECK_FALSE(f.doc.find(txt)->common.annotative);
    f.host.setSelection({txt});
    f.host.run("ANNOUPDATE", {});
    CHECK(f.host.logContains("0 object(s) updated"));
}

TEST_CASE("an annotation zone draws the annotative objects inside it at its own scale") {
    Fixture f;
    f.host.run("CANNOSCALE", {"100"});
    f.host.run("TEXT", {"0,0", "2.5", "0", "Plan"});
    f.host.run("TEXT", {"5000,0", "2.5", "0", "Detail"});
    f.host.run("TEXT", {"5000,100", "2.5", "0", "Plain"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 3);
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(ids[0], {0, 0}), cmd::ScriptItem::pick(ids[1], {5000, 0}),
                cmd::ScriptItem::enter(), "On"});
    const double plan = textHeightOnScreen(f.doc, *f.doc.find(ids[0]));
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[1])) == doctest::Approx(plan).epsilon(0.01));

    // A 1:20 zone around the detail: only the annotative object inside changes.
    f.host.run("ANNOZONE", {"4000,-1000", "7000,1000", "1:20", "Detail A"});
    auto zones = annotationZones(f.doc.header());
    REQUIRE(zones.size() == 1);
    CHECK(zones[0].name == "Detail A");
    CHECK(zones[0].scale == doctest::Approx(20));
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[0])) == doctest::Approx(plan).epsilon(0.01));
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[1])) ==
          doctest::Approx(plan / 5).epsilon(0.01));
    const double plain = textHeightOnScreen(f.doc, *f.doc.find(ids[2]));
    CHECK(plain == doctest::Approx(plan / 100).epsilon(0.01));
    // A layout viewport still draws at its own scale.
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[1]), 50) ==
          doctest::Approx(plan / 2).epsilon(0.01));

    // New annotative text placed in the zone is made for the zone's scale.
    f.host.run("ANNONEW", {"On"});
    f.host.run("TEXT", {"5000,200", "2.5", "0", "New"});
    EntityId last{};
    f.doc.forEachEntity([&](const Entity& e) { last = std::max(last, e.common.id); });
    REQUIRE(f.doc.find(last)->common.annotative);
    CHECK(f.doc.find(last)->common.annoScales.front() == doctest::Approx(20));

    // Change the scale by clicking inside the zone, rename by name, then delete.
    f.host.run("ANNOZONE", {"Scale", "5000,0", "10"});
    CHECK(annotationZones(f.doc.header())[0].scale == doctest::Approx(10));
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[1])) ==
          doctest::Approx(plan / 10).epsilon(0.01));
    f.host.run("ANNOZONE", {"Rename", "detail a", "Kitchen"});
    CHECK(annotationZones(f.doc.header())[0].name == "Kitchen");
    f.host.run("ANNOZONE", {"Frames", "Off"});
    CHECK_FALSE(annotationZonesShown(f.doc.header()));
    f.host.run("ANNOZONE", {"Delete", "5000,0"});
    CHECK(annotationZones(f.doc.header()).empty());
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[1])) == doctest::Approx(plan).epsilon(0.01));
    f.host.undo();
    CHECK(annotationZones(f.doc.header()).size() == 1);
}

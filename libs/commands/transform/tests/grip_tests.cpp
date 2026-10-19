#include <doctest/doctest.h>

#include <chrono>
#include <limits>

#include "common.h"
#include "tbcad/blocks/attributes.h"
#include "tbcad/geom/geom.h"
#include "tbcad/transform/grips.h"
#include "tbcad/transform/transform.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbtest;
using cmd::ScriptItem;
namespace tf = tbcad::transform;

TEST_CASE("grip collection follows geom::gripPoints and respects the limit") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto c = add(f.doc, CircleData{{0, 0}, 5});
    std::vector<EntityId> ids{a, c};
    auto grips = tf::collectGrips(f.doc, ids);
    CHECK(grips.size() == 3 + 5);
    CHECK(tf::collectGrips(f.doc, ids, 1).empty());
    const tf::Grip* g = tf::hitGrip(grips, {10.2, 0.1}, 1.0);
    REQUIRE(g != nullptr);
    CHECK(g->entity == a);
    CHECK(g->index == 1);
    CHECK(tf::hitGrip(grips, {50, 50}, 1.0) == nullptr);
}

TEST_CASE("GRIPEDIT stretches a line end - one undo step") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("GRIPEDIT", {"10,0", "20,5"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    CHECK(near(get<LineData>(f.doc, a).b, {20, 5}));
    CHECK(f.host.undo());
    CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("the shared end of two selected lines stretches together") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto b = add(f.doc, LineData{{10, 0}, {10, 10}});
    f.host.setSelection({a, b});
    f.host.run("GRIPEDIT", {"10,0", "12,-3"});
    CHECK(near(get<LineData>(f.doc, a).b, {12, -3}));
    CHECK(near(get<LineData>(f.doc, b).a, {12, -3}));
    CHECK(near(get<LineData>(f.doc, b).b, {10, 10}));
}

TEST_CASE("the midpoint grip of a line moves the whole line") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("GRIPEDIT", {"5,0", "5,5"});
    CHECK(near(get<LineData>(f.doc, a).a, {0, 5}));
    CHECK(near(get<LineData>(f.doc, a).b, {10, 5}));
}

TEST_CASE("a circle quadrant grip changes the radius, the centre grip moves it") {
    Fixture f;
    auto c = add(f.doc, CircleData{{0, 0}, 5});
    f.host.setSelection({c});
    f.host.run("GRIPEDIT", {"5,0", "8,0"});
    CHECK(get<CircleData>(f.doc, c).radius == doctest::Approx(8));
    CHECK(near(get<CircleData>(f.doc, c).center, {0, 0}));
    f.host.run("GRIPEDIT", {"0,0", "3,4"});
    CHECK(near(get<CircleData>(f.doc, c).center, {3, 4}));
}

TEST_CASE("polyline vertex grip and bulge midpoint grip") {
    Fixture f;
    PolylineData pl;
    pl.v = {{{0, 0}, 0}, {{10, 0}, 0}, {{10, 10}, 0}};
    auto id = add(f.doc, pl);
    f.host.setSelection({id});
    f.host.run("GRIPEDIT", {"10,0", "12,1"});
    CHECK(near(get<PolylineData>(f.doc, id).v[1].p, {12, 1}));
    // midpoint of segment 0 is now (6, 0.5); dragging a straight edge moves the edge (no arc)
    f.host.run("GRIPEDIT", {"6,0.5", "6,-3"});
    const auto& p = get<PolylineData>(f.doc, id);
    CHECK(p.v[0].bulge == doctest::Approx(0.0));
    CHECK(near(p.v[0].p, {0, -3.5}));
    CHECK(near(p.v[1].p, {12, -2.5}));
}

TEST_CASE("Enter cycles stretch -> move -> rotate and keywords pick a mode") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    // Enter once: Move. Drag the hot grip (10,0) to (10,10): the whole line moves by (0,10).
    f.host.run("GRIPEDIT", {"10,0", ScriptItem::enter(), "10,10"});
    CHECK(near(get<LineData>(f.doc, a).a, {0, 10}));
    CHECK(near(get<LineData>(f.doc, a).b, {10, 10}));
}

TEST_CASE("grip rotate, scale and mirror use the base point") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("GRIPEDIT", {"10,0", "Base", "0,0", "Rotate", "0,10"});
        CHECK(near(get<LineData>(f.doc, a).b, {0, 10}));
        CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("GRIPEDIT", {"10,0", "Base", "0,0", "Scale", "30,0"});
        CHECK(near(get<LineData>(f.doc, a).b, {30, 0}));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 1}, {10, 1}});
        f.host.setSelection({a});
        f.host.run("GRIPEDIT", {"10,1", "Base", "0,0", "Mirror", "10,0"});
        CHECK(near(get<LineData>(f.doc, a).a, {0, -1}));
        CHECK(near(get<LineData>(f.doc, a).b, {10, -1}));
    }
}

TEST_CASE("Copy mode keeps the original and repeats until Exit - all copies undo together") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("GRIPEDIT", {"5,0", "Move", "Copy", "5,10", "5,20", "Exit"});
    CHECK(r.finished);
    CHECK(f.doc.modelSpaceCount() == 3);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    CHECK(f.host.undo());
    CHECK(f.doc.modelSpaceCount() == 1);
}

TEST_CASE("Undo keyword inside a grip session steps back one drag") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("GRIPEDIT", {"5,0", "Move", "Copy", "5,10", "Undo", "Exit"});
    CHECK(f.doc.modelSpaceCount() == 1);
}

TEST_CASE("selections above grips.limit show no grips and GRIPEDIT refuses") {
    Fixture f;
    f.host.setSetting("grips.limit", "2");
    std::vector<EntityId> ids;
    for (int i = 0; i < 3; ++i)
        ids.push_back(add(f.doc, PointData{{double(i), 0}}));
    f.host.setSelection(ids);
    CHECK(tf::gripLimit(f.host) == 2);
    CHECK(tf::collectGrips(f.doc, ids, tf::gripLimit(f.host)).empty());
    f.host.run("GRIPEDIT", {"1,0", "5,5"});
    CHECK(f.host.logContains("Too many"));
    CHECK(near(get<PointData>(f.doc, ids[1]).p, {1, 0}));
}

TEST_CASE("a click that misses every grip changes nothing") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {100, 0}});
    f.host.setSelection({a});
    f.host.run("GRIPEDIT", {"50,50", "60,60"});
    CHECK(f.host.logContains("No grip"));
    CHECK(near(get<LineData>(f.doc, a).b, {100, 0}));
}

TEST_CASE("performance: MOVE of 100k selected lines is one undo step and fast") {
    Fixture f;
    std::vector<EntityId> ids;
    {
        auto tx = f.doc.begin("setup");
        for (int i = 0; i < 100000; ++i)
            ids.push_back(tx.add(Entity{{}, LineData{{double(i), 0}, {double(i), 1}}}).value());
        tx.commit();
    }
    f.host.setSelection(ids);
    auto t0 = std::chrono::steady_clock::now();
    auto r = f.host.run("MOVE", {"0,0", "10,10"});
    double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, ids[5]).a, {15, 10}));
    CHECK(f.host.undo());
    CHECK(near(get<LineData>(f.doc, ids[5]).a, {5, 0}));
    MESSAGE("MOVE 100k lines: " << ms << " ms");
    CHECK(ms < 5000);  // generous bound for debug/sanitizer CI; typical release is far lower
    // Grips are never built above the limit: no per-entity cost for a huge selection.
    auto g0 = std::chrono::steady_clock::now();
    CHECK(tf::collectGrips(f.doc, ids, tf::kDefaultGripLimit).empty());
    CHECK(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count() <
          5);
}

TEST_CASE("gripClickCommand names the grip under the click, and Copy for Ctrl") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10.5, 0}});
    std::vector<EntityId> ids{a};
    auto line = tf::gripClickCommand(f.doc, ids, {10.6, 0.1}, 1.0, 100, false);
    CHECK(line == "GRIPEDIT 10.5,0");
    CHECK(tf::gripClickCommand(f.doc, ids, {10.6, 0.1}, 1.0, 100, true) == "GRIPEDIT 10.5,0 Copy");
    CHECK(tf::gripClickCommand(f.doc, ids, {50, 50}, 1.0, 100, false).empty());
    // the produced line runs as typed in the command line
    f.host.setSelection(ids);
    f.host.run("GRIPEDIT", {"10.5,0", "20,0"});
    CHECK(near(get<LineData>(f.doc, a).b, {20, 0}));
}

TEST_CASE("the hot grip is set while GRIPEDIT waits and cleared afterwards") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("GRIPEDIT", {"10,0"});
    REQUIRE(tf::hotGrip().has_value());
    CHECK(near(*tf::hotGrip(), {10, 0}));
    f.host.feed({cmd::ScriptItem::esc()});
    CHECK_FALSE(tf::hotGrip().has_value());
}

TEST_CASE("a dynamic block insert offers the handle of its linear parameter") {
    Fixture f;
    BlockId block;
    {
        auto tx = f.doc.begin("block");
        block = tx.addBlock(BlockDef{.name = "Bar"}).value();
        Entity l;
        l.data = LineData{{0, 0}, {10, 0}};
        REQUIRE(tx.add(std::move(l), block).ok());
        DynamicDef def;
        DynParam w;
        w.name = "Length";
        w.base = {0, 0};
        w.end = {10, 0};
        def.params.push_back(w);
        DynAction stretch;
        stretch.kind = DynAction::Kind::Stretch;
        stretch.frameMin = {9, -1};
        stretch.frameMax = {11, 1};
        def.actions.push_back(stretch);
        REQUIRE(tx.modifyBlock(block, [&](BlockDef& d) { d.dynamic = std::move(def); }).ok());
        tx.commit();
    }
    // the action must name the line; set it now that the entity exists
    {
        auto tx = f.doc.begin("action");
        std::vector<EntityId> members = f.doc.block(block)->entities;
        REQUIRE(tx.modifyBlock(block, [&](BlockDef& d) {
                      d.dynamic->actions[0].objects = members;
                  }).ok());
        tx.commit();
    }
    auto ins = add(f.doc, InsertData{block, {50, 0}, {1, 1}, 0});
    std::vector<EntityId> ids{ins};
    auto grips = tf::collectGrips(f.doc, ids);
    REQUIRE(grips.size() == 2);  // the position and the Length handle
    CHECK(grips[1].index == tf::kDynGripBase);
    CHECK(near(grips[1].pos, {60, 0}));

    tf::Grip hot = grips[1];
    std::string error;
    auto moved = tf::stretchResults(f.doc, grips, hot, {5, 0}, &error);
    REQUIRE(moved.size() == 1);
    CHECK(entityAs<InsertData>(moved[0].second)->dynValues == std::vector<double>{15});
}

TEST_CASE("an insert offers a grip per attribute and dragging it moves just that value") {
    Fixture f;
    BlockId block;
    {
        auto tx = f.doc.begin("block");
        block = tx.addBlock(BlockDef{.name = "Tag"}).value();
        Entity t;
        TextData def;
        def.position = {0, 5};
        def.height = 2;
        def.utf8 = "NAME";
        def.attdef = AttDefInfo{"NAME", "", "x", false, false, false, false};
        t.data = def;
        REQUIRE(tx.add(std::move(t), block).ok());
        tx.commit();
    }
    InsertData d{block, {100, 0}, {1, 1}, 0};
    d.attribs = blocks::defaultAttribs(f.doc, block);
    auto ins = add(f.doc, d);
    std::vector<EntityId> ids{ins};
    auto grips = tf::collectGrips(f.doc, ids);
    REQUIRE(grips.size() == 2);  // the position and the attribute
    CHECK(grips[1].index == tf::kAttGripBase);
    CHECK(near(grips[1].pos, {100, 5}));

    f.host.setSelection(ids);
    f.host.run("GRIPEDIT", {"100,5", "110,12"});
    const auto& after = get<InsertData>(f.doc, ins);
    CHECK(near(after.position, {100, 0}));
    REQUIRE(after.attribs.size() == 1);
    CHECK(near(after.attribs[0].offset, {10, 7}));
    f.host.undo();
    CHECK(near(get<InsertData>(f.doc, ins).attribs[0].offset, {0, 0}));
}

TEST_CASE("GRIPOBJLIMIT sets the limit and 0 lifts it") {
    Fixture f;
    f.host.run("GRIPOBJLIMIT", {"2"});
    CHECK(tf::gripLimit(f.host) == 2);
    f.host.run("GRIPOBJLIMIT", {"0"});
    CHECK(tf::gripLimit(f.host) == std::numeric_limits<std::size_t>::max());
    std::vector<EntityId> ids;
    for (int i = 0; i < 300; ++i)
        ids.push_back(add(f.doc, PointData{{double(i), 0}}));
    CHECK(tf::collectGrips(f.doc, ids, tf::gripLimit(f.host)).size() == 300);
    f.host.run("GRIPOBJLIMIT", {"250"});
    CHECK(tf::collectGrips(f.doc, ids, tf::gripLimit(f.host)).empty());
}

TEST_CASE("cached grips follow the drawing, the selection and the limit") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto b = add(f.doc, LineData{{0, 5}, {10, 5}});
    std::vector<EntityId> one{a}, two{a, b};
    CHECK(tf::cachedGrips(f.doc, one, 100).size() == tf::collectGrips(f.doc, one, 100).size());
    CHECK(tf::cachedGrips(f.doc, two, 100).size() == tf::collectGrips(f.doc, two, 100).size());
    CHECK(tf::cachedGrips(f.doc, two, 1).empty());
    {
        Transaction tx = f.doc.begin("move");
        REQUIRE(tx.modify(a, [](Entity& e) { std::get<LineData>(e.data).b = {20, 0}; }));
        tx.commit();
    }
    bool found = false;
    for (const tf::Grip& g : tf::cachedGrips(f.doc, one, 100))
        found = found || near(g.pos, {20, 0});
    CHECK(found);
}

TEST_CASE("grips of a very big selection are found quickly") {
    Fixture f;
    std::vector<EntityId> ids;
    for (int i = 0; i < 50000; ++i)
        ids.push_back(add(f.doc, LineData{{double(i), 0}, {double(i) + 0.5, 1}}));
    const auto& grips = tf::cachedGrips(f.doc, ids, std::numeric_limits<std::size_t>::max());
    CHECK(grips.size() >= 100000);
    const tf::Grip* g = tf::hitGrip(grips, {25000.0, 0.0}, 0.2);
    REQUIRE(g);
    CHECK(near(g->pos, {25000.0, 0.0}));
}

TEST_CASE(
    "performance: the MOVE preview of 100k selected lines does not re-measure them per move") {
    Fixture f;
    std::vector<EntityId> ids;
    {
        auto tx = f.doc.begin("setup");
        for (int i = 0; i < 100000; ++i)
            ids.push_back(tx.add(Entity{{}, LineData{{double(i), 0}, {double(i), 1}}}).value());
        tx.commit();
    }
    f.host.setSelection(ids);
    f.host.run("MOVE", {"0,0"});
    // The first move measures the selection; the next ones reuse that.
    f.host.runner().pointerMoved(cmd::RawInput{{1, 1}, {1, 1}, 1});
    REQUIRE_FALSE(f.host.preview().items.empty());
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 2; i < 52; ++i)
        f.host.runner().pointerMoved(cmd::RawInput{{double(i), 1}, {double(i), 1}, 1});
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("50 preview moves over 100k lines: " << ms << " ms");
    CHECK(ms / 50 < 4);  // measured: 0.8 ms per move cached, 8 ms when every move measures
    f.host.feed({cmd::ScriptItem::esc()});
}

TEST_CASE("performance: a grip drag where 100k lines meet keeps the preview light") {
    Fixture f;
    std::vector<EntityId> ids;
    {
        auto tx = f.doc.begin("setup");
        for (int i = 0; i < 100000; ++i)
            ids.push_back(
                tx.add(Entity{{}, LineData{{0, 0}, {10 + double(i % 100), double(i / 100)}}})
                    .value());
        (void)tx.commit();
    }
    f.host.setSetting("grips.limit", "0");
    f.host.setSelection(ids);
    auto grips = tf::collectGrips(f.doc, ids, std::numeric_limits<std::size_t>::max());
    REQUIRE_FALSE(grips.empty());
    const tf::Grip* hot = tf::hitGrip(grips, {0, 0}, 0.5);
    REQUIRE(hot);
    auto t0 = std::chrono::steady_clock::now();
    const tf::HotGrips set = tf::hotGripsOf(grips, *hot);
    const double setMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(set.order.size() == 100000);
    t0 = std::chrono::steady_clock::now();
    auto some = tf::stretchResults(f.doc, set, *hot, {3, 4}, nullptr, 4000);
    const double dragMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(some.size() == 4000);
    MESSAGE("hot set of 100k: " << setMs << " ms, one preview of 4000: " << dragMs << " ms");
    CHECK(setMs < 2000);  // was quadratic: minutes
    // The real drag moves all of them, as one undo step.
    auto r = f.host.run("GRIPEDIT", {"0,0", "3,4"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, ids[99999]).a, {3, 4}));
    CHECK(f.host.undo());
    CHECK(near(get<LineData>(f.doc, ids[99999]).a, {0, 0}));
}

TEST_CASE("Shift-picked grips are dragged together by the same displacement") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto b = add(f.doc, LineData{{0, 5}, {10, 5}});
    auto c = add(f.doc, LineData{{0, 9}, {10, 9}});
    f.host.setSelection({a, b, c});
    tf::clearWarmGrips();
    tf::toggleWarmGrip({10, 0});
    tf::toggleWarmGrip({10, 5});
    auto r = f.host.run("GRIPEDIT", {"10,0", "13,4"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).b, {13, 4}));
    CHECK(near(get<LineData>(f.doc, b).b, {13, 9}));  // moved by the same (3, 4)
    CHECK(near(get<LineData>(f.doc, c).b, {10, 9}));  // not picked
    CHECK(tf::warmGrips().empty());                   // the drag ends the pick
    CHECK(f.host.undo());                             // one step for both
    CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
    CHECK(near(get<LineData>(f.doc, b).b, {10, 5}));
}

TEST_CASE("A grip that was not picked drags alone even when others are picked") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto b = add(f.doc, LineData{{0, 5}, {10, 5}});
    f.host.setSelection({a, b});
    tf::clearWarmGrips();
    tf::toggleWarmGrip({10, 5});
    f.host.run("GRIPEDIT", {"10,0", "12,2"});
    CHECK(near(get<LineData>(f.doc, a).b, {12, 2}));
    CHECK(near(get<LineData>(f.doc, b).b, {10, 5}));
    tf::clearWarmGrips();
}

TEST_CASE("Picking a picked grip again lets it go, two grips of one polyline move together") {
    tf::clearWarmGrips();
    tf::toggleWarmGrip({1, 1});
    tf::toggleWarmGrip({2, 2});
    tf::toggleWarmGrip({1, 1});
    REQUIRE(tf::warmGrips().size() == 1);
    CHECK(near(tf::warmGrips()[0], {2, 2}));
    tf::clearWarmGrips();
    Fixture f;
    PolylineData pl;
    pl.v = {{{0, 0}, 0}, {{10, 0}, 0}, {{10, 10}, 0}, {{0, 10}, 0}};
    pl.closed = true;
    auto id = add(f.doc, pl);
    f.host.setSelection({id});
    tf::toggleWarmGrip({10, 0});
    tf::toggleWarmGrip({10, 10});
    f.host.run("GRIPEDIT", {"10,0", "15,0"});
    const auto& v = get<PolylineData>(f.doc, id).v;
    CHECK(near(v[1].p, {15, 0}));
    CHECK(near(v[2].p, {15, 10}));
    CHECK(near(v[0].p, {0, 0}));
}

TEST_CASE(
    "Shift-click on a grip picks it without starting a drag, a plain click elsewhere forgets") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    ui::UiRegistry reg;
    tf::registerUi(reg);
    std::vector<EntityId> sel{a};
    std::vector<std::string> ran;
    ui::PanelContext ctx;
    ctx.doc = &f.doc;
    ctx.host = &f.host;
    ctx.selection = &sel;
    ctx.runCommand = [&](const std::string& l) { ran.push_back(l); };
    tf::clearWarmGrips();
    const auto& hook = reg.idleClickHooks().front();
    CHECK(hook(ctx, {10.1, 0.1}, 1.0, true, false));
    CHECK(ran.empty());
    REQUIRE(tf::warmGrips().size() == 1);
    CHECK(near(tf::warmGrips()[0], {10, 0}));
    CHECK(hook(ctx, {10.1, 0.1}, 1.0, false, false));  // plain click on it: the drag starts
    REQUIRE(ran.size() == 1);
    CHECK(ran[0].rfind("GRIPEDIT ", 0) == 0);
    CHECK_FALSE(hook(ctx, {50, 50}, 1.0, false, false));  // elsewhere: forget
    CHECK(tf::warmGrips().empty());
}

TEST_CASE("TEXT grips sit at the anchor and aligned text has both ends") {
    Fixture f;
    TextData t;
    t.position = {0, 0};
    t.alignPoint = {5, 1};
    t.utf8 = "abc";
    t.h = HAlign::Center;
    auto id = add(f.doc, Indirect<TextData>(t));
    auto g = tf::collectGrips(f.doc, std::vector<EntityId>{id});
    REQUIRE(g.size() == 1);
    CHECK(near(g[0].pos, {5, 1}));  // the justification point, where the text is drawn from
    auto moved = tbcad::geom::moveGrip(*f.doc.find(id), 0, {15, 11});
    REQUIRE(moved.ok());
    CHECK(near(entityAs<TextData>(moved.value())->alignPoint, {15, 11}));
    CHECK(near(entityAs<TextData>(moved.value())->position, {10, 10}));
    TextData a = t;
    a.h = HAlign::Aligned;
    auto aid = add(f.doc, Indirect<TextData>(a));
    auto ag = tf::collectGrips(f.doc, std::vector<EntityId>{aid});
    REQUIRE(ag.size() == 2);
    auto end = tbcad::geom::moveGrip(*f.doc.find(aid), 1, {20, 0});
    REQUIRE(end.ok());
    CHECK(near(entityAs<TextData>(end.value())->alignPoint, {20, 0}));
    CHECK(near(entityAs<TextData>(end.value())->position, {0, 0}));
    CHECK_FALSE(tbcad::geom::moveGrip(*f.doc.find(aid), 2, {1, 1}).ok());
}

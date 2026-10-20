#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

namespace {
bool drawsAsMask(const Document& doc, const Entity& e) {
    DisplayList d;
    geom::buildDisplay(doc, e, geom::DisplayContext{}, d);
    if (d.items.empty())
        return false;
    const auto* f = std::get_if<DisplayFill>(&d.items[0]);
    return f && isBackgroundColor(f->style.color);
}
}  // namespace

TEST_CASE("WIPEOUT from a rectangle makes a closed polyline that draws as a background mask") {
    Fixture f;
    REQUIRE(f.host.run("WIPEOUT", {"Rectangle", "0,0", "40,20"}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    CHECK(pl.closed);
    CHECK(pl.v.size() == 4);
    CHECK(es[0]->common.wipeout);
    CHECK(drawsAsMask(f.doc, *es[0]));
    f.host.undo();
    CHECK(f.entities().empty());
}

TEST_CASE("WIPEOUT from points closes with Enter and needs three") {
    Fixture f;
    REQUIRE(f.host.run("WO", {"0,0", "10,0", "10,10", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(as<PolylineData>(es[0]).v.size() == 3);
}

TEST_CASE("WIPEOUT Polyline and Remove switch an existing closed polyline") {
    Fixture f;
    PolylineData pl;
    pl.closed = true;
    pl.v = {{.p = {0, 0}}, {.p = {10, 0}}, {.p = {10, 10}}};
    EntityId id = f.addEntity(pl);
    f.host.setSelection({id});
    REQUIRE(f.host.run("WIPEOUT", {"Polyline", "Yes"}).finished);
    CHECK(f.doc.find(id)->common.wipeout);
    CHECK(f.entities().size() == 1);
    f.host.setSelection({id});
    REQUIRE(f.host.run("WIPEOUT", {"Remove"}).finished);
    CHECK_FALSE(f.doc.find(id)->common.wipeout);
    CHECK_FALSE(drawsAsMask(f.doc, *f.doc.find(id)));
}

TEST_CASE("WIPEOUTFRAME shows, hides or screen-only shows the outline, and is undoable") {
    Fixture f;
    REQUIRE(f.host.run("WIPEOUT", {"Rectangle", "0,0", "40,20"}).finished);
    const Entity& e = *f.entities()[0];
    auto items = [&] {
        DisplayList d;
        geom::buildDisplay(f.doc, e, geom::DisplayContext{}, d);
        return d;
    };
    // The default shows and plots the frame: the mask, then the outline above it.
    DisplayList d = items();
    REQUIRE(d.items.size() == 2);
    const auto* frame = std::get_if<DisplayPolyline>(&d.items[1]);
    REQUIRE(frame);
    CHECK(frame->closed);
    CHECK_FALSE(frame->style.noPlot);
    CHECK(frame->pts.size() == 4);

    REQUIRE(f.host.run("WIPEOUTFRAME", {"2"}).finished);
    d = items();
    REQUIRE(d.items.size() == 2);
    CHECK(std::get<DisplayPolyline>(d.items[1]).style.noPlot);

    REQUIRE(f.host.run("WIPEOUTFRAME", {"0"}).finished);
    CHECK(items().items.size() == 1);
    CHECK(drawsAsMask(f.doc, e));

    f.host.undo();  // back to 2
    CHECK(items().items.size() == 2);
    f.host.undo();  // back to the default
    CHECK(wipeoutFrameMode(f.doc.header()) == 1);
}

TEST_CASE("WIPEOUT points: Undo drops a point, repeated points count once, no area makes nothing") {
    Fixture f;
    REQUIRE(
        f.host.run("WO", {"0,0", "10,0", "10,0", "50,50", "Undo", "10,10", "0,0", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    const auto& pl = as<PolylineData>(es[0]);
    REQUIRE(pl.v.size() == 3);  // 0,0 10,0 10,10: the repeated and the closing point dropped
    CHECK(close(pl.v[2].p, {10, 10}));

    REQUIRE(f.host.run("WO", {"0,0", "10,0", "20,0", ""}).finished);
    CHECK(f.entities().size() == 1);
    CHECK(f.host.logContains("enclose an area"));
}

TEST_CASE("WIPEOUT Frames sets the frame mode, and Polyline leaves locked layers alone") {
    Fixture f;
    REQUIRE(f.host.run("WIPEOUT", {"Frames", "Off"}).finished);
    CHECK(wipeoutFrameMode(f.doc.header()) == 0);
    REQUIRE(f.host.run("WIPEOUT", {"Frames", "Display"}).finished);
    CHECK(wipeoutFrameMode(f.doc.header()) == 2);

    Transaction tx = f.doc.begin("layer");
    const LayerId locked = tx.addLayer(Layer{.name = "L", .locked = true}).value();
    tx.commit();
    PolylineData pl;
    pl.closed = true;
    pl.v = {{.p = {0, 0}}, {.p = {10, 0}}, {.p = {10, 10}}};
    EntityId id = f.addEntity(pl);
    Transaction tx2 = f.doc.begin("move");
    REQUIRE(tx2.modify(id, [&](Entity& e) { e.common.layer = locked; }));
    tx2.commit();
    f.host.setSelection({id});
    f.host.run("WIPEOUT", {"Polyline", ScriptItem::esc()});
    CHECK_FALSE(f.doc.find(id)->common.wipeout);
}

TEST_CASE("WIPEOUT: Enter takes a polyline and keeps it unless Erase polyline is Yes") {
    Fixture f;
    Transaction tx = f.doc.begin("layer");
    const LayerId walls = tx.addLayer(Layer{.name = "Walls"}).value();
    tx.commit();
    PolylineData pl;
    pl.closed = true;
    pl.v = {{.p = {0, 0}, .bulge = 0.5}, {.p = {10, 0}}, {.p = {10, 10}}};
    EntityId id = f.addEntity(pl);
    Transaction tx2 = f.doc.begin("move");
    REQUIRE(tx2.modify(id, [&](Entity& e) { e.common.layer = walls; }));
    tx2.commit();

    REQUIRE(f.host.run("WIPEOUT", {"", ScriptItem::pick(id, {5, 0}), "", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK_FALSE(es[0]->common.wipeout);  // the polyline stays as it was
    CHECK(es[1]->common.wipeout);
    CHECK(es[1]->common.layer == walls);
    CHECK(as<PolylineData>(es[1]).v.size() == 3);
    CHECK(as<PolylineData>(es[1]).v[0].bulge == 0.5);
    CHECK(f.host.logContains("1 wipeout made"));
    f.host.undo();
    CHECK(f.entities().size() == 1);
    // a wipeout is not offered again
    REQUIRE(f.host.run("WIPEOUT", {"P", ScriptItem::pick(id, {5, 0}), "", "Y"}).finished);
    CHECK(f.doc.find(id)->common.wipeout);
    f.host.setSelection({id});
    f.host.run("WIPEOUT", {"P"});
    CHECK(f.host.runner().active());  // the wipeout was filtered out of the selection
    f.host.feed({ScriptItem::esc()});
}

TEST_CASE("WIPEOUT: the first point starts the outline, and Undo of the first point ends it") {
    Fixture f;
    CHECK(f.host.run("WIPEOUT", {"0,0", "Undo"}).finished);
    CHECK(f.entities().empty());
    REQUIRE(f.host.run("WO", {"0,0", "20,0", "20,10", "0,10", ""}).finished);
    REQUIRE(f.entities().size() == 1);
    CHECK(as<PolylineData>(f.entities()[0]).v.size() == 4);
}

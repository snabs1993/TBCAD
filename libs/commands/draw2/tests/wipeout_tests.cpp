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
    REQUIRE(f.host.run("WO", {"Polygon", "0,0", "10,0", "10,10", ""}).finished);
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
    REQUIRE(f.host.run("WIPEOUT", {"Polyline"}).finished);
    CHECK(f.doc.find(id)->common.wipeout);
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

#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw::test;
using cmd::ScriptItem;

TEST_CASE("every draw command is registered with its alias") {
    Fixture f;
    for (auto [name, alias] : {std::pair{"LINE", "L"},
                               {"POLYLINE", "PL"},
                               {"CIRCLE", "C"},
                               {"ARC", "A"},
                               {"RECTANGLE", "REC"},
                               {"POLYGON", "POL"},
                               {"ELLIPSE", "EL"},
                               {"SPLINE", "SPL"}}) {
        CAPTURE(name);
        REQUIRE(f.reg.find(name) != nullptr);
        CHECK(f.reg.find(alias) == f.reg.find(name));
    }
}

TEST_CASE("LINE draws chained segments until Enter") {
    Fixture f;
    auto r = f.host.run("L", {"0,0", "@100,0", "@0,50", ""});
    CHECK(r.finished);
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(close(as<LineData>(es[0]).a, {0, 0}));
    CHECK(close(as<LineData>(es[0]).b, {100, 0}));
    CHECK(close(as<LineData>(es[1]).a, {100, 0}));
    CHECK(close(as<LineData>(es[1]).b, {100, 50}));
    CHECK(es[0]->common.layer == kLayerZero);
    CHECK(es[0]->common.color == Color::byLayer());
}

TEST_CASE("LINE Close joins the last point to the first once there are two segments") {
    Fixture f;
    auto r = f.host.run("LINE", {"0,0", "100,0", "100,50", "Close"});
    CHECK(r.finished);
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    CHECK(close(as<LineData>(es[2]).a, {100, 50}));
    CHECK(close(as<LineData>(es[2]).b, {0, 0}));
}

TEST_CASE("LINE does not offer Close after a single segment") {
    Fixture f;
    f.host.run("LINE", {"0,0", "100,0"});
    // "C" is not a keyword now, so the typed text is parsed as a point and rejected.
    auto r = f.host.feed({"C"});
    CHECK(r.rejected == 1);
    CHECK_FALSE(r.finished);
    f.host.feed({ScriptItem::esc()});
    CHECK(f.entities().size() == 1);
}

TEST_CASE("LINE Undo removes the last segment and continues from the previous point") {
    Fixture f;
    auto r = f.host.run("LINE", {"0,0", "100,0", "100,50", "Undo", "0,50", ""});
    CHECK(r.finished);
    auto es = f.entities();
    // The undone segment's entity is gone; the next one starts from (100,0).
    std::vector<LineData> lines;
    for (auto* e : es)
        lines.push_back(as<LineData>(e));
    REQUIRE(lines.size() == 2);
    CHECK(close(lines[0].b, {100, 0}));
    CHECK(close(lines[1].a, {100, 0}));
    CHECK(close(lines[1].b, {0, 50}));
}

TEST_CASE("LINE Undo several times walks back to the first point") {
    Fixture f;
    f.host.run("LINE", {"0,0", "10,0", "10,10", "0,10", "U", "U"});
    CHECK(f.entities().size() == 1);
    f.host.feed({"U"});
    CHECK(f.entities().empty());
    auto r = f.host.feed({"U"});  // nothing left to undo: message, command goes on
    CHECK_FALSE(r.finished);
    CHECK(f.host.logContains("Nothing to undo"));
    f.host.feed({"20,20", ""});
    auto es = f.entities();
    REQUIRE(es.size() == 1);
    CHECK(close(as<LineData>(es[0]).a, {0, 0}));
    CHECK(close(as<LineData>(es[0]).b, {20, 20}));
}

TEST_CASE("LINE: Esc keeps the finished segments, one undo step removes them all") {
    Fixture f;
    auto r = f.host.run("LINE", {"0,0", "10,0", "10,10", ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.entities().size() == 2);
    CHECK(f.host.undoStack().undoCount() == 1);
    CHECK(f.host.undo());
    CHECK(f.entities().empty());
    CHECK(f.host.redo());
    CHECK(f.entities().size() == 2);
}

TEST_CASE("LINE: Esc before the first segment draws nothing and records no undo step") {
    Fixture f;
    f.host.run("LINE", {"0,0", ScriptItem::esc()});
    CHECK(f.entities().empty());
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("LINE: Undo inside the command leaves exactly one undo step") {
    Fixture f;
    f.host.run("LINE", {"0,0", "10,0", "10,10", "U", ""});
    CHECK(f.entities().size() == 1);
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().empty());
}

TEST_CASE("LINE rubber-bands a preview to the cursor") {
    Fixture f;
    f.host.run("LINE", {"5,5"});
    cmd::RawInput in;
    in.worldCursor = {20, 30};
    in.screenCursor = {20, 30};
    f.host.runner().pointerMoved(in);
    REQUIRE(f.host.preview().items.size() == 1);
    const auto* pl = std::get_if<DisplayPolyline>(&f.host.preview().items[0]);
    REQUIRE(pl != nullptr);
    REQUIRE(pl->pts.size() == 2);
    CHECK(close(pl->pts[0], {5, 5}));
    CHECK(close(pl->pts[1], {20, 30}));
    f.host.feed({ScriptItem::esc()});
}

TEST_CASE("LINE accepts clicked points as well as typed ones") {
    Fixture f;
    f.host.run("LINE", {ScriptItem::click({1, 1}), ScriptItem::click({2, 2}), ""});
    CHECK(f.entities().size() == 1);
}

TEST_CASE("LINE: each segment is in the drawing while the command still waits for the next point") {
    Fixture f;
    auto r = f.host.run("LINE", {"0,0", "100,0"});
    CHECK_FALSE(r.finished);  // still asking for the next point
    CHECK(f.entities().size() == 1);
    f.host.feed({"100,50"});
    CHECK(f.entities().size() == 2);
}

TEST_CASE("LINE Enter at the first prompt continues from the end of the last line") {
    Fixture f;
    f.host.run("L", {"0,0", "10,0", ""});
    auto r = f.host.run("L", {"", "@0,5", ""});
    CHECK(r.finished);
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(close(as<LineData>(es[1]).a, {10, 0}));
    CHECK(close(as<LineData>(es[1]).b, {10, 5}));
}

TEST_CASE("LINE Enter after an arc asks for a tangent length") {
    Fixture f;
    f.host.run("ARC", {"10,0", "C", "0,0", "0,10"});  // ends at (0,10) heading west
    auto r = f.host.run("L", {"", "4", ""});
    CHECK(r.finished);
    auto es = f.entities();
    REQUIRE(es.size() == 2);
    CHECK(close(as<LineData>(es[1]).a, {0, 10}));
    CHECK(close(as<LineData>(es[1]).b, {-4, 10}));
}

TEST_CASE("LINE Enter with nothing to continue from draws nothing") {
    Fixture f;
    auto r = f.host.run("L", {""});
    CHECK(r.finished);
    CHECK(f.entities().empty());
    CHECK(f.host.logContains("no line or arc"));
}

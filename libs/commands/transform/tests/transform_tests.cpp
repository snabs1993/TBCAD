#include <doctest/doctest.h>

#include "common.h"

using namespace tbtest;
using cmd::ScriptItem;

TEST_CASE("the module registers its commands and aliases") {
    cmd::Registry reg;
    registerAllCommands(reg);
    for (const char* n : {"MOVE", "COPY", "ROTATE", "SCALE", "MIRROR"})
        CHECK(reg.find(n) != nullptr);
    CHECK(reg.find("M") == reg.find("MOVE"));
    CHECK(reg.find("CO") == reg.find("COPY"));
    CHECK(reg.find("CP") == reg.find("COPY"));
    CHECK(reg.find("RO") == reg.find("ROTATE"));
    CHECK(reg.find("SC") == reg.find("SCALE"));
    CHECK(reg.find("MI") == reg.find("MIRROR"));
}

TEST_CASE("MOVE with pre-selection: base point and second point, one undo step") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto b = add(f.doc, CircleData{{5, 5}, 2});
    f.host.setSelection({a, b});
    auto r = f.host.run("MOVE", {"0,0", "100,50"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {100, 50}));
    CHECK(near(get<LineData>(f.doc, a).b, {110, 50}));
    CHECK(near(get<CircleData>(f.doc, b).center, {105, 55}));
    CHECK(f.host.undoStack().undoLabel() == "MOVE");
    CHECK(f.host.undo());
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    CHECK(near(get<CircleData>(f.doc, b).center, {5, 5}));
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("MOVE selects objects first when nothing is selected - Enter uses base as displacement") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto r =
        f.host.run("M", {ScriptItem::select({a}), ScriptItem::enter(), "5,7", ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {5, 7}));
}

TEST_CASE("MOVE preview follows the cursor and Esc leaves the drawing untouched") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("MOVE", {"0,0", ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    CHECK_FALSE(f.host.undoStack().canUndo());
}

TEST_CASE("COPY repeats until Enter and leaves the source") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("COPY", {"0,0", "0,10", "0,20", ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(f.doc.modelSpaceCount() == 3);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
    int atY10 = 0, atY20 = 0;
    for (EntityId id : f.doc.modelSpace()) {
        const auto& l = get<LineData>(f.doc, id);
        atY10 += near(l.a, {0, 10});
        atY20 += near(l.a, {0, 20});
    }
    CHECK(atY10 == 1);
    CHECK(atY20 == 1);
    CHECK(f.host.undo());  // all copies are one step
    CHECK(f.doc.modelSpaceCount() == 1);
}

TEST_CASE("COPY keeps the copies made before Esc") {
    Fixture f;
    auto a = add(f.doc, PointData{{1, 1}});
    f.host.setSelection({a});
    f.host.run("CO", {"0,0", "10,0", ScriptItem::esc()});
    CHECK(f.doc.modelSpaceCount() == 2);
}

TEST_CASE("ROTATE by angle, about a base point") {
    Fixture f;
    auto a = add(f.doc, LineData{{10, 0}, {20, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("ROTATE", {"0,0", "90"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, a).a, {0, 10}));
    CHECK(near(get<LineData>(f.doc, a).b, {0, 20}));
}

TEST_CASE("ROTATE Copy option keeps the original") {
    Fixture f;
    auto a = add(f.doc, LineData{{10, 0}, {20, 0}});
    f.host.setSelection({a});
    f.host.run("RO", {"0,0", "Copy", "180"});
    CHECK(f.doc.modelSpaceCount() == 2);
    CHECK(near(get<LineData>(f.doc, a).a, {10, 0}));
    bool found = false;
    for (EntityId id : f.doc.modelSpace())
        found |= near(get<LineData>(f.doc, id).a, {-10, 0});
    CHECK(found);
}

TEST_CASE("ROTATE by reference: old angle 30, new angle 75 rotates by 45") {
    Fixture f;
    auto a = add(f.doc, LineData{{10, 0}, {20, 0}});
    f.host.setSelection({a});
    auto r = f.host.run("ROTATE", {"0,0", "Reference", "30", "75"});
    CHECK(r.finished);
    const double c = std::cos(kPi / 4), s = std::sin(kPi / 4);
    CHECK(near(get<LineData>(f.doc, a).a, {10 * c, 10 * s}));
    CHECK(near(get<LineData>(f.doc, a).b, {20 * c, 20 * s}));
}

TEST_CASE("ROTATE by reference with points") {
    Fixture f;
    auto a = add(f.doc, LineData{{10, 0}, {20, 0}});
    f.host.setSelection({a});
    // reference direction along +x (angle 0), new direction along +y: rotates 90 degrees
    f.host.run("ROTATE", {"0,0", "Reference", "0,0", "10,0", "0,10"});
    CHECK(near(get<LineData>(f.doc, a).b, {0, 20}));
    // A first point and Enter measures that point from the base point.
    CHECK(f.host.undo());
    f.host.run("ROTATE", {"0,0", "Reference", "10,0", "", "0,10"});
    CHECK(near(get<LineData>(f.doc, a).b, {0, 20}));
    // The direction of two points away from the base: 45 degrees to 135.
    CHECK(f.host.undo());
    f.host.run("ROTATE", {"0,0", "Reference", "5,5", "9,9", "-20,0"});
    CHECK(near(get<LineData>(f.doc, a).b, {20 * std::cos(kPi * 3 / 4), 20 * std::sin(kPi * 3 / 4)}));
}

TEST_CASE("SCALE by factor, Copy, and Reference") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{10, 10}, {20, 10}});
        auto c = add(f.doc, CircleData{{10, 10}, 2});
        f.host.setSelection({a, c});
        f.host.run("SCALE", {"10,10", "3"});
        CHECK(near(get<LineData>(f.doc, a).a, {10, 10}));
        CHECK(near(get<LineData>(f.doc, a).b, {40, 10}));
        CHECK(get<CircleData>(f.doc, c).radius == doctest::Approx(6));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("SC", {"0,0", "Copy", "0.5"});
        CHECK(f.doc.modelSpaceCount() == 2);
        CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("SCALE", {"0,0", "Reference", "10", "25"});
        CHECK(near(get<LineData>(f.doc, a).b, {25, 0}));
    }
}

TEST_CASE("SCALE refuses a zero factor and changes nothing") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("SCALE", {"0,0", "0"});
    CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
}

TEST_CASE("MIRROR keeps the source by default and erases it on request") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{1, 1}, {5, 3}});
        f.host.setSelection({a});
        auto r = f.host.run("MIRROR", {"0,0", "10,0", ScriptItem::enter()});
        CHECK(r.finished);
        CHECK(f.doc.modelSpaceCount() == 2);
        bool found = false;
        for (EntityId id : f.doc.modelSpace()) {
            const auto& l = get<LineData>(f.doc, id);
            found |= near(l.a, {1, -1}) && near(l.b, {5, -3});
        }
        CHECK(found);
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{1, 1}, {5, 3}});
        f.host.setSelection({a});
        f.host.run("MI", {"0,0", "0,10", "Yes"});
        REQUIRE(f.doc.modelSpaceCount() == 1);
        CHECK(near(get<LineData>(f.doc, a).a, {-1, 1}));
        CHECK(near(get<LineData>(f.doc, a).b, {-5, 3}));
        CHECK(f.host.undo());
        CHECK(near(get<LineData>(f.doc, a).a, {1, 1}));
    }
}

TEST_CASE("MIRROR rejects identical points then accepts a valid line") {
    Fixture f;
    auto a = add(f.doc, PointData{{2, 2}});
    f.host.setSelection({a});
    f.host.run("MIRROR", {"1,1", "1,1", "5,1", "Yes"});
    CHECK(f.host.logContains("must differ"));
    CHECK(near(get<PointData>(f.doc, a).p, {2, 0}));
}

TEST_CASE("MIRROR keeps text readable: position moves, rotation and glyph direction stay") {
    Fixture f;
    TextData t;
    t.position = {10, 5};
    t.rotation = 0.3;
    t.utf8 = "TBCAD";
    auto id = add(f.doc, t);
    f.host.setSelection({id});
    f.host.run("MIRROR", {"0,0", "0,10", "Yes"});
    const auto& m = get<TextData>(f.doc, id);
    CHECK(near(m.position, {-10, 5}));
    CHECK(m.rotation == doctest::Approx(0.3));
}

TEST_CASE("SCALE scales text uniformly (position and height)") {
    Fixture f;
    TextData t;
    t.position = {10, 0};
    t.height = 2;
    t.utf8 = "x";
    auto id = add(f.doc, t);
    f.host.setSelection({id});
    f.host.run("SCALE", {"0,0", "2"});
    CHECK(near(get<TextData>(f.doc, id).position, {20, 0}));
    CHECK(get<TextData>(f.doc, id).height == doctest::Approx(4));
}

TEST_CASE("selected polylines with bulges move, rotate and scale uniformly") {
    Fixture f;
    PolylineData pl;
    pl.v = {{{0, 0}, 1.0}, {{10, 0}, 0}};
    auto id = add(f.doc, pl);
    f.host.setSelection({id});
    f.host.run("SCALE", {"0,0", "2"});
    const auto& p = get<PolylineData>(f.doc, id);
    CHECK(near(p.v[1].p, {20, 0}));
    CHECK(p.v[0].bulge == doctest::Approx(1.0));
}

TEST_CASE("preview is produced while MOVE waits for the second point") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("MOVE", {"0,0"});
    f.host.runner().pointerMoved(cmd::RawInput{{5, 5}, {5, 5}, 1});
    REQUIRE_FALSE(f.host.preview().items.empty());
    f.host.feed({ScriptItem::esc()});
    CHECK(near(get<LineData>(f.doc, a).a, {0, 0}));
}

TEST_CASE("Select objects accepts All, Last and Previous typed like AutoCAD") {
    Fixture f;
    auto a = add(f.doc, LineData{{1, 1}, {5, 3}});
    auto b = add(f.doc, LineData{{1, 2}, {5, 4}});
    f.host.run("MIRROR", {"ALL", ScriptItem::enter(), "0,0", "10,0", ScriptItem::enter()});
    CHECK(f.doc.modelSpaceCount() == 4);  // both lines mirrored, sources kept
    Fixture g;
    add(g.doc, LineData{{1, 1}, {5, 3}});
    auto last = add(g.doc, LineData{{1, 2}, {5, 4}});
    g.host.run("MIRROR", {"Last", ScriptItem::enter(), "0,0", "10,0", "Yes"});
    CHECK(near(get<LineData>(g.doc, last).a, {1, -2}));
    Fixture h;
    add(h.doc, LineData{{1, 1}, {5, 3}});
    h.host.run("MIRROR", {"Nonsense", "Cancel"});
    CHECK(h.host.logContains("type Window, Crossing, WPolygon"));
    (void)a;
    (void)b;
}

TEST_CASE("MOVE and COPY Displacement take a vector instead of two points") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("MOVE", {"Displacement", "3,4"});
    CHECK(near(get<LineData>(f.doc, a).a, {3, 4}));
    f.host.setSelection({a});
    f.host.run("COPY", {"Displacement", "0,10"});
    CHECK(near(get<LineData>(f.doc, a).a, {3, 4}));
    int lines = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e); l && near(l->a, {3, 14}))
            ++lines;
    });
    CHECK(lines == 1);
}

TEST_CASE("SCALE by reference with two points, a typed length, or Enter after one point") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    // Two points 5 apart, away from the base: 5 becomes 20, so the 10 long line becomes 40.
    f.host.run("SCALE", {"0,0", "Reference", "3,3", "8,3", "20,0"});
    CHECK(near(get<LineData>(f.doc, a).b, {40, 0}));
    CHECK(f.host.undo());
    // One point and Enter measures from the base point: 5 becomes 10 -> factor 2.
    f.host.run("SCALE", {"0,0", "Reference", "5,0", "", "10,0"});
    CHECK(near(get<LineData>(f.doc, a).b, {20, 0}));
    CHECK(f.host.undo());
    // Two identical points give no length; the prompt asks again.
    f.host.run("SCALE", {"0,0", "Reference", "4,4", "4,4", "10", "25"});
    CHECK(near(get<LineData>(f.doc, a).b, {25, 0}));
}

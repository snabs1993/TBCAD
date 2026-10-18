#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "common.h"
#include "tbcad/geom/geom.h"
#include "tbcad/transform/grip_menu.h"

using namespace tbtest;
using cmd::ScriptItem;
namespace tf = tbcad::transform;
using tf::GripAction;

namespace {

bool offers(const Document& doc, EntityId id, int index, GripAction a) {
    auto grips = tf::collectGrips(doc, std::vector<EntityId>{id});
    for (const auto& g : grips)
        if (g.index == index) {
            auto acts = tf::gripActions(doc, g);
            return std::find(acts.begin(), acts.end(), a) != acts.end();
        }
    return false;
}

PolylineData square() {
    PolylineData pl;
    pl.closed = true;
    pl.v = {{{0, 0}, 0}, {{10, 0}, 0}, {{10, 10}, 0}, {{0, 10}, 0}};
    return pl;
}

}  // namespace

TEST_CASE("each kind of grip offers its own actions") {
    Fixture f;
    auto line = add(f.doc, LineData{{0, 0}, {10, 0}});
    auto circle = add(f.doc, CircleData{{0, 0}, 5});
    auto arc = add(f.doc, ArcData{{0, 0}, 5, 0, kPi});
    auto pl = add(f.doc, square());
    CHECK(offers(f.doc, line, 1, GripAction::Lengthen));
    CHECK_FALSE(offers(f.doc, line, 2, GripAction::Lengthen));
    CHECK(offers(f.doc, circle, 1, GripAction::Radius));
    CHECK_FALSE(offers(f.doc, circle, 0, GripAction::Radius));
    CHECK(offers(f.doc, arc, 2, GripAction::Radius));
    CHECK(offers(f.doc, arc, 0, GripAction::Lengthen));
    CHECK(offers(f.doc, pl, 0, GripAction::AddVertex));
    CHECK(offers(f.doc, pl, 0, GripAction::RemoveVertex));
    CHECK(offers(f.doc, pl, 4, GripAction::AddVertex));
    CHECK(offers(f.doc, pl, 4, GripAction::ConvertToArc));
    CHECK_FALSE(offers(f.doc, pl, 4, GripAction::ConvertToLine));
    CHECK_FALSE(offers(f.doc, pl, 4, GripAction::RemoveVertex));
    // Every grip starts with Stretch.
    for (EntityId id : {line, circle, arc, pl})
        for (const auto& g : tf::collectGrips(f.doc, std::vector<EntityId>{id}))
            CHECK(tf::gripActions(f.doc, g).front() == GripAction::Stretch);
}

TEST_CASE("polyline: add vertex on a vertex and on a segment, remove vertex") {
    Fixture f;
    auto id = add(f.doc, square());
    f.host.setSelection({id});
    // Segment grip (midpoint of the bottom edge) -> a new vertex where the user clicks.
    auto r = f.host.run("GRIPEDIT", {"5,0", "AddVertex", "5,-3"});
    CHECK(r.finished);
    const auto& v = get<PolylineData>(f.doc, id).v;
    REQUIRE(v.size() == 5);
    CHECK(near(v[1].p, {5, -3}));
    CHECK(near(v[2].p, {10, 0}));
    CHECK(f.host.undo());
    CHECK(get<PolylineData>(f.doc, id).v.size() == 4);

    // Vertex grip -> removes the vertex at once.
    r = f.host.run("GRIPEDIT", {"10,10", "RemoveVertex"});
    CHECK(r.finished);
    const auto& w = get<PolylineData>(f.doc, id).v;
    REQUIRE(w.size() == 3);
    CHECK(near(w[2].p, {0, 10}));
    CHECK(f.host.undo());
    CHECK(get<PolylineData>(f.doc, id).v.size() == 4);
}

TEST_CASE("polyline: cannot remove below the minimum, arcs split into two arcs") {
    Fixture f;
    PolylineData tri;
    tri.closed = true;
    tri.v = {{{0, 0}, 0}, {{10, 0}, 0}, {{5, 8}, 0}};
    auto t = add(f.doc, tri);
    CHECK_FALSE(offers(f.doc, t, 0, GripAction::RemoveVertex));

    PolylineData half;
    half.v = {{{0, 0}, 1.0}, {{10, 0}, 0}};  // a half circle, centre (5,0)
    auto h = add(f.doc, half);
    f.host.setSelection({h});
    auto r = f.host.run("GRIPEDIT", {"5,-5", "AddVertex", "5,-9"});
    CHECK(r.finished);
    const auto& v = get<PolylineData>(f.doc, h).v;
    REQUIRE(v.size() == 3);
    CHECK(near(v[1].p, {5, -5}, 1e-9));  // snapped onto the arc
    CHECK(v[0].bulge == doctest::Approx(std::tan(kPi / 8)));
    CHECK(v[1].bulge == doctest::Approx(std::tan(kPi / 8)));
}

TEST_CASE("polyline: convert a segment to an arc and back") {
    Fixture f;
    auto id = add(f.doc, square());
    f.host.setSelection({id});
    auto r = f.host.run("GRIPEDIT", {"5,0", "ConvertToArc", "5,-2"});
    CHECK(r.finished);
    CHECK(get<PolylineData>(f.doc, id).v[0].bulge != 0);
    CHECK(offers(f.doc, id, 4, GripAction::ConvertToLine));
    r = f.host.run("GRIPEDIT", {"5,-2", "ConvertToLine"});
    CHECK(r.finished);
    CHECK(get<PolylineData>(f.doc, id).v[0].bulge == 0);
}

TEST_CASE("line end: Lengthen slides along the line, ignoring the sideways part of the click") {
    Fixture f;
    auto id = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({id});
    auto r = f.host.run("GRIPEDIT", {"10,0", "Lengthen", "25,7"});
    CHECK(r.finished);
    CHECK(near(get<LineData>(f.doc, id).b, {25, 0}));
    CHECK(near(get<LineData>(f.doc, id).a, {0, 0}));
    // Past the other end: refused, nothing changes.
    r = f.host.run("GRIPEDIT", {"25,0", "Lengthen", "-5,0"});
    CHECK(near(get<LineData>(f.doc, id).b, {25, 0}));
}

TEST_CASE("arc: Radius at the middle grip, Lengthen at an end grip") {
    Fixture f;
    auto id = add(f.doc, ArcData{{0, 0}, 10, 0, kPi / 2});
    f.host.setSelection({id});
    // Middle grip is at angle 45 degrees.
    const double m = 10 * std::cos(kPi / 4);
    auto r =
        f.host.run("GRIPEDIT", {std::to_string(m) + "," + std::to_string(m), "Radius", "20,0"});
    CHECK(r.finished);
    CHECK(get<ArcData>(f.doc, id).radius == doctest::Approx(20));
    CHECK(get<ArcData>(f.doc, id).center.x == doctest::Approx(0));
    r = f.host.run("GRIPEDIT", {"0,20", "Lengthen", "-30,0"});
    CHECK(r.finished);
    CHECK(get<ArcData>(f.doc, id).endAngle == doctest::Approx(kPi));
    CHECK(get<ArcData>(f.doc, id).radius == doctest::Approx(20));
}

TEST_CASE("circle quadrant: Radius") {
    Fixture f;
    auto id = add(f.doc, CircleData{{0, 0}, 5});
    f.host.setSelection({id});
    auto r = f.host.run("GRIPEDIT", {"0,5", "Radius", "0,12"});
    CHECK(r.finished);
    CHECK(get<CircleData>(f.doc, id).radius == doctest::Approx(12));
}

TEST_CASE("dimension: reset text position, continue and baseline commands") {
    Fixture f;
    f.host.run("DIMLINEAR", {"0,0", "10,0", "5,5"});
    EntityId dimId{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dimId = e.common.id;
    });
    REQUIRE(dimId);
    f.host.setSelection({dimId});
    auto grips = tf::collectGrips(f.doc, std::vector<EntityId>{dimId});
    const tf::Grip* textGrip = nullptr;
    const tf::Grip* originGrip = nullptr;
    for (const auto& g : grips) {
        if (g.index == 4)
            textGrip = &g;
        if (g.index == 0)
            originGrip = &g;
    }
    REQUIRE(textGrip);
    REQUIRE(originGrip);
    // The text sits at its automatic place: nothing to reset until it is moved by hand.
    CHECK_FALSE(offers(f.doc, dimId, 4, GripAction::ResetText));
    f.host.run("GRIPEDIT",
               {ScriptItem(std::to_string(textGrip->pos.x) + "," + std::to_string(textGrip->pos.y)),
                "5,9"});
    CHECK(get<DimensionData>(f.doc, dimId).textPosUser);
    CHECK(offers(f.doc, dimId, 4, GripAction::ResetText));
    const auto moved = tf::collectGrips(f.doc, std::vector<EntityId>{dimId});
    f.host.run("GRIPEDIT",
               {ScriptItem(std::to_string(moved[4].pos.x) + "," + std::to_string(moved[4].pos.y)),
                "ResetText"});
    CHECK_FALSE(get<DimensionData>(f.doc, dimId).textPosUser);

    // Continue and Baseline start the chain commands from this dimension.
    const std::string cont = tf::gripActionCommand(f.doc, *originGrip, GripAction::Continue);
    const std::string base = tf::gripActionCommand(f.doc, *originGrip, GripAction::Baseline);
    CHECK(cont.rfind("DIMCONTINUE Select ", 0) == 0);
    CHECK(base.rfind("DIMBASELINE Select ", 0) == 0);
    CHECK(tf::gripActionCommand(f.doc, *textGrip, GripAction::Continue).empty());
}

TEST_CASE("action keywords and commands round trip") {
    Fixture f;
    auto id = add(f.doc, square());
    for (GripAction a :
         {GripAction::Stretch, GripAction::AddVertex, GripAction::RemoveVertex,
          GripAction::ConvertToArc, GripAction::ConvertToLine, GripAction::Lengthen,
          GripAction::Radius, GripAction::ResetText, GripAction::Continue, GripAction::Baseline}) {
        GripAction back{};
        REQUIRE(tf::gripActionFromKeyword(tf::gripActionKeyword(a), back));
        CHECK(back == a);
        CHECK(std::string(tf::gripActionLabel(a)).size() > 3);
    }
    GripAction x{};
    CHECK_FALSE(tf::gripActionFromKeyword("Nonsense", x));
    auto grips = tf::collectGrips(f.doc, std::vector<EntityId>{id});
    const std::string line = tf::gripActionCommand(f.doc, grips[0], GripAction::AddVertex);
    CHECK(line.rfind("GRIPEDIT 0,0 AddVertex", 0) == 0);
    CHECK(tf::gripActionCommand(f.doc, grips[0], GripAction::Radius).empty());
}

TEST_CASE("every action on every grip of every sample kind keeps the drawing valid") {
    Fixture f;
    std::vector<EntityId> ids{add(f.doc, LineData{{0, 0}, {10, 3}}),
                              add(f.doc, CircleData{{3, 3}, 4}),
                              add(f.doc, ArcData{{0, 0}, 6, 0.5, 2.5}), add(f.doc, square())};
    PolylineData arcs;
    arcs.v = {{{0, 0}, 0.5}, {{10, 0}, -0.3}, {{10, 10}, 0}};
    ids.push_back(add(f.doc, arcs));
    const Vec2 targets[] = {{4, 4}, {0, 0}, {1e6, -1e6}, {-7, 2}};
    for (EntityId id : ids)
        for (const auto& g : tf::collectGrips(f.doc, std::vector<EntityId>{id}))
            for (GripAction a : tf::gripActions(f.doc, g))
                for (Vec2 t : targets) {
                    const Entity* e = f.doc.find(id);
                    auto res = tf::applyGripAction(*e, g.index, a, t);
                    if (!res)
                        continue;
                    // The result must have finite grips.
                    for (Vec2 p : geom::gripPoints(res.value())) {
                        CHECK(std::isfinite(p.x));
                        CHECK(std::isfinite(p.y));
                    }
                }
}

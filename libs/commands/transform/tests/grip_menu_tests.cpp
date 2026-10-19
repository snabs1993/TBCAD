#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "common.h"
#include "tbcad/geom/geom.h"
#include "tbcad/transform/grip_menu.h"
#include "tbcad/transform/transform.h"
#include "tbcad/ui/ui_registry.h"

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
    SplineData fitted;
    fitted.fit = {{0, 0}, {5, 4}, {10, 0}};
    if (auto sr = geom::interpolateSpline(fitted.fit, {})) {
        fitted.degree = sr.value().degree;
        fitted.knots = sr.value().knots;
        fitted.control = sr.value().control;
        ids.push_back(add(f.doc, fitted));
    }
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

TEST_CASE("resting the cursor on a polyline vertex grip offers a menu, empty space does not") {
    Fixture f;
    auto pl = add(f.doc, square());
    f.host.setSelection({pl});
    ui::UiRegistry reg;
    tf::registerUi(reg);
    REQUIRE_FALSE(reg.hoverMenus().empty());
    std::vector<EntityId> sel{pl};
    ui::PanelContext ctx;
    ctx.doc = &f.doc;
    ctx.host = &f.host;
    ctx.selection = &sel;
    ctx.apertureMm = 1;
    ctx.cursorWorld = {10, 10};
    ui::HoverMenu menu;
    REQUIRE(reg.hoverMenus().front()(ctx, menu));
    CHECK(menu.items.size() >= 2);
    CHECK(menu.items.front().label == "Stretch");
    CHECK(menu.items.front().command.rfind("GRIPEDIT ", 0) == 0);
    CHECK(menu.anchorWorld.x == doctest::Approx(10));
    ctx.cursorWorld = {5, 5};
    ui::HoverMenu none;
    CHECK_FALSE(reg.hoverMenus().front()(ctx, none));
    ctx.cursorWorld = {10, 10};
    ctx.commandActive = true;
    CHECK_FALSE(reg.hoverMenus().front()(ctx, none));
}

namespace {

HatchData squareHatch() {
    HatchLoop loop;
    loop.edges = {HatchLineEdge{{0, 0}, {10, 0}}, HatchLineEdge{{10, 0}, {10, 10}},
                  HatchLineEdge{{10, 10}, {0, 10}}, HatchLineEdge{{0, 10}, {0, 0}}};
    HatchData h;
    h.loops.push_back(loop);
    return h;
}

}  // namespace

TEST_CASE("hatch: one grip per boundary corner, a corner moves the edges that meet there") {
    Fixture f;
    auto id = add(f.doc, squareHatch());
    const Entity* e = f.doc.find(id);
    REQUIRE(geom::gripPoints(*e).size() == 4);
    auto moved = geom::moveGrip(*e, 2, {12, 13});
    REQUIRE(moved);
    const auto& loop = entityAs<HatchData>(moved.value())->loops[0];
    CHECK(std::get<HatchLineEdge>(loop.edges[1]).b.x == doctest::Approx(12));
    CHECK(std::get<HatchLineEdge>(loop.edges[2]).a.y == doctest::Approx(13));
    CHECK(std::get<HatchLineEdge>(loop.edges[0]).b.x == doctest::Approx(10));
    CHECK_FALSE(geom::moveGrip(*e, 9, {0, 0}));
}

TEST_CASE("hatch: stretching the end of an arc edge keeps the arc through its old middle") {
    for (bool ccw : {true, false}) {
        Fixture f;
        HatchArcEdge arc{{0, 0}, 5, 0, kPi, true};
        if (!ccw)
            arc = HatchArcEdge{{0, 0}, 5, kPi, 0, false};
        HatchLoop loop;
        loop.edges = {arc, HatchLineEdge{ccw ? Vec2{-5, 0} : Vec2{5, 0}, ccw ? Vec2{5, 0} : Vec2{-5, 0}}};
        HatchData h;
        h.loops.push_back(loop);
        auto id = add(f.doc, h);
        const Entity* e = f.doc.find(id);
        auto grips = geom::gripPoints(*e);
        REQUIRE(grips.size() == 2);
        // Move the corner at (5, 0) up: the line follows and the arc still bulges to y > 0.
        int idx = std::abs(grips[0].x - 5) < 1e-9 ? 0 : 1;
        auto moved = geom::moveGrip(*e, idx, {5, 2});
        REQUIRE(moved);
        const auto& out = entityAs<HatchData>(moved.value())->loops[0];
        const auto& a = std::get<HatchArcEdge>(out.edges[0]);
        const double s = a.center.y + std::sin(a.startAngle) * a.radius;
        const double t = a.center.y + std::sin(a.endAngle) * a.radius;
        CHECK((std::abs(s - 2) < 1e-6 || std::abs(t - 2) < 1e-6));
        CHECK(a.radius > 0);
        for (Vec2 p : geom::gripPoints(moved.value())) {
            CHECK(std::isfinite(p.x));
            CHECK(std::isfinite(p.y));
        }
    }
}

TEST_CASE("hatch: polyline edge vertices have grips that only stretch, so no menu") {
    Fixture f;
    HatchLoop loop;
    loop.edges = {HatchPolylineEdge{square().v, true}};
    HatchData h;
    h.loops.push_back(loop);
    auto id = add(f.doc, h);
    f.host.setSelection({id});
    REQUIRE(geom::gripPoints(*f.doc.find(id)).size() == 4);
    ui::UiRegistry reg;
    tf::registerUi(reg);
    std::vector<EntityId> sel{id};
    ui::PanelContext ctx;
    ctx.doc = &f.doc;
    ctx.host = &f.host;
    ctx.selection = &sel;
    ctx.apertureMm = 1;
    ctx.cursorWorld = {10, 10};
    ui::HoverMenu menu;
    CHECK_FALSE(reg.hoverMenus().front()(ctx, menu));
    for (const auto& g : tf::collectGrips(f.doc, std::vector<EntityId>{id}))
        CHECK(tf::gripActions(f.doc, g) == std::vector<GripAction>{GripAction::Stretch});
    auto moved = geom::moveGrip(*f.doc.find(id), 2, {11, 12});
    REQUIRE(moved);
    const auto& pe = std::get<HatchPolylineEdge>(entityAs<HatchData>(moved.value())->loops[0].edges[0]);
    CHECK(pe.v[2].p.y == doctest::Approx(12));
}

TEST_CASE("hatch: GRIPEDIT stretches a corner, one undo step, redo") {
    Fixture f;
    auto id = add(f.doc, squareHatch());
    f.host.setSelection({id});
    auto r = f.host.run("GRIPEDIT", {"10,10", "14,15"});
    CHECK(r.finished);
    auto corner = [&] {
        return std::get<HatchLineEdge>(get<HatchData>(f.doc, id).loops[0].edges[1]).b;
    };
    CHECK(near(corner(), {14, 15}));
    CHECK(f.host.undo());
    CHECK(near(corner(), {10, 10}));
    CHECK(f.host.redo());
    CHECK(near(corner(), {14, 15}));
}

TEST_CASE("spline with fit points: add and remove a fit point, one undo step each") {
    Fixture f;
    SplineData sp;
    sp.fit = {{0, 0}, {10, 8}, {20, 0}, {30, 8}};
    auto r0 = geom::interpolateSpline(sp.fit, {});
    REQUIRE(r0);
    sp.knots = r0.value().knots;
    sp.control = r0.value().control;
    auto id = add(f.doc, sp);
    f.host.setSelection({id});
    CHECK(offers(f.doc, id, 1, GripAction::AddVertex));
    CHECK(offers(f.doc, id, 1, GripAction::RemoveVertex));
    auto r = f.host.run("GRIPEDIT", {"10,8", "AddVertex", "15,9"});
    CHECK(r.finished);
    {
        const auto& d = get<SplineData>(f.doc, id);
        REQUIRE(d.fit.size() == 5);
        CHECK(near(d.fit[2], {15, 9}));
        CHECK(near(d.fit[3], {20, 0}));
        CHECK(d.control.size() >= 5);
    }
    CHECK(f.host.undo());
    CHECK(get<SplineData>(f.doc, id).fit.size() == 4);
    r = f.host.run("GRIPEDIT", {"10,8", "RemoveVertex"});
    CHECK(r.finished);
    CHECK(get<SplineData>(f.doc, id).fit.size() == 3);
    // Two fit points are the least a spline can keep.
    CHECK(offers(f.doc, id, 1, GripAction::RemoveVertex));
    r = f.host.run("GRIPEDIT", {"20,0", "RemoveVertex"});
    CHECK(r.finished);
    CHECK(get<SplineData>(f.doc, id).fit.size() == 2);
    CHECK_FALSE(offers(f.doc, id, 1, GripAction::RemoveVertex));
    for (int i = 0; i < 2; ++i)
        CHECK(f.host.undo());
    CHECK(get<SplineData>(f.doc, id).fit.size() == 4);
}

TEST_CASE("spline without fit points offers no vertex actions") {
    Fixture f;
    SplineData sp;
    sp.control = {{0, 0}, {5, 8}, {10, 0}, {15, 8}};
    sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    auto id = add(f.doc, sp);
    CHECK_FALSE(offers(f.doc, id, 1, GripAction::AddVertex));
}

TEST_CASE("leader: add a vertex after the grip, remove one, never below two") {
    Fixture f;
    LeaderData ld;
    ld.vertices = {{0, 0}, {10, 10}, {20, 10}};
    ld.textPos = {22, 10};
    auto id = add(f.doc, ld);
    f.host.setSelection({id});
    CHECK(offers(f.doc, id, 1, GripAction::AddVertex));
    CHECK(offers(f.doc, id, 1, GripAction::RemoveVertex));
    CHECK_FALSE(offers(f.doc, id, 3, GripAction::AddVertex));  // the text grip
    auto r = f.host.run("GRIPEDIT", {"0,0", "AddVertex", "5,2"});
    CHECK(r.finished);
    {
        const auto& v = get<LeaderData>(f.doc, id).vertices;
        REQUIRE(v.size() == 4);
        CHECK(near(v[1], {5, 2}));
        CHECK(near(v[2], {10, 10}));
    }
    CHECK(f.host.undo());
    CHECK(get<LeaderData>(f.doc, id).vertices.size() == 3);
    r = f.host.run("GRIPEDIT", {"10,10", "RemoveVertex"});
    CHECK(r.finished);
    REQUIRE(get<LeaderData>(f.doc, id).vertices.size() == 2);
    CHECK_FALSE(offers(f.doc, id, 1, GripAction::RemoveVertex));
    CHECK(f.host.undo());
    CHECK(get<LeaderData>(f.doc, id).vertices.size() == 3);
}

TEST_CASE("multileader: add a bend to a leader line, remove one, the landing end stays") {
    Fixture f;
    MLeaderData ml;
    ml.lines = {{{0, 0}, {5, 5}, {10, 5}}};
    ml.landing = {10, 5};
    ml.contentPos = {14, 5};
    auto id = add(f.doc, Indirect<MLeaderData>(ml));
    f.host.setSelection({id});
    CHECK(offers(f.doc, id, 0, GripAction::AddVertex));
    CHECK(offers(f.doc, id, 1, GripAction::RemoveVertex));
    CHECK_FALSE(offers(f.doc, id, 2, GripAction::RemoveVertex));  // meets the landing
    CHECK_FALSE(offers(f.doc, id, 3, GripAction::AddVertex));     // the landing grip
    auto r = f.host.run("GRIPEDIT", {"0,0", "AddVertex", "2,4"});
    CHECK(r.finished);
    {
        const auto& l = get<MLeaderData>(f.doc, id).lines[0];
        REQUIRE(l.size() == 4);
        CHECK(near(l[1], {2, 4}));
    }
    CHECK(f.host.undo());
    CHECK(get<MLeaderData>(f.doc, id).lines[0].size() == 3);
    r = f.host.run("GRIPEDIT", {"5,5", "RemoveVertex"});
    CHECK(r.finished);
    CHECK(get<MLeaderData>(f.doc, id).lines[0].size() == 2);
    CHECK(f.host.undo());
    CHECK(get<MLeaderData>(f.doc, id).lines[0].size() == 3);
}

TEST_CASE("multiline: add and remove a vertex keep the cuts of the other segments") {
    Fixture f;
    MlineData m;
    m.v = {{0, 0}, {10, 0}, {20, 0}, {30, 0}};
    m.cuts = {{0, 0.2, 0.4, 0, 255}, {1, 0.2, 0.4, 0, 255}, {2, 0.2, 0.4, 0, 255}};
    auto id = add(f.doc, m);
    CHECK(offers(f.doc, id, 1, GripAction::AddVertex));
    CHECK(offers(f.doc, id, 1, GripAction::RemoveVertex));
    // Add after vertex 1: segment 1 is reshaped and loses its cut, segment 2 becomes 3.
    auto r = tf::applyGripAction(*f.doc.find(id), 1, GripAction::AddVertex, {15, 5});
    REQUIRE(r.ok());
    const auto& a = *entityAs<MlineData>(r.value());
    REQUIRE(a.v.size() == 5);
    CHECK(near(a.v[2], {15, 5}));
    REQUIRE(a.cuts.size() == 2);
    CHECK(a.cuts[0].seg == 0);
    CHECK(a.cuts[1].seg == 3);
    // Remove vertex 2: segments 1 and 2 merge and lose theirs.
    auto q = tf::applyGripAction(*f.doc.find(id), 2, GripAction::RemoveVertex, {});
    REQUIRE(q.ok());
    const auto& b = *entityAs<MlineData>(q.value());
    CHECK(b.v.size() == 3);
    REQUIRE(b.cuts.size() == 1);
    CHECK(b.cuts[0].seg == 0);
    // Never below two vertices.
    MlineData two;
    two.v = {{0, 0}, {10, 0}};
    auto id2 = add(f.doc, two);
    CHECK_FALSE(offers(f.doc, id2, 0, GripAction::RemoveVertex));
    CHECK_FALSE(tf::applyGripAction(*f.doc.find(id2), 0, GripAction::RemoveVertex, {}).ok());
}

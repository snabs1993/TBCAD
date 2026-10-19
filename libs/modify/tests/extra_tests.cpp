#include <algorithm>
#include <random>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::testing;
using namespace tbcad::modify;

TEST_CASE("lengthen a line at the end nearest the pick") {
    Entity l = line({0, 0}, {10, 0});
    auto r = lengthenEntity(l, {9, 1}, LengthenMode::Delta, 5);
    REQUIRE(r.ok());
    const auto& d = as<LineData>(*r.value().modified);
    CHECK(near(d.a, {0, 0}));
    CHECK(near(d.b, {15, 0}));
    r = lengthenEntity(l, {1, 1}, LengthenMode::Delta, -4);
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).a, {4, 0}));
    r = lengthenEntity(l, {9, 0}, LengthenMode::Percent, 150);
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).b, {15, 0}));
    r = lengthenEntity(l, {9, 0}, LengthenMode::Total, 3);
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).b, {3, 0}));
    CHECK_FALSE(lengthenEntity(l, {9, 0}, LengthenMode::Delta, -10).ok());
}

TEST_CASE("lengthen an arc changes its sweep, not its radius") {
    Entity a = arc({0, 0}, 10, 0, kPi / 2);
    double len = 10 * kPi / 2;
    auto r = lengthenEntity(a, {0, 10}, LengthenMode::Delta, 10 * kPi / 4);
    REQUIRE(r.ok());
    const auto& d = as<ArcData>(*r.value().modified);
    CHECK(near(d.radius, 10));
    CHECK(near(d.startAngle, 0));
    CHECK(near(d.endAngle, 3 * kPi / 4, 1e-6));
    CHECK(near(*lengthenableLength(a), len));
    CHECK_FALSE(lengthenEntity(a, {0, 10}, LengthenMode::Delta, 100).ok());
}

TEST_CASE("lengthen and shorten a polyline across segments") {
    Entity p = poly(pts({{0, 0}, {10, 0}, {10, 10}}), false);
    auto r = lengthenEntity(p, {10, 9}, LengthenMode::Delta, 5);
    REQUIRE(r.ok());
    const auto& d = as<PolylineData>(*r.value().modified);
    REQUIRE(d.v.size() == 3);
    CHECK(near(d.v[2].p, {10, 15}));
    r = lengthenEntity(p, {10, 9}, LengthenMode::Delta, -14);  // drops the whole last leg
    REQUIRE(r.ok());
    const auto& s = as<PolylineData>(*r.value().modified);
    REQUIRE(s.v.size() == 2);
    CHECK(near(s.v[1].p, {6, 0}));
    r = lengthenEntity(p, {1, 0}, LengthenMode::Delta, 2);  // start end
    REQUIRE(r.ok());
    CHECK(near(as<PolylineData>(*r.value().modified).v[0].p, {-2, 0}));
}

TEST_CASE("lengthen to a point") {
    Entity l = line({0, 0}, {10, 0});
    auto r = lengthenToPoint(l, {9, 0}, {14, 3});
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).b, {14, 0}));
    Entity a = arc({0, 0}, 10, 0, kPi / 2);
    r = lengthenToPoint(a, {0, 10}, {-5, 5});
    REQUIRE(r.ok());
    CHECK(near(as<ArcData>(*r.value().modified).endAngle, 3 * kPi / 4, 1e-6));
}

TEST_CASE("circles and closed polylines cannot be lengthened") {
    CHECK_FALSE(lengthenEntity(circle({0, 0}, 5), {5, 0}, LengthenMode::Delta, 1).ok());
    CHECK_FALSE(
        lengthenEntity(poly(pts({{0, 0}, {1, 0}, {1, 1}}), true), {0, 0}, LengthenMode::Delta, 1)
            .ok());
}

TEST_CASE("reverse a polyline keeps its shape") {
    std::vector<PolyVertex> v = pts({{0, 0}, {10, 0}, {10, 10}});
    v[1].bulge = 0.5;
    Entity p = poly(v, false);
    auto r = reverseEntity(p);
    REQUIRE(r.ok());
    const auto& d = as<PolylineData>(*r.value().modified);
    REQUIRE(d.v.size() == 3);
    CHECK(near(d.v[0].p, {10, 10}));
    CHECK(near(d.v[2].p, {0, 0}));
    CHECK(near(d.v[0].bulge, -0.5));
    CHECK(near(d.v[1].bulge, 0));
    CHECK(near(d.v[2].bulge, 0));
    // Reversing twice gives the original back.
    Entity back = *r.value().modified;
    auto r2 = reverseEntity(back);
    REQUIRE(r2.ok());
    const auto& e = as<PolylineData>(*r2.value().modified);
    CHECK(near(e.v[1].bulge, 0.5));
    CHECK(near(e.v[0].p, {0, 0}));
}

TEST_CASE("reverse a line and refuse an arc") {
    auto r = reverseEntity(line({1, 2}, {3, 4}));
    REQUIRE(r.ok());
    CHECK(near(as<LineData>(*r.value().modified).a, {3, 4}));
    CHECK_FALSE(reverseEntity(arc({0, 0}, 1, 0, 1)).ok());
}

TEST_CASE("divide an open line, a circle and a polyline") {
    auto d = dividePoints(line({0, 0}, {10, 0}), 4);
    REQUIRE(d.ok());
    REQUIRE(d.value().size() == 3);
    CHECK(near(d.value()[0], {2.5, 0}));
    CHECK(near(d.value()[2], {7.5, 0}));
    auto c = dividePoints(circle({0, 0}, 2), 4);
    REQUIRE(c.ok());
    REQUIRE(c.value().size() == 4);
    CHECK(near(c.value()[0], {2, 0}, 1e-4));
    CHECK(near(c.value()[1], {0, 2}, 1e-4));
    auto sq = dividePoints(poly(pts({{0, 0}, {4, 0}, {4, 4}, {0, 4}}), true), 8);
    REQUIRE(sq.ok());
    REQUIRE(sq.value().size() == 8);
    CHECK(near(sq.value()[1], {2, 0}));
    CHECK(near(sq.value()[2], {4, 0}));
    CHECK_FALSE(dividePoints(line({0, 0}, {1, 0}), 1).ok());
}

TEST_CASE("measure along an arc and from the far end") {
    auto m = measurePoints(line({0, 0}, {10, 0}), 3);
    REQUIRE(m.ok());
    REQUIRE(m.value().size() == 3);
    CHECK(near(m.value()[2], {9, 0}));
    auto f = measurePoints(line({0, 0}, {10, 0}), 4, true);
    REQUIRE(f.ok());
    REQUIRE(f.value().size() == 2);
    CHECK(near(f.value()[0], {6, 0}));
    auto a = measurePoints(arc({0, 0}, 10, 0, kPi), 10 * kPi / 2);
    REQUIRE(a.ok());
    REQUIRE(a.value().size() == 2);
    CHECK(near(a.value()[0], {0, 10}, 1e-4));
    CHECK_FALSE(measurePoints(line({0, 0}, {1, 0}), 0).ok());
}

TEST_CASE("align with one and two point pairs") {
    auto t1 = alignTransform({1, 1}, {5, 6}, std::nullopt, std::nullopt, false);
    REQUIRE(t1.ok());
    CHECK(near(t1.value().apply({2, 1}), {6, 6}));
    auto t2 = alignTransform({0, 0}, {10, 10}, Vec2{1, 0}, Vec2{10, 12}, false);
    REQUIRE(t2.ok());
    CHECK(near(t2.value().apply({0, 0}), {10, 10}));
    CHECK(near(t2.value().apply({2, 0}), {10, 12}));
    auto t3 = alignTransform({0, 0}, {0, 0}, Vec2{1, 0}, Vec2{0, 4}, true);
    REQUIRE(t3.ok());
    CHECK(near(t3.value().apply({1, 0}), {0, 4}));
    CHECK_FALSE(alignTransform({0, 0}, {1, 1}, Vec2{0, 0}, Vec2{2, 2}, true).ok());
}

TEST_CASE("overkill removes duplicates and merges overlapping lines") {
    Entity a = line({0, 0}, {10, 0});
    Entity b = line({10, 0}, {0, 0});  // reversed duplicate
    Entity c = line({5, 0}, {20, 0});  // overlaps a
    Entity d = line({0, 5}, {1, 5});   // unrelated
    Entity e = circle({0, 0}, 3);
    Entity f = circle({0, 0}, 3);
    OverkillResult r = overkill({&a, &b, &c, &d, &e, &f});
    CHECK(r.duplicates == 2);
    CHECK(r.erased == std::vector<std::size_t>{0, 1, 2, 5});
    REQUIRE(r.created.size() == 1);
    const auto& l = as<LineData>(r.created[0]);
    CHECK(near(std::min(l.a.x, l.b.x), 0));
    CHECK(near(std::max(l.a.x, l.b.x), 20));
}

TEST_CASE("overkill respects properties and end-to-end option") {
    Entity a = line({0, 0}, {10, 0});
    Entity b = line({0, 0}, {10, 0});
    b.common.color = Color::byLayer();
    b.common.layer = LayerId{9};
    OverkillResult r = overkill({&a, &b});
    CHECK(r.erased.empty());
    OverkillOptions o;
    o.ignoreProperties = true;
    CHECK(overkill({&a, &b}, o).erased.size() == 1);

    Entity c = line({0, 0}, {5, 0});
    Entity d = line({5, 0}, {9, 0});
    OverkillOptions touch;
    touch.combineEndToEnd = false;
    CHECK(overkill({&c, &d}, touch).created.empty());
    CHECK(overkill({&c, &d}).created.size() == 1);
}

TEST_CASE("overkill merges arcs of one circle and drops duplicate polylines") {
    Entity a = arc({0, 0}, 5, 0, 1.0);
    Entity b = arc({0, 0}, 5, 0.5, 2.0);
    OverkillResult r = overkill({&a, &b});
    REQUIRE(r.created.size() == 1);
    const auto& m = as<ArcData>(r.created[0]);
    CHECK(near(m.startAngle, 0, 1e-6));
    CHECK(near(m.endAngle, 2.0, 1e-6));
    Entity p = poly(pts({{0, 0}, {1, 0}, {1, 1}}), false);
    Entity q = poly(pts({{1, 1}, {1, 0}, {0, 0}}), false);
    CHECK(overkill({&p, &q}).duplicates == 1);
}

TEST_CASE("overkill optimizes polylines and drops duplicate text, inserts and splines") {
    Entity p = poly(pts({{0, 0}, {5, 0}, {5, 0}, {10, 0}, {10, 5}}), false);
    OverkillOptions o;
    CHECK(overkill({&p}, o).created.empty());
    o.optimizePolylines = true;
    OverkillResult r = overkill({&p}, o);
    REQUIRE(r.created.size() == 1);
    CHECK(r.optimized == 1);
    CHECK(r.erased == std::vector<std::size_t>{0});
    const auto& v = as<PolylineData>(r.created[0]).v;
    REQUIRE(v.size() == 3);
    CHECK(near(v[1].p, {10, 0}));
    // A real corner and an already-clean polyline are left alone.
    Entity clean = poly(pts({{0, 0}, {5, 0}, {5, 5}}), false);
    CHECK(overkill({&clean}, o).created.empty());
    // A closed polyline keeps three vertices.
    Entity tri = poly(pts({{0, 0}, {5, 0}, {10, 0}, {5, 5}}), true);
    const OverkillResult triResult = overkill({&tri}, o);
    CHECK(as<PolylineData>(triResult.created.at(0)).v.size() == 3);

    TextData t;
    t.position = {1, 2};
    t.utf8 = "hello";
    Entity t1 = make(t);
    Entity t2 = make(t);
    t.utf8 = "other";
    Entity t3 = make(t);
    OverkillResult tr = overkill({&t1, &t2, &t3});
    CHECK(tr.duplicates == 1);
    CHECK(tr.erased == std::vector<std::size_t>{1});

    InsertData ins;
    ins.block = BlockId{4};
    ins.position = {3, 3};
    Entity i1 = make(ins);
    Entity i2 = make(ins);
    ins.rotation = 0.5;
    Entity i3 = make(ins);
    CHECK(overkill({&i1, &i2, &i3}).duplicates == 1);
}

TEST_CASE("overkill groups many parallel lines quickly") {
    std::vector<Entity> ls;
    for (int i = 0; i < 20000; ++i)
        ls.push_back(line({0, i * 1.0}, {10, i * 1.0}));
    for (int i = 0; i < 20000; i += 2)
        ls.push_back(line({5, i * 1.0}, {20, i * 1.0}));  // overlaps one of the above
    std::vector<const Entity*> in;
    for (const Entity& e : ls)
        in.push_back(&e);
    OverkillResult r = overkill(in);
    CHECK(r.created.size() == 10000);
    CHECK(r.erased.size() == 20000);
}

TEST_CASE("polyline conversions") {
    auto pl = toPolylineEntity(line({0, 0}, {3, 4}));
    REQUIRE(pl.ok());
    CHECK(as<PolylineData>(pl.value()).v.size() == 2);
    auto ap = toPolylineEntity(arc({0, 0}, 5, 0, kPi / 2));
    REQUIRE(ap.ok());
    CHECK(near(as<PolylineData>(ap.value()).v[0].bulge, std::tan(kPi / 8)));
    CHECK_FALSE(toPolylineEntity(circle({0, 0}, 1)).ok());

    Entity p = poly(pts({{0, 0}, {5, 3}, {10, 0}, {15, 3}}), false);
    auto sp = polylineToSpline(p);
    REQUIRE(sp.ok());
    CHECK(kind(sp.value()) == EntityKind::Spline);
    auto back = splineToPolyline(sp.value(), 0.01);
    REQUIRE(back.ok());
    const auto& bp = as<PolylineData>(back.value());
    CHECK(bp.v.size() > 4);
    CHECK(near(bp.v.front().p, {0, 0}, 1e-6));
    CHECK(near(bp.v.back().p, {15, 3}, 1e-6));

    auto w = setPolylineWidth(p, 0.5);
    REQUIRE(w.ok());
    CHECK(near(as<PolylineData>(*w.value().modified).v[1].endWidth, 0.5));
    CHECK_FALSE(setPolylineWidth(line({0, 0}, {1, 1}), 1).ok());
}

TEST_CASE("extrim removes the part of a line beyond a cutting line") {
    Entity edge = line({5, -10}, {5, 10});
    Entity target = line({0, 0}, {10, 0});
    auto r = extrimEntity(target, edge, {8, 3});
    REQUIRE(r.ok());
    REQUIRE(r.value().modified.has_value());
    const auto& d = as<LineData>(*r.value().modified);
    CHECK(near(d.a, {0, 0}));
    CHECK(near(d.b, {5, 0}));
    CHECK(r.value().added.empty());
    // Pick on the other side keeps the right half.
    auto l = extrimEntity(target, edge, {1, 3});
    REQUIRE(l.ok());
    CHECK(near(as<LineData>(*l.value().modified).a, {5, 0}));
}

TEST_CASE("extrim with a circle edge cuts what is inside, keeping both outer ends") {
    Entity edge = circle({5, 0}, 2);
    Entity target = line({0, 0}, {10, 0});
    auto r = extrimEntity(target, edge, {5, 0});  // inside
    REQUIRE(r.ok());
    REQUIRE(r.value().modified.has_value());
    REQUIRE(r.value().added.size() == 1);
    std::vector<double> xs;
    for (const Entity* e : {&*r.value().modified, &r.value().added[0]}) {
        const auto& d = as<LineData>(*e);
        xs.push_back(std::min(d.a.x, d.b.x));
        xs.push_back(std::max(d.a.x, d.b.x));
    }
    std::sort(xs.begin(), xs.end());
    CHECK(near(xs[0], 0));
    CHECK(near(xs[1], 3));
    CHECK(near(xs[2], 7));
    CHECK(near(xs[3], 10));
}

TEST_CASE("extrim reports edges without two sides and misses") {
    Entity target = line({0, 0}, {10, 0});
    CHECK_FALSE(extrimEntity(target, poly(pts({{0, 5}, {1, 5}, {2, 6}}), false), {0, 0}).ok());
    auto none = extrimEntity(target, line({50, -1}, {50, 1}), {60, 0});
    REQUIRE(none.ok());
    CHECK_FALSE(none.value().modified.has_value());
    CHECK_FALSE(none.value().eraseSource);
    // Everything lies on the picked side: the target disappears.
    auto gone = extrimEntity(target, line({-5, -1}, {-5, 1}), {5, 0});
    REQUIRE(gone.ok());
    CHECK_FALSE(gone.value().eraseSource);  // never crosses the edge
}

TEST_CASE("blend two lines with a tangent spline") {
    Entity a = line({0, 0}, {10, 0});
    Entity b = line({30, 10}, {20, 10});
    auto r = blendCurves(a, {9, 0}, b, {21, 10});
    REQUIRE(r.ok());
    REQUIRE(kind(r.value()) == EntityKind::Spline);
    std::vector<geom::CurveRef> cs;
    REQUIRE(geom::curvesOf(r.value(), cs));
    const auto& c = cs.front();
    Vec2 s = c.pointAt(c.t0()), e = c.pointAt(c.t0() + c.sweep());
    CHECK(near(s, {10, 0}, 1e-6));
    CHECK(near(e, {20, 10}, 1e-6));
    Vec2 ts = c.tangentAt(c.t0()), te = c.tangentAt(c.t0() + c.sweep());
    CHECK(near(ts, {1, 0}, 1e-6));
    CHECK(near(te, {1, 0}, 1e-6));  // arrives travelling along +x, into b's end
    CHECK_FALSE(blendCurves(a, {9, 0}, line({10, 0}, {12, 3}), {10, 0}).ok());
    CHECK_FALSE(blendCurves(a, {9, 0}, circle({0, 0}, 1), {1, 0}).ok());
}

TEST_CASE("polyline vertex insert, remove and move") {
    Entity p = poly(pts({{0, 0}, {10, 0}, {10, 10}}), false);
    auto ins = insertPolylineVertex(p, {4, 1});
    REQUIRE(ins.ok());
    const auto& a = as<PolylineData>(*ins.value().modified);
    REQUIRE(a.v.size() == 4);
    CHECK(near(a.v[1].p, {4, 0}));
    CHECK_FALSE(insertPolylineVertex(p, {10, 0}).ok());  // already a vertex

    auto rem = removePolylineVertex(*ins.value().modified, {4.2, 0.3});
    REQUIRE(rem.ok());
    CHECK(as<PolylineData>(*rem.value().modified).v.size() == 3);
    CHECK_FALSE(removePolylineVertex(poly(pts({{0, 0}, {1, 0}}), false), {0, 0}).ok());

    auto mv = movePolylineVertex(p, {10.5, 0.5}, {12, 3});
    REQUIRE(mv.ok());
    CHECK(near(as<PolylineData>(*mv.value().modified).v[1].p, {12, 3}));
}

TEST_CASE("inserting into an arc segment keeps the arc") {
    std::vector<PolyVertex> v = pts({{0, 0}, {10, 0}});
    v[0].bulge = 1;  // half circle through (5, -5)
    Entity p = poly(v, false);
    auto ins = insertPolylineVertex(p, {5, -5});
    REQUIRE(ins.ok());
    const auto& a = as<PolylineData>(*ins.value().modified);
    REQUIRE(a.v.size() == 3);
    CHECK(near(a.v[1].p, {5, -5}, 1e-6));
    CHECK(near(a.v[0].bulge, std::tan(kPi / 8), 1e-6));
    CHECK(near(a.v[1].bulge, std::tan(kPi / 8), 1e-6));
}

TEST_CASE("spline fit point editing") {
    Entity src = poly(pts({{0, 0}, {5, 4}, {10, 0}, {15, 4}}), false);
    auto sp = polylineToSpline(src);
    REQUIRE(sp.ok());
    std::size_t n = as<SplineData>(sp.value()).fit.size();
    auto add = editSplineFit(sp.value(), SplineFitOp::Add, {7, 3});
    REQUIRE(add.ok());
    CHECK(as<SplineData>(*add.value().modified).fit.size() == n + 1);
    auto del = editSplineFit(sp.value(), SplineFitOp::Delete, {5, 4});
    REQUIRE(del.ok());
    CHECK(as<SplineData>(*del.value().modified).fit.size() == n - 1);
    auto mv = editSplineFit(sp.value(), SplineFitOp::Move, {5, 4}, {5, 8});
    REQUIRE(mv.ok());
    bool moved = false;
    for (Vec2 q : as<SplineData>(*mv.value().modified).fit)
        moved = moved || near(q, {5, 8}, 1e-9);
    CHECK(moved);
    auto closed = editSplineFit(sp.value(), SplineFitOp::Close);
    REQUIRE(closed.ok());
    CHECK(as<SplineData>(*closed.value().modified).closed);
    CHECK_FALSE(editSplineFit(line({0, 0}, {1, 1}), SplineFitOp::Add).ok());
}

TEST_CASE("replaceText replaces in text kinds, honours case and skips the rest") {
    Entity t;
    TextData td;
    td.utf8 = "Door door DOOR";
    t.data = Indirect<TextData>(td);
    auto r = replaceText(t, "door", "window", false);
    REQUIRE(r.has_value());
    CHECK(r->count == 3);
    CHECK(entityAs<TextData>(r->entity)->utf8 == "window window window");
    auto cs = replaceText(t, "door", "x", true);
    REQUIRE(cs.has_value());
    CHECK(cs->count == 1);
    CHECK(entityAs<TextData>(cs->entity)->utf8 == "Door x DOOR");
    CHECK_FALSE(replaceText(t, "wall", "x", false).has_value());
    CHECK_FALSE(replaceText(t, "", "x", false).has_value());
    auto del = replaceText(t, " ", "", true);
    REQUIRE(del.has_value());
    CHECK(entityAs<TextData>(del->entity)->utf8 == "DoordoorDOOR");
    CHECK_FALSE(replaceText(line({0, 0}, {1, 1}), "a", "b", false).has_value());
}

TEST_CASE("replaceText in MTEXT leaves the formatting codes alone") {
    auto mtext = [](const char* markup) {
        Entity e;
        MTextData m;
        m.utf8 = markup;
        e.data = Indirect<MTextData>(m);
        return e;
    };
    // Words that also occur in the codes are not matched there.
    CHECK_FALSE(replaceText(mtext("{\\b1;Hello}"), "b1", "x", true).has_value());
    CHECK_FALSE(replaceText(mtext("one\\Ptwo"), "P", "x", true).has_value());
    CHECK_FALSE(replaceText(mtext("{\\C7;red}"), "C7", "x", true).has_value());
    CHECK_FALSE(
        replaceText(mtext("\\S1/2;"), "1/2", "x", true).has_value());  // a fraction is one code
    // A match inside formatted text keeps the formatting around it.
    auto r = replaceText(mtext("{\\b1;Hello} world"), "Hello", "Bye", true);
    REQUIRE(r.has_value());
    CHECK(entityAs<MTextData>(r->entity)->utf8 == "{\\b1;Bye} world");
    // A match across a formatting change is found; the replacement takes the first piece's format.
    auto span = replaceText(mtext("Hel{\\b1;lo} world"), "Hello", "Bye", true);
    REQUIRE(span.has_value());
    CHECK(span->count == 1);
    CHECK(entityAs<MTextData>(span->entity)->utf8 == "Bye{\\b1;} world");
    // Special characters in the replacement are text, not markup.
    auto esc = replaceText(mtext("a b"), "a", "{\\b1;", true);
    REQUIRE(esc.has_value());
    CHECK(entityAs<MTextData>(esc->entity)->utf8 ==
          "\\{\\\\b1;"
          " b");
    // Escaped characters in the source match as the characters they stand for.
    auto lit = replaceText(mtext("x\\{y\\}"), "{y}", "z", true);
    REQUIRE(lit.has_value());
    CHECK(entityAs<MTextData>(lit->entity)->utf8 == "xz");
    // Thai text matches by bytes of whole characters.
    auto thai = replaceText(mtext("{\\i1;สวัสดี}"), "วั", "-", true);
    REQUIRE(thai.has_value());
    CHECK(entityAs<MTextData>(thai->entity)->utf8 == "{\\i1;ส-สดี}");
}

namespace {
Entity cvSpline(double ox, double oy) {
    SplineData sp;
    sp.degree = 3;
    sp.knots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
    sp.control = {
        {ox, oy}, {ox + 10, oy + 20}, {ox + 30, oy - 20}, {ox + 50, oy + 10}, {ox + 60, oy}};
    return make(Indirect<SplineData>(sp));
}

geom::CurveRef curveOf(const Entity& e) {
    std::vector<geom::CurveRef> cs;
    REQUIRE(geom::curvesOf(e, cs));
    REQUIRE(cs.size() == 1);
    return cs[0];
}
}  // namespace

TEST_CASE("SPLINEDIT control vertices: Add keeps the curve exactly, at survey coordinates too") {
    for (double ox : {0.0, 500123.456}) {
        const double oy = ox * 9;
        Entity e = cvSpline(ox, oy);
        auto r = editSplineCv(e, SplineCvOp::Add, {ox + 20, oy});
        REQUIRE(r.ok());
        REQUIRE(r.value().modified.has_value());
        const auto& after = as<SplineData>(*r.value().modified);
        CHECK(after.control.size() == 6);
        CHECK(after.knots.size() == 10);
        auto c0 = curveOf(e), c1 = curveOf(*r.value().modified);
        for (int i = 0; i <= 40; ++i) {
            double u = i / 40.0;
            Vec2 p0 = c0.pointAt(c0.t0() + u * c0.sweep());
            Vec2 p1 = c1.pointAt(c1.t0() + u * c1.sweep());
            CHECK(distance(p0, p1) < 1e-6);
        }
    }
}

TEST_CASE("SPLINEDIT control vertices: Add on a rational spline keeps the curve") {
    Entity e = cvSpline(0, 0);
    SplineData sp = as<SplineData>(e);
    sp.weights = {1, 2, 0.5, 1.5, 1};
    e = make(Indirect<SplineData>(sp));
    auto r = editSplineCv(e, SplineCvOp::Add, {40, 0});
    REQUIRE(r.ok());
    auto c0 = curveOf(e), c1 = curveOf(*r.value().modified);
    for (int i = 0; i <= 40; ++i) {
        double u = i / 40.0;
        CHECK(distance(c0.pointAt(c0.t0() + u * c0.sweep()), c1.pointAt(c1.t0() + u * c1.sweep())) <
              1e-6);
    }
}

TEST_CASE("SPLINEDIT control vertices: Delete, Move, Weight, Purge") {
    Entity e = cvSpline(100, 100);
    auto del = editSplineCv(e, SplineCvOp::Delete, {130, 80});
    REQUIRE(del.ok());
    const auto& d = as<SplineData>(*del.value().modified);
    CHECK(d.control.size() == 4);
    CHECK(d.knots.size() == 8);
    auto c = curveOf(*del.value().modified);
    CHECK(near(c.startPoint(), {100, 100}));
    CHECK(near(c.endPoint(), {160, 100}));

    auto mv = editSplineCv(e, SplineCvOp::Move, {110, 120}, {110, 140});
    REQUIRE(mv.ok());
    CHECK(near(as<SplineData>(*mv.value().modified).control[1], {110, 140}));

    auto w = editSplineCv(e, SplineCvOp::Weight, {110, 120}, {}, 5);
    REQUIRE(w.ok());
    const auto& wd = as<SplineData>(*w.value().modified);
    REQUIRE(wd.weights.size() == 5);
    CHECK(wd.weights[1] == doctest::Approx(5));
    // A heavier vertex pulls the curve towards it.
    CHECK(curveOf(*w.value().modified).distanceTo({110, 120}) < curveOf(e).distanceTo({110, 120}));
    CHECK_FALSE(editSplineCv(e, SplineCvOp::Weight, {110, 120}, {}, -1).ok());

    // Purge needs fit points.
    CHECK_FALSE(editSplineCv(e, SplineCvOp::Purge).ok());
    auto fitted = geom::interpolateSpline(std::vector<Vec2>{{0, 0}, {5, 4}, {10, 0}, {15, 4}});
    REQUIRE(fitted.ok());
    auto pg = editSplineCv(make(Indirect<SplineData>(fitted.value())), SplineCvOp::Purge);
    REQUIRE(pg.ok());
    CHECK(as<SplineData>(*pg.value().modified).fit.empty());
}

TEST_CASE("SPLINEDIT control vertices: Close and Open, and a closed spline keeps its wrap") {
    Entity e = cvSpline(0, 0);
    auto closed = editSplineCv(e, SplineCvOp::Close);
    REQUIRE(closed.ok());
    const auto& cd = as<SplineData>(*closed.value().modified);
    CHECK(cd.closed);
    CHECK(cd.periodic);
    CHECK(cd.control.size() == 5 + 3);
    CHECK(near(curveOf(*closed.value().modified).startPoint(),
               curveOf(*closed.value().modified).endPoint(), 1e-6));
    // Moving a vertex that is repeated at the end moves both copies.
    auto mv = editSplineCv(*closed.value().modified, SplineCvOp::Move, {0, 0}, {-5, -5});
    REQUIRE(mv.ok());
    const auto& md = as<SplineData>(*mv.value().modified);
    CHECK(near(md.control.front(), {-5, -5}));
    CHECK(near(md.control[5], {-5, -5}));
    auto reopened = editSplineCv(*closed.value().modified, SplineCvOp::Open);
    REQUIRE(reopened.ok());
    CHECK_FALSE(as<SplineData>(*reopened.value().modified).closed);
    CHECK_FALSE(editSplineCv(e, SplineCvOp::Open).ok());
}

TEST_CASE("SPLINEDIT converts a spline to a polyline within the tolerance") {
    for (double ox : {0.0, 500123.456}) {
        Entity e = cvSpline(ox, ox * 9);
        auto r = editSplineCv(e, SplineCvOp::ToPolyline, {}, {}, 0.05);
        REQUIRE(r.ok());
        const auto& pl = as<PolylineData>(*r.value().modified);
        CHECK(pl.v.size() > 10);
        auto c = curveOf(e);
        for (const auto& v : pl.v)
            CHECK(c.distanceTo(v.p) < 0.06);
    }
}

TEST_CASE("LENGTHEN an elliptical arc and an open spline") {
    EllipseData el;
    el.majorAxis = {10, 0};
    el.ratio = 0.5;
    el.startParam = 0.3;
    el.endParam = 1.0;
    Entity e = make(el);
    const double len = lengthenableLength(e).value();
    const Vec2 endPick = curveOf(e).endPoint(), startPick = curveOf(e).startPoint();
    auto grown = lengthenEntity(e, endPick, LengthenMode::Delta, 3.0);
    REQUIRE(grown.ok());
    CHECK(near(lengthenableLength(*grown.value().modified).value(), len + 3.0, 1e-6));
    CHECK(near(as<EllipseData>(*grown.value().modified).startParam, 0.3));
    auto front = lengthenEntity(e, startPick, LengthenMode::Total, len + 2.0);
    REQUIRE(front.ok());
    CHECK(near(lengthenableLength(*front.value().modified).value(), len + 2.0, 1e-6));
    CHECK(near(as<EllipseData>(*front.value().modified).endParam, 1.0));
    auto cut = lengthenEntity(e, endPick, LengthenMode::Percent, 50);
    REQUIRE(cut.ok());
    CHECK(near(lengthenableLength(*cut.value().modified).value(), len / 2, 1e-6));
    CHECK_FALSE(lengthenEntity(e, endPick, LengthenMode::Delta, -len).ok());
    CHECK_FALSE(lengthenEntity(e, endPick, LengthenMode::Delta, 1000).ok());
    auto toPt = lengthenToPoint(e, endPick, {10 * std::cos(2.0), 5 * std::sin(2.0)});
    REQUIRE(toPt.ok());
    CHECK(near(as<EllipseData>(*toPt.value().modified).endParam, 2.0, 1e-6));

    for (double ox : {0.0, 500123.456}) {
        Entity s = cvSpline(ox, 0);
        const double sl = lengthenableLength(s).value();
        const geom::CurveRef c0 = curveOf(s);
        auto more = lengthenEntity(s, c0.endPoint(), LengthenMode::Delta, 12.0);
        REQUIRE(more.ok());
        const geom::CurveRef c1 = curveOf(*more.value().modified);
        CHECK(near(c1.length(), sl + 12.0, 1e-5));
        CHECK(near(c1.startPoint(), c0.startPoint(), 1e-6));
        for (double t : {0.3, 1.2, 1.9})
            CHECK(c1.distanceTo(c0.pointAt(t)) < 1e-6);
        auto less = lengthenEntity(s, c0.startPoint(), LengthenMode::Delta, -10.0);
        REQUIRE(less.ok());
        const geom::CurveRef c2 = curveOf(*less.value().modified);
        CHECK(near(c2.length(), sl - 10.0, 1e-5));
        CHECK(near(c2.endPoint(), c0.endPoint(), 1e-6));
        CHECK(c2.distanceTo(c0.pointAt(1.5)) < 1e-6);
        SplineData closed = *entityAs<SplineData>(s);
        closed.closed = true;
        CHECK_FALSE(lengthenableLength(make(Indirect<SplineData>(closed))).has_value());
    }
}

TEST_CASE("BLEND from an elliptical arc end is tangent to it") {
    EllipseData el;
    el.majorAxis = {10, 0};
    el.ratio = 0.5;
    el.startParam = 0;
    el.endParam = kPi / 2;  // ends at (0, 5) heading in -x
    Entity e = make(el);
    Entity l = line({-20, 10}, {-30, 10});
    auto r = blendCurves(e, {0, 5}, l, {-20, 10});
    REQUIRE(r.ok());
    const geom::CurveRef c = curveOf(r.value());
    CHECK(near(c.startPoint(), {0, 5}, 1e-6));
    CHECK(near(c.endPoint(), {-20, 10}, 1e-6));
    const Vec2 t = c.tangentAt(c.t0());
    CHECK(t.x < -0.99);
    CHECK(std::abs(t.y) < 0.15);
}

TEST_CASE("PEDIT vertex width, straighten and taper") {
    Entity pl = poly(pts({{0, 0}, {10, 0}, {10, 10}, {20, 10}}), false);
    auto w = setPolylineVertexWidth(pl, {10.2, 0.1}, 1, 3);
    REQUIRE(w.ok());
    const auto& pw = as<PolylineData>(*w.value().modified);
    CHECK(pw.v[1].startWidth == 1);
    CHECK(pw.v[1].endWidth == 3);
    CHECK(pw.v[0].startWidth == 0);
    CHECK_FALSE(setPolylineVertexWidth(pl, {20, 10}, 1, 1).ok());  // the last vertex starts nothing
    CHECK_FALSE(setPolylineVertexWidth(pl, {0, 0}, -1, 1).ok());

    // Straighten from the first to the last vertex: one straight segment.
    std::vector<PolyVertex> v{
        {{0, 0}, 0.5, 0, 0}, {{10, 0}, 0.3, 0, 0}, {{10, 10}, 0, 0, 0}, {{20, 10}, 0, 0, 0}};
    Entity arcs = poly(v, false);
    auto st = straightenPolyline(arcs, {0, 0}, {20, 10});
    REQUIRE(st.ok());
    const auto& ps = as<PolylineData>(*st.value().modified);
    CHECK(ps.v.size() == 2);
    CHECK(ps.v[0].bulge == 0);
    CHECK(near(ps.v[1].p, {20, 10}));
    // Two neighbours: just that segment loses its bulge.
    auto one = straightenPolyline(arcs, {10, 0}, {10, 10});
    REQUIRE(one.ok());
    CHECK(as<PolylineData>(*one.value().modified).v.size() == 4);
    CHECK(as<PolylineData>(*one.value().modified).v[1].bulge == 0);
    CHECK(as<PolylineData>(*one.value().modified).v[0].bulge == 0.5);
    CHECK_FALSE(straightenPolyline(arcs, {0, 0}, {0.1, 0}).ok());

    // Taper over the whole length: 30 long, widths 0 -> 6.
    auto tp = taperPolyline(pl, 0, 6);
    REQUIRE(tp.ok());
    const auto& pt = as<PolylineData>(*tp.value().modified);
    CHECK(near(pt.v[0].startWidth, 0));
    CHECK(near(pt.v[0].endWidth, 2));
    CHECK(near(pt.v[1].startWidth, 2));
    CHECK(near(pt.v[1].endWidth, 4));
    CHECK(near(pt.v[2].endWidth, 6));
    // Closed polylines taper round the loop too.
    Entity sq = poly(pts({{0, 0}, {10, 0}, {10, 10}, {0, 10}}), true);
    auto tq = taperPolyline(sq, 4, 0);
    REQUIRE(tq.ok());
    CHECK(near(as<PolylineData>(*tq.value().modified).v[3].endWidth, 0));
}

TEST_CASE("PEDIT Join with a fuzz distance gathers shuffled, reversed and gapped pieces") {
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> step(5, 15), turn(-1.0, 1.0), noise(-1, 1),
        bul(-0.6, 0.6);
    for (int iter = 0; iter < 40; ++iter) {
        const double fuzz = 0.5;
        const int pieces = 3 + iter % 6;
        // One long open chain of arcs/lines, cut into pieces of two segments each.
        std::vector<Vec2> pts_;
        std::vector<double> bulges;
        Vec2 p{0, 0};
        double h = 0;
        const int segs = pieces * 2;
        for (int i = 0; i <= segs; ++i) {
            pts_.push_back(p);
            bulges.push_back((rng() % 2) ? bul(rng) : 0.0);
            h += turn(rng) * 0.5;
            p = p + Vec2{std::cos(h), std::sin(h)} * step(rng);
        }
        std::vector<Entity> input;
        for (int k = 0; k < pieces; ++k) {
            std::vector<PolyVertex> v;
            for (int j = 0; j <= 2; ++j) {
                Vec2 q = pts_[k * 2 + j];
                // Joint ends move by less than half the fuzz.
                if (j == 0 && k > 0)
                    q = q + Vec2{noise(rng), noise(rng)} * (0.1 * fuzz);
                if (j == 2 && k < pieces - 1)
                    q = q + Vec2{noise(rng), noise(rng)} * (0.1 * fuzz);
                v.push_back({q, j < 2 ? bulges[k * 2 + j] : 0.0, 0, 0});
            }
            Entity e = poly(v, false);
            if (rng() % 2)
                e = *reverseEntity(e).value().modified;
            input.push_back(e);
        }
        std::shuffle(input.begin(), input.end(), rng);
        std::vector<const Entity*> ptrs_;
        for (const auto& e : input)
            ptrs_.push_back(&e);
        auto res = joinEntities(ptrs_, fuzz);
        CHECK(res.consumed.size() == input.size());
        REQUIRE(res.created.size() == 1);
        const auto& out = as<PolylineData>(res.created[0]);
        CHECK(out.v.size() == static_cast<std::size_t>(segs + 1));
        CHECK_FALSE(out.closed);
        // Without the fuzz the gaps keep the pieces apart.
        auto strict = joinEntities(ptrs_, 1e-9);
        CHECK(strict.created.size() != 1);
    }
}

TEST_CASE("refitSpline uses fewer fit points and stays within the tolerance") {
    std::vector<PolyVertex> v;
    for (int i = 0; i <= 40; ++i)
        v.push_back(PolyVertex{{i * 0.5, 3 * std::sin(i * 0.5 * 0.4)}, 0, 0, 0});
    auto sp = polylineToSpline(poly(v, false));
    REQUIRE(sp.ok());
    const std::size_t before = as<SplineData>(sp.value()).fit.size();
    REQUIRE(before >= 30);
    auto out = refitSpline(sp.value(), 0.05);
    REQUIRE(out.ok());
    const auto& ns = as<SplineData>(*out.value().modified);
    CHECK(ns.fit.size() < before);
    CHECK(ns.fit.size() >= 4);
    // every point of the old curve is near the new one
    std::vector<geom::CurveRef> oc, nc;
    REQUIRE(geom::curvesOf(sp.value(), oc));
    REQUIRE(geom::curvesOf(*out.value().modified, nc));
    double worst = 0;
    for (int i = 0; i <= 200; ++i) {
        const Vec2 q = oc.front().pointAt(oc.front().paramAtLength(oc.front().length() * i / 200));
        worst = std::max(worst, length(nc.front().closestPoint(q) - q));
    }
    CHECK(worst <= 0.06);
    // the ends stay put
    CHECK(near(ns.fit.front(), as<SplineData>(sp.value()).fit.front(), 1e-6));
    CHECK(near(ns.fit.back(), as<SplineData>(sp.value()).fit.back(), 1e-6));
    CHECK_FALSE(refitSpline(sp.value(), 0).ok());
    CHECK_FALSE(refitSpline(poly(v, false), 0.1).ok());
}

#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/constraints/constraints.h"

using namespace tbcad;
using namespace tbcad::constraints;

TEST_CASE("parameters evaluate in dependency order and refuse cycles and bad names") {
    ParameterSet p;
    CHECK_FALSE(p.set("Height", "Width * 0.5"));  // Width is not defined yet
    REQUIRE(p.set("Width", "120"));
    REQUIRE(p.set("Height", "Width * 0.5"));
    auto v = p.values();
    REQUIRE(v);
    CHECK(v.value().at("Width") == doctest::Approx(120));
    CHECK(v.value().at("Height") == doctest::Approx(60));
    CHECK(p.evaluate("Width + Height * 2").value() == doctest::Approx(240));
    CHECK(p.evaluate("if(Width > 100, 1, 2)").value() == doctest::Approx(1));
    // Cycle: Width depends on Height which depends on Width.
    CHECK_FALSE(p.set("Width", "Height + 1"));
    CHECK(p.find("Width")->expr == "120");  // unchanged
    CHECK_FALSE(p.set("1bad", "3"));
    CHECK_FALSE(p.set("with space", "3"));
    CHECK_FALSE(p.set("pi", "3"));
    CHECK_FALSE(p.set("X", "Missing + 1"));
    CHECK_FALSE(p.set("Y", "1 +"));
    CHECK_FALSE(p.evaluate("Width / 0").ok());
    CHECK(p.unknownNames("Width + Foo").size() == 1);

    const ParameterSet again = ParameterSet::fromText(p.toText());
    CHECK(again.all().size() == 2);
    CHECK(again.values().value().at("Height") == doctest::Approx(60));
    CHECK(p.remove("Height"));
    CHECK_FALSE(p.remove("Height"));
}

TEST_CASE("constraint formulas survive the text form") {
    ConstraintSet s;
    Constraint c;
    c.kind = Kind::Distance;
    c.a = {EntityId{1}, Part::Start};
    c.b = {EntityId{1}, Part::End};
    c.value = 60;
    c.expr = "Width * 0.5";
    s.add(c);
    Constraint d;
    d.kind = Kind::Radius;
    d.a = {EntityId{2}, Part::Whole};
    d.value = 5;
    s.add(d);
    const ConstraintSet back = ConstraintSet::fromText(s.toText());
    REQUIRE(back.all().size() == 2);
    CHECK(back.all()[0].expr == "Width * 0.5");
    CHECK(back.all()[0].value == doctest::Approx(60));
    CHECK(back.all()[1].expr.empty());
}

namespace {
EntityId addLine(Transaction& tx, Vec2 a, Vec2 b) {
    Entity e;
    e.data = LineData{a, b};
    return tx.add(std::move(e)).value();
}
EntityId addCircle(Transaction& tx, Vec2 c, double r) {
    Entity e;
    e.data = CircleData{c, r};
    return tx.add(std::move(e)).value();
}
bool hasKind(const std::vector<Constraint>& v, Kind k) {
    return std::any_of(v.begin(), v.end(), [&](const Constraint& c) { return c.kind == k; });
}
std::size_t count(const std::vector<Constraint>& v, Kind k) {
    return static_cast<std::size_t>(
        std::count_if(v.begin(), v.end(), [&](const Constraint& c) { return c.kind == k; }));
}
}  // namespace

TEST_CASE("autoConstrain finds the relations a sketch already has") {
    Document doc;
    std::vector<EntityId> ids;
    {
        auto tx = doc.begin("sketch");
        // A slightly untidy rectangle with a circle touching the top, and a tilted copy.
        ids.push_back(addLine(tx, {0, 0}, {40, 0.001}));
        ids.push_back(addLine(tx, {40, 0.001}, {40.002, 20}));
        ids.push_back(addLine(tx, {40.002, 20}, {0, 20}));
        ids.push_back(addLine(tx, {0, 20}, {0, 0}));
        ids.push_back(addCircle(tx, {20, 30}, 10));  // tangent to the top line (y = 20)
        ids.push_back(addCircle(tx, {60, 30}, 10));  // equal to the first one
        tx.commit();
    }
    ConstraintSet none;
    const auto found = autoConstrain(doc, ids, none, AutoOptions{.distance = 0.01, .angle = 0.01});
    CHECK(count(found, Kind::Coincident) == 4);
    CHECK(count(found, Kind::Horizontal) == 2);
    CHECK(count(found, Kind::Vertical) == 2);
    CHECK(count(found, Kind::Equal) >= 2);  // top = bottom, left = ... circles
    CHECK(hasKind(found, Kind::Tangent));
    // Horizontal/vertical lines are not also reported as parallel to each other.
    CHECK_FALSE(hasKind(found, Kind::Parallel));
    CHECK_FALSE(hasKind(found, Kind::Perpendicular));

    // Nothing is reported twice once the constraints exist.
    ConstraintSet existing;
    for (const Constraint& c : found)
        existing.add(c);
    CHECK(autoConstrain(doc, ids, existing).empty());

    // With a tolerance of zero nothing nearly-true is found.
    AutoOptions strict;
    strict.distance = 1e-9;
    strict.angle = 1e-9;
    const auto few = autoConstrain(doc, ids, none, strict);
    CHECK(count(few, Kind::Horizontal) == 1);  // only the top line is exactly horizontal
}

TEST_CASE("autoConstrain: tilted lines give parallel and perpendicular, not horizontal") {
    Document doc;
    std::vector<EntityId> ids;
    {
        auto tx = doc.begin("tilt");
        ids.push_back(addLine(tx, {0, 0}, {10, 10}));
        ids.push_back(addLine(tx, {0, 5}, {10, 15}));
        ids.push_back(addLine(tx, {0, 0}, {-10, 10}));
        tx.commit();
    }
    const auto found = autoConstrain(doc, ids, ConstraintSet{});
    CHECK(count(found, Kind::Horizontal) + count(found, Kind::Vertical) == 0);
    CHECK(count(found, Kind::Parallel) == 1);
    CHECK(count(found, Kind::Perpendicular) == 2);
}

TEST_CASE("applyParameters drives a dimensional constraint and the geometry follows") {
    Document doc;
    EntityId l{};
    {
        auto tx = doc.begin("line");
        l = addLine(tx, {0, 0}, {50, 0});
        tx.commit();
    }
    ConstraintSet set;
    Constraint fixA;
    fixA.kind = Kind::Fix;
    fixA.a = {l, Part::Start};
    set.add(fixA);
    Constraint hor;
    hor.kind = Kind::Horizontal;
    hor.a = {l, Part::Whole};
    set.add(hor);
    Constraint len;
    len.kind = Kind::Distance;
    len.a = {l, Part::Start};
    len.b = {l, Part::End};
    len.value = 50;
    len.expr = "Width * 2";
    set.add(len);
    ParameterSet params;
    REQUIRE(params.set("Width", "40"));
    {
        auto tx = doc.begin("apply");
        auto res = applyParameters(tx, set, params);
        REQUIRE(res);
        CHECK(res.value().converged);
        tx.commit();
    }
    CHECK(set.find(len.id == 0 ? 3 : len.id)->value == doctest::Approx(80));
    const auto* line = entityAs<LineData>(*doc.find(l));
    CHECK(line->b.x == doctest::Approx(80).epsilon(1e-6));
    CHECK(line->b.y == doctest::Approx(0).epsilon(1e-6));
    // A formula that fails leaves everything as it is.
    ParameterSet bad;
    REQUIRE(bad.set("Width", "0 - 5"));
    {
        auto tx = doc.begin("bad");
        CHECK_FALSE(applyParameters(tx, set, bad).ok());
        tx.commit();
    }
    CHECK(entityAs<LineData>(*doc.find(l))->b.x == doctest::Approx(80).epsilon(1e-6));
}

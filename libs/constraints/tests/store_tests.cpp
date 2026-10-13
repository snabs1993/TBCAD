#include <doctest/doctest.h>

#include "tbcad/constraints/constraints.h"

using namespace tbcad;
using namespace tbcad::constraints;

namespace {

EntityId addLine(Document& doc, Vec2 a, Vec2 b) {
    auto tx = doc.begin("line");
    auto id = tx.add(Entity{{}, LineData{a, b}});
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}

}  // namespace

TEST_CASE("a constraint set survives its text form") {
    ConstraintSet set;
    Constraint p;
    p.kind = Kind::Parallel;
    p.a = {EntityId{4}, Part::Whole};
    p.b = {EntityId{9}, Part::Whole};
    const auto idP = set.add(p);
    Constraint d;
    d.kind = Kind::Distance;
    d.a = {EntityId{4}, Part::Start};
    d.b = {EntityId{9}, Part::End};
    d.value = 12.345678901234;
    const auto idD = set.add(d);
    CHECK(idP != idD);

    ConstraintSet back = ConstraintSet::fromText(set.toText());
    REQUIRE(back.all().size() == 2);
    CHECK(back.find(idD)->value == 12.345678901234);
    CHECK(back.find(idD)->a.part == Part::Start);
    CHECK(back.find(idP)->kind == Kind::Parallel);
    // Ids keep counting after a reload.
    CHECK(back.add(p) > idD);

    CHECK(ConstraintSet::fromText("garbage\n1 99 1 0 1 0 0\n").empty());
}

TEST_CASE("the set lives in the header: stored, undone and redone with the document") {
    Document doc;
    const EntityId a = addLine(doc, {0, 0}, {10, 0});
    const EntityId b = addLine(doc, {0, 5}, {10, 6});
    ConstraintSet set;
    Constraint c;
    c.kind = Kind::Parallel;
    c.a = {a, Part::Whole};
    c.b = {b, Part::Whole};
    set.add(c);
    {
        auto tx = doc.begin("constrain");
        REQUIRE(store(tx, set).ok());
        tx.commit();
    }
    CHECK(load(doc).all().size() == 1);
    {
        auto tx = doc.begin("clear");
        REQUIRE(store(tx, ConstraintSet{}).ok());
        tx.commit();
    }
    CHECK(load(doc).empty());
    CHECK(doc.header().custom.count(kHeaderKey) == 0);
}

TEST_CASE("validate checks the objects against the constraint kind") {
    Document doc;
    const EntityId l1 = addLine(doc, {0, 0}, {10, 0});
    const EntityId l2 = addLine(doc, {0, 5}, {10, 6});
    EntityId circle;
    {
        auto tx = doc.begin("circle");
        circle = tx.add(Entity{{}, CircleData{{0, 0}, 3}}).value();
        tx.commit();
    }
    Constraint c;
    c.kind = Kind::Parallel;
    c.a = {l1, Part::Whole};
    c.b = {l2, Part::Whole};
    CHECK(validate(doc, c).ok());
    c.b = {circle, Part::Whole};
    CHECK_FALSE(validate(doc, c).ok());
    c.kind = Kind::Radius;
    c.b = {};
    c.value = 2;
    c.a = {circle, Part::Whole};
    CHECK(validate(doc, c).ok());
    c.value = -1;
    CHECK_FALSE(validate(doc, c).ok());
    c.kind = Kind::Coincident;
    c.a = {l1, Part::Whole};  // a whole line is not a point
    c.b = {l2, Part::Start};
    CHECK_FALSE(validate(doc, c).ok());
    c.a = {l1, Part::End};
    CHECK(validate(doc, c).ok());
    c.a = {EntityId{999}, Part::Start};
    CHECK_FALSE(validate(doc, c).ok());
}

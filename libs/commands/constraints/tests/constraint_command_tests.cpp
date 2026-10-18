#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/constraints/constraints.h"
#include "tbcad/constraintscmd/constraintscmd.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

// Test command: tilts the first line the way a grip drag would.
class Tilt final : public Command {
public:
    explicit Tilt(EntityId id) : id_(id) {}
    std::string_view name() const override { return "TILT"; }
    Task run(CommandContext& ctx) override {
        (void)ctx.tx().modify(id_, [](Entity& e) { entityAs<LineData>(e)->b = {10, 4}; });
        co_return;
    }

private:
    EntityId id_;
};

// Test command: erases the second line.
class Kill final : public Command {
public:
    explicit Kill(EntityId id) : id_(id) {}
    std::string_view name() const override { return "KILL"; }
    Task run(CommandContext& ctx) override {
        (void)ctx.tx().erase(id_);
        co_return;
    }

private:
    EntityId id_;
};

struct Fixture {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;
    EntityId l1{}, l2{};

    Fixture() {
        registerBuiltinCommands(reg);
        constraintscmd::registerCommands(reg);
        l1 = addLine({0, 0}, {10, 0});
        l2 = addLine({0, 5}, {10, 6.5});
        (void)reg.add(std::make_unique<Tilt>(l1));
        (void)reg.add(std::make_unique<Kill>(l2));
        host = std::make_unique<ScriptHost>(doc, reg);
    }
    EntityId addLine(Vec2 a, Vec2 b) {
        Transaction tx = doc.begin("setup");
        EntityId id = tx.add(Entity{{}, LineData{a, b}}).value();
        tx.commit();
        return id;
    }
    LineData line(EntityId id) { return *entityAs<LineData>(*doc.find(id)); }
    ScriptItem pick(EntityId id, Vec2 at) { return ScriptItem::pick(id, at); }
};

}  // namespace

TEST_CASE("GEOMCON Parallel makes the second line follow the first and is one undo step") {
    Fixture f;
    auto r = f.host->run("GEOMCON", {"Parallel", f.pick(f.l1, {5, 0}), f.pick(f.l2, {5, 5.7})});
    CHECK(r.finished);
    const LineData a = f.line(f.l1), b = f.line(f.l2);
    CHECK((a.b.x - a.a.x) * (b.b.y - b.a.y) - (a.b.y - a.a.y) * (b.b.x - b.a.x) ==
          doctest::Approx(0).epsilon(1e-6));
    CHECK(constraints::load(f.doc).all().size() == 1);

    CHECK(f.host->undo());
    CHECK(constraints::load(f.doc).empty());
    CHECK(f.line(f.l2).b.y == doctest::Approx(6.5));
    CHECK(f.host->redo());
    CHECK(constraints::load(f.doc).all().size() == 1);
}

TEST_CASE("editing a constrained line moves the other one along") {
    Fixture f;
    f.host->run("GEOMCON", {"Parallel", f.pick(f.l1, {5, 0}), f.pick(f.l2, {5, 5.7})});
    const double l1y = f.line(f.l1).b.y, l2y = f.line(f.l2).b.y;
    f.host->run("TILT", {});
    const LineData a = f.line(f.l1), b = f.line(f.l2);
    CHECK(a.b.y == 4);  // the edited line stays as the user left it
    CHECK((a.b.x - a.a.x) * (b.b.y - b.a.y) - (a.b.y - a.a.y) * (b.b.x - b.a.x) ==
          doctest::Approx(0).epsilon(1e-6));
    // One undo step undoes both.
    CHECK(f.host->undo());
    CHECK(f.line(f.l1).b.y == doctest::Approx(l1y));
    CHECK(f.line(f.l2).b.y == doctest::Approx(l2y));
}

TEST_CASE("DIMCON sets a length and a conflicting constraint is refused") {
    Fixture f;
    f.host->run("GEOMCON", {"Fix", f.pick(f.l1, {5, 0})});
    f.host->run("GEOMCON", {"Horizontal", f.pick(f.l2, {5, 5.7})});
    auto r = f.host->run("DIMCON", {"Length", f.pick(f.l2, {5, 5.7}), "20"});
    CHECK(r.finished);
    CHECK(length(f.line(f.l2).b - f.line(f.l2).a) == doctest::Approx(20));
    CHECK(f.line(f.l2).a.y == doctest::Approx(f.line(f.l2).b.y));

    const std::size_t before = constraints::load(f.doc).all().size();
    f.host->run("DIMCON", {"Length", f.pick(f.l1, {5, 0}), "30"});  // line 1 is fixed
    CHECK(constraints::load(f.doc).all().size() == before);
    CHECK(f.line(f.l1).b.x == 10);
}

TEST_CASE("DELCON removes the constraints of the selected objects - erased objects drop theirs") {
    Fixture f;
    f.host->run("GEOMCON", {"Parallel", f.pick(f.l1, {5, 0}), f.pick(f.l2, {5, 5.7})});
    f.host->run("DELCON", {ScriptItem::select({f.l2}), ScriptItem::enter()});
    CHECK(constraints::load(f.doc).empty());

    f.host->run("GEOMCON", {"Parallel", f.pick(f.l1, {5, 0}), f.pick(f.l2, {5, 5.7})});
    f.host->run("KILL", {});
    CHECK(constraints::load(f.doc).empty());
}

TEST_CASE("SHOWCON lists constraints") {
    Fixture f;
    f.host->run("SHOWCON", {});
    f.host->run("GEOMCON", {"Perpendicular", f.pick(f.l1, {5, 0}), f.pick(f.l2, {5, 5.7})});
    f.host->run("SHOWCON", {});
    bool found = false;
    for (const std::string& line : f.host->log())
        found = found || line.find("Perpendicular") != std::string::npos;
    CHECK(found);
}

TEST_CASE("constraints are saved in the .tbcad file and still work after reopening") {
    Fixture f;
    f.host->run("GEOMCON", {"Parallel", f.pick(f.l1, {5, 0}), f.pick(f.l2, {5, 5.7})});
    auto bytes = io::writeTbcadToMemory(f.doc);
    REQUIRE(bytes.ok());
    io::IoReport report;
    auto reopened = io::readTbcad(std::span<const std::uint8_t>(bytes.value()), report);
    REQUIRE(reopened.ok());
    Document& doc2 = *reopened.value();
    CHECK(constraints::load(doc2).all().size() == 1);

    Registry reg2;
    registerBuiltinCommands(reg2);
    constraintscmd::registerCommands(reg2);
    class Tilt2 final : public Command {
    public:
        explicit Tilt2(EntityId id) : id_(id) {}
        std::string_view name() const override { return "TILT"; }
        Task run(CommandContext& ctx) override {
            (void)ctx.tx().modify(id_, [](Entity& e) { entityAs<LineData>(e)->b = {10, -3}; });
            co_return;
        }
        EntityId id_;
    };
    (void)reg2.add(std::make_unique<Tilt2>(f.l1));
    ScriptHost host2(doc2, reg2);
    host2.run("TILT", {});
    const LineData a = *entityAs<LineData>(*doc2.find(f.l1));
    const LineData b = *entityAs<LineData>(*doc2.find(f.l2));
    CHECK((a.b.x - a.a.x) * (b.b.y - b.a.y) - (a.b.y - a.a.y) * (b.b.x - b.a.x) ==
          doctest::Approx(0).epsilon(1e-6));
}

TEST_CASE("SOLVECON brings objects back in line after they were changed behind the constraints") {
    Fixture f;
    f.host->run("GEOMCON", {"Parallel", f.pick(f.l1, {5, 0}), f.pick(f.l2, {5, 5.7})});
    {
        // Not a command, so nothing keeps the constraints in step.
        Transaction tx = f.doc.begin("edit");
        REQUIRE(tx.modify(f.l2, [](Entity& e) { entityAs<LineData>(e)->b = {10, 9}; }));
        tx.commit();
    }
    const LineData before = f.line(f.l2);
    auto cross = [&] {
        const LineData a = f.line(f.l1), b = f.line(f.l2);
        return (a.b.x - a.a.x) * (b.b.y - b.a.y) - (a.b.y - a.a.y) * (b.b.x - b.a.x);
    };
    if (std::abs(cross()) > 1e-6) {  // the edit was not followed
        REQUIRE(f.host->run("SOLVECON", {}).finished);
        CHECK(f.host->logContains("Solved"));
        CHECK(cross() == doctest::Approx(0).epsilon(1e-6));
        CHECK(f.host->undo());
        CHECK(f.line(f.l2).b.y == doctest::Approx(before.b.y));
    } else {
        REQUIRE(f.host->run("SOLVECON", {}).finished);
        CHECK(f.host->logContains("Solved"));
    }
}

TEST_CASE("AUTOCONSTRAIN adds what the sketch already satisfies, and one undo removes it") {
    Fixture f;
    // l2 is nearly parallel to l1 (1.5 off over 10) - not close enough for the defaults - so add
    // a tidy rectangle corner instead.
    EntityId a = f.addLine({100, 100}, {120, 100.0001});
    EntityId b = f.addLine({120, 100.0001}, {120.0001, 110});
    f.host->run("AUTOCONSTRAIN", {ScriptItem::select({a, b}), ScriptItem::enter()});
    const auto set = constraints::load(f.doc);
    bool horiz = false, vert = false, coinc = false;
    for (const auto& c : set.all()) {
        horiz = horiz || c.kind == constraints::Kind::Horizontal;
        vert = vert || c.kind == constraints::Kind::Vertical;
        coinc = coinc || c.kind == constraints::Kind::Coincident;
    }
    CHECK(horiz);
    CHECK(vert);
    CHECK(coinc);
    // The geometry was nudged onto the constraints.
    CHECK(f.line(a).a.y == doctest::Approx(f.line(a).b.y).epsilon(1e-9));
    CHECK(f.line(b).a.x == doctest::Approx(f.line(b).b.x).epsilon(1e-9));
    f.host->undo();
    CHECK(constraints::load(f.doc).empty());
    // Nothing to add on a second pass.
    f.host->run("AUTOCONSTRAIN", {ScriptItem::select({a, b}), ScriptItem::enter()});
    const std::size_t n = constraints::load(f.doc).all().size();
    f.host->run("AUTOCONSTRAIN", {ScriptItem::select({a, b}), ScriptItem::enter()});
    CHECK(constraints::load(f.doc).all().size() == n);
    CHECK(f.host->logContains("No new constraints"));
}

TEST_CASE("PARAMETERS drive a DIMCON formula; editing a parameter moves the geometry; saved") {
    Fixture f;
    f.host->run("GEOMCON", {"Fix", f.pick(f.l1, {5, 0})});
    f.host->run("GEOMCON", {"Horizontal", f.pick(f.l2, {5, 5.7})});
    f.host->run("PARAMETERS", {"Set", "Width", "20", "Set", "Half", "Width / 2", "List", "Exit"});
    CHECK(f.host->logContains("Half = 10"));
    f.host->run("DIMCON", {"Length", f.pick(f.l2, {5, 5.7}), "Formula", "Half * 3"});
    CHECK(length(f.line(f.l2).b - f.line(f.l2).a) == doctest::Approx(30));
    // Changing the parameter changes the line.
    f.host->run("PARAMETERS", {"Set", "Width", "40", "Exit"});
    CHECK(length(f.line(f.l2).b - f.line(f.l2).a) == doctest::Approx(60));
    const auto set = constraints::load(f.doc);
    bool formula = false;
    for (const auto& c : set.all())
        formula = formula || c.expr == "Half * 3";
    CHECK(formula);
    // A value the constraints cannot follow is refused and nothing changes.
    f.host->run("PARAMETERS", {"Set", "Width", "0 - 8", "Exit"});
    CHECK(length(f.line(f.l2).b - f.line(f.l2).a) == doctest::Approx(60));
    CHECK(constraints::loadParameters(f.doc).find("Width")->expr == "40");
    // A cycle is refused.
    f.host->run("PARAMETERS", {"Set", "Width", "Half", "Exit"});
    CHECK(constraints::loadParameters(f.doc).find("Width")->expr == "40");
    // Deleting a parameter that a constraint uses is refused.
    f.host->run("PARAMETERS", {"Delete", "Half", "Exit"});
    CHECK(constraints::loadParameters(f.doc).find("Half") != nullptr);

    auto bytes = io::writeTbcadToMemory(f.doc);
    REQUIRE(bytes.ok());
    io::IoReport report;
    auto reopened = io::readTbcad(std::span<const std::uint8_t>(bytes.value()), report);
    REQUIRE(reopened.ok());
    const auto params = constraints::loadParameters(*reopened.value());
    CHECK(params.all().size() == 2);
    CHECK(params.values().value().at("Half") == doctest::Approx(20));
    bool kept = false;
    for (const auto& c : constraints::load(*reopened.value()).all())
        kept = kept || c.expr == "Half * 3";
    CHECK(kept);
}

TEST_CASE("GEOMCON on a polyline uses the segment next to the click; DIMCON Length takes it too") {
    Fixture f;
    EntityId pl;
    {
        Transaction tx = f.doc.begin("polyline");
        PolylineData p;
        for (Vec2 v : {Vec2{0, 20}, Vec2{10, 22}, Vec2{12, 30}})
            p.v.push_back(PolyVertex{v});
        pl = tx.add(Entity{{}, std::move(p)}).value();
        tx.commit();
    }
    // the second segment (10,22)-(12,30) is picked near its middle and made vertical
    auto r = f.host->run("GEOMCON", {"Vertical", f.pick(pl, {11, 26})});
    CHECK(r.finished);
    const auto* p = entityAs<PolylineData>(*f.doc.find(pl));
    REQUIRE(p);
    CHECK(p->v[1].p.x == doctest::Approx(p->v[2].p.x).epsilon(1e-6));
    const auto set = constraints::load(f.doc);
    REQUIRE(set.all().size() == 1);
    CHECK(set.all()[0].a.index == 1);

    auto r2 = f.host->run("DIMCON", {"Length", f.pick(pl, {5, 21}), "12"});
    CHECK(r2.finished);
    const auto* p2 = entityAs<PolylineData>(*f.doc.find(pl));
    CHECK(length(p2->v[1].p - p2->v[0].p) == doctest::Approx(12).epsilon(1e-6));
}

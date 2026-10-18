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

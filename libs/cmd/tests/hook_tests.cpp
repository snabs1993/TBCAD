#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

// Moves the first line's end point to the typed point.
class MoveEnd final : public Command {
public:
    std::string_view name() const override { return "MOVEEND"; }
    Task run(CommandContext& ctx) override {
        auto p = co_await ctx.getPoint("Point");
        if (!p.ok())
            co_return;
        EntityId id = ctx.doc().modelSpace().front();
        (void)ctx.tx().modify(id, [&](Entity& e) { entityAs<LineData>(e)->b = p.point; });
        auto q = co_await ctx.getPoint("Wait");  // so that Esc can come after the edit
        (void)q;
    }
};

struct Fixture {
    Document doc;
    Registry reg;
    EntityId line, follower;
    int hookCalls = 0;
    std::vector<EntityId> seen;

    Fixture() {
        registerBuiltinCommands(reg);
        (void)reg.add(std::make_unique<MoveEnd>());
        Transaction tx = doc.begin("setup");
        line = tx.add(Entity{{}, LineData{{0, 0}, {10, 0}}}).value();
        follower = tx.add(Entity{{}, PointData{{10, 0}}}).value();
        tx.commit();
        // A dependent object: the point sits at the line's end.
        reg.addCommitHook([this](Document& d, Transaction& tx) {
            ++hookCalls;
            for (EntityId id : tx.touchedEntities()) {
                seen.push_back(id);
                if (id != line)
                    continue;
                Vec2 end = entityAs<LineData>(*d.find(line))->b;
                (void)tx.modify(follower, [&](Entity& e) { entityAs<PointData>(e)->p = end; });
            }
        });
    }
};

}  // namespace

TEST_CASE("a commit hook edits inside the command's transaction: one undo step") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("MOVEEND", {"20,5", ""});
    CHECK(f.hookCalls >= 1);
    CHECK(entityAs<PointData>(*f.doc.find(f.follower))->p == Vec2{20, 5});
    CHECK(host.undo());  // one step undoes the line and the follower together
    CHECK(entityAs<LineData>(*f.doc.find(f.line))->b == Vec2{10, 0});
    CHECK(entityAs<PointData>(*f.doc.find(f.follower))->p == Vec2{10, 0});
    CHECK(host.redo());
    CHECK(entityAs<PointData>(*f.doc.find(f.follower))->p == Vec2{20, 5});
}

TEST_CASE("hooks do not run for a cancelled command") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("MOVEEND", {"20,5", ScriptItem::esc()});
    CHECK(f.hookCalls == 0);
    CHECK(entityAs<PointData>(*f.doc.find(f.follower))->p == Vec2{10, 0});
}

TEST_CASE("the hook sees nothing for a command that touches nothing") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("MOVEEND", {ScriptItem::esc()});
    CHECK(f.seen.empty());
}

TEST_CASE("entitiesInWindow: no index by default, the test hook answers when set") {
    Document doc;
    Registry reg;
    ScriptHost host(doc, reg);
    BBox2 w;
    w.expand({0, 0});
    w.expand({1, 1});
    CHECK_FALSE(host.entitiesInWindow(w).has_value());
    host.setWindowQuery([](const BBox2&) { return std::vector<EntityId>{EntityId{7}}; });
    auto r = host.entitiesInWindow(w);
    REQUIRE(r.has_value());
    CHECK(*r == std::vector<EntityId>{EntityId{7}});
}

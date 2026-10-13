#include <doctest/doctest.h>

#include <cmath>
#include <stdexcept>

#include "tbcad/cmd/cmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

Entity lineEntity(Vec2 a, Vec2 b) {
    return Entity{{}, LineData{a, b}};
}

std::vector<LineData> lines(const Document& doc) {
    std::vector<LineData> out;
    for (EntityId id : doc.modelSpace()) {
        if (const auto* l = entityAs<LineData>(*doc.find(id)))
            out.push_back(*l);
    }
    return out;
}

bool near(Vec2 a, Vec2 b) {
    return distance(a, b) < 1e-9;
}

// Asks for two points and adds a line.
class TwoPointLine final : public Command {
public:
    std::string_view name() const override { return "TWOLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"TL"}; }
    Task run(CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("First point");
        if (!a.ok())
            co_return;
        PointOptions o;
        o.base = a.point;
        o.preview = [from = a.point](Vec2 cur, DisplayList& out) {
            DisplayPolyline pl;
            pl.pts = {from, cur};
            out.items.emplace_back(std::move(pl));
        };
        auto b = co_await ctx.getPoint("Second point", std::move(o));
        if (!b.ok())
            co_return;
        (void)ctx.tx().add(lineEntity(a.point, b.point));
    }
};

// LINE-like: chained segments, Undo keyword via checkpoints, Esc keeps what was drawn.
class ChainLine final : public Command {
public:
    std::string_view name() const override { return "CHAIN"; }
    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        auto first = co_await ctx.getPoint("First point");
        if (!first.ok())
            co_return;
        std::vector<Vec2> pts{first.point};
        for (;;) {
            PointOptions o;
            o.base = pts.back();
            if (pts.size() > 1)
                o.keywords = {"Undo"};
            auto r = co_await ctx.getPoint("Next point", std::move(o));
            if (r.status == Status::Keyword && r.keyword == "Undo") {
                ctx.rollbackToCheckpoint();
                pts.pop_back();
                continue;
            }
            if (!r.ok())
                co_return;
            ctx.checkpoint();
            (void)ctx.tx().add(lineEntity(pts.back(), r.point));
            pts.push_back(r.point);
        }
    }
};

// Adds a line first, then asks: Esc must discard the line.
class AddThenAsk final : public Command {
public:
    std::string_view name() const override { return "ADDTHENASK"; }
    Task run(CommandContext& ctx) override {
        (void)ctx.tx().add(lineEntity({0, 0}, {1, 1}));
        (void)co_await ctx.getPoint("More?");
    }
};

class Thrower final : public Command {
public:
    std::string_view name() const override { return "THROWER"; }
    Task run(CommandContext& ctx) override {
        (void)ctx.tx().add(lineEntity({0, 0}, {1, 1}));
        (void)co_await ctx.getPoint("Point");
        throw std::runtime_error("boom");
    }
};

// Keeps asking after Esc: must still terminate.
class Stubborn final : public Command {
public:
    std::string_view name() const override { return "STUBBORN"; }
    Task run(CommandContext& ctx) override {
        int asked = 0;
        for (int i = 0; i < 5; ++i) {
            auto r = co_await ctx.getPoint("Again");
            if (r.status == Status::Cancel)
                break;
            ++asked;
        }
        ctx.message("asked " + std::to_string(asked));
    }
};

Task helper(CommandContext& ctx, Vec2& out) {
    auto r = co_await ctx.getPoint("From helper");
    out = r.point;
}

class UsesHelper final : public Command {
public:
    std::string_view name() const override { return "HELPER"; }
    Task run(CommandContext& ctx) override {
        Vec2 p{};
        co_await helper(ctx, p);
        ctx.message("got " + formatDouble(p.x));
    }
};

// Records every kind of input into the log.
class Everything final : public Command {
public:
    std::string_view name() const override { return "EVERYTHING"; }
    Task run(CommandContext& ctx) override {
        DistanceOptions d;
        d.base = Vec2{0, 0};
        auto dist = co_await ctx.getDistance("Distance", d);
        ctx.message("dist " + formatDouble(dist.value));
        auto ang = co_await ctx.getAngle("Angle", AngleOptions{.base = Vec2{0, 0}});
        ctx.message("ang " + formatDouble(ang.value));
        auto n =
            co_await ctx.getInteger("Count", IntOptions{.defaultValue = 4, .min = 1, .max = 10});
        ctx.message("n " + std::to_string(n.value) + " st " + std::to_string(int(n.status)));
        auto x = co_await ctx.getReal("Real", RealOptions{.defaultValue = 2.5});
        ctx.message("real " + formatDouble(x.value));
        auto s = co_await ctx.getString("Name", StringOptions{});
        ctx.message("str " + s.text + " st " + std::to_string(int(s.status)));
        auto k = co_await ctx.getKeyword("Mode", KeywordOptions{{"Fast", "Slow"}, "Slow"});
        ctx.message("kw " + k.keyword);
    }
};

class PickCmd final : public Command {
public:
    std::string_view name() const override { return "PICKCMD"; }
    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects", SelectionOptions{});
        ctx.message("sel " + std::to_string(sel.ids.size()) + " st " +
                    std::to_string(int(sel.status)));
        auto e = co_await ctx.getEntity(
            "Pick a line",
            EntityOptions{.filter = [](const Entity& en) { return kind(en) == EntityKind::Line; }});
        ctx.message("entity " + std::to_string(e.id.value) + " st " +
                    std::to_string(int(e.status)));
    }
};

class ZoomT final : public Command {
public:
    std::string_view name() const override { return "ZOOMT"; }
    bool isTransparent() const override { return true; }
    Task run(CommandContext& ctx) override {
        ctx.host().setSetting("zoomed", "yes");
        // The shared transaction: this edit belongs to the parent command.
        (void)ctx.tx().add(lineEntity({5, 5}, {6, 6}));
        co_return;
    }
};

class WaitsForPoint final : public Command {
public:
    std::string_view name() const override { return "WAITER"; }
    Task run(CommandContext& ctx) override {
        auto r = co_await ctx.getPoint("Point");
        if (r.ok())
            (void)ctx.tx().add(lineEntity({0, 0}, r.point));
    }
};

class KeywordCmd final : public Command {
public:
    std::string_view name() const override { return "KW"; }
    Task run(CommandContext& ctx) override {
        auto r =
            co_await ctx.getPoint("Point", PointOptions{.keywords = {"Close", "Cancelx", "Undo"}});
        ctx.message("status " + std::to_string(int(r.status)) + " kw " + r.keyword);
    }
};

struct Fixture {
    Document doc;
    Registry reg;
    Fixture() {
        registerBuiltinCommands(reg);
        (void)reg.add(std::make_unique<TwoPointLine>());
        (void)reg.add(std::make_unique<ChainLine>());
        (void)reg.add(std::make_unique<AddThenAsk>());
        (void)reg.add(std::make_unique<Thrower>());
        (void)reg.add(std::make_unique<Stubborn>());
        (void)reg.add(std::make_unique<UsesHelper>());
        (void)reg.add(std::make_unique<Everything>());
        (void)reg.add(std::make_unique<PickCmd>());
        (void)reg.add(std::make_unique<ZoomT>());
        (void)reg.add(std::make_unique<WaitsForPoint>());
        (void)reg.add(std::make_unique<KeywordCmd>());
    }
};

}  // namespace

TEST_CASE("two-point command creates the line; undo removes it; redo restores it") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    auto r = host.run("TWOLINE", {"0,0", "@100,0"});
    CHECK(r.started);
    CHECK(r.finished);
    CHECK(r.consumed == 2);
    REQUIRE(lines(f.doc).size() == 1);
    CHECK(near(lines(f.doc)[0].a, {0, 0}));
    CHECK(near(lines(f.doc)[0].b, {100, 0}));
    CHECK(host.undoStack().undoLabel() == "TWOLINE");

    CHECK(host.undo());
    CHECK(lines(f.doc).empty());
    CHECK(host.undoStack().canRedo());
    CHECK(host.redo());
    REQUIRE(lines(f.doc).size() == 1);
    CHECK(near(lines(f.doc)[0].b, {100, 0}));
}

TEST_CASE("Esc in the middle leaves the document unchanged and records no undo step") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    auto r = host.run("TWOLINE", {"0,0", ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.doc.entityCount() == 0);
    CHECK_FALSE(host.undoStack().canUndo());
    CHECK_FALSE(host.runner().active());

    // Esc after the command already changed the document discards those changes too.
    auto r2 = host.run("ADDTHENASK", {ScriptItem::esc()});
    CHECK(r2.finished);
    CHECK(f.doc.entityCount() == 0);
    CHECK_FALSE(host.undoStack().canUndo());
}

TEST_CASE("an exception rolls the command back and is reported") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    auto r = host.run("THROWER", {"1,1"});
    CHECK(r.finished);
    CHECK(f.doc.entityCount() == 0);
    CHECK(host.logContains("THROWER failed: boom"));
    CHECK_FALSE(host.undoStack().canUndo());
}

TEST_CASE("checkpoint rollback inside a running command") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    auto r = host.run("CHAIN", {"0,0", "10,0", "10,10", "20,10"});
    CHECK_FALSE(r.finished);
    CHECK(lines(f.doc).size() == 3);

    // Undo twice: back to one segment.
    host.feed({"U", "U"});
    REQUIRE(lines(f.doc).size() == 1);
    CHECK(near(lines(f.doc)[0].a, {0, 0}));
    CHECK(near(lines(f.doc)[0].b, {10, 0}));
    CHECK(host.runner().active());

    host.feed({"30,0", ScriptItem::enter()});
    CHECK_FALSE(host.runner().active());
    auto ls = lines(f.doc);
    REQUIRE(ls.size() == 2);
    CHECK(near(ls[1].a, {10, 0}));
    CHECK(near(ls[1].b, {30, 0}));

    // The whole command is one undo step.
    CHECK(host.undoStack().undoCount() == 1);
    CHECK(host.undo());
    CHECK(lines(f.doc).empty());
    CHECK(host.redo());
    CHECK(lines(f.doc).size() == 2);
}

TEST_CASE("Esc in a commitOnCancel command keeps the finished segments") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("CHAIN", {"0,0", "5,0", "5,5", ScriptItem::esc()});
    CHECK_FALSE(host.runner().active());
    CHECK(lines(f.doc).size() == 2);
    CHECK(host.undoStack().undoCount() == 1);
    host.undo();
    CHECK(lines(f.doc).empty());
}

TEST_CASE("a command that keeps asking after Esc still terminates") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    auto r = host.run("STUBBORN", {"1,1", ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(host.logContains("asked 1"));
}

TEST_CASE("sub-task coroutines can be awaited") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    auto r = host.run("HELPER", {"7,0"});
    CHECK(r.finished);
    CHECK(host.logContains("got 7"));
}

TEST_CASE("keywords: exact, unique prefix, ambiguous prefix is refused") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("KW", {"cl"});
    CHECK(host.logContains("status 2 kw Close"));  // Keyword = 2
    host.clearLog();
    auto r = host.run("KW", {"c", "undo"});  // "c" is ambiguous (Close/Cancelx)
    CHECK(r.rejected == 1);
    CHECK(host.logContains("kw Undo"));
}

TEST_CASE("typed input grammar and rejection of bad input") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    // Rejected input keeps the prompt alive.
    auto r = host.run("TWOLINE", {"banana", "10,10", "@5<90"});
    CHECK(r.rejected == 1);
    CHECK(r.finished);
    REQUIRE(lines(f.doc).size() == 1);
    CHECK(near(lines(f.doc)[0].a, {10, 10}));
    CHECK(near(lines(f.doc)[0].b, {10, 15}));

    // "@dx,dy" is relative to the last point that was entered.
    host.run("TWOLINE", {"100,100", "@-50,25"});
    REQUIRE(lines(f.doc).size() == 2);
    CHECK(near(lines(f.doc)[1].b, {50, 125}));

    // Units in typed coordinates.
    host.run("TWOLINE", {"0,0", "1m,0"});
    CHECK(near(lines(f.doc)[2].b, {1000, 0}));
}

TEST_CASE("typed values for distance, angle, integer, real, string, keyword") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    auto r = host.run("EVERYTHING", {"12.5", "90", "99", "7", "", "Hello world", "fast"});
    CHECK(r.finished);
    CHECK(r.rejected == 1);  // 99 is out of range
    CHECK(host.logContains("dist 12.5"));
    CHECK(host.logContains("ang " + formatDouble(kPi / 2)));
    CHECK(host.logContains("n 7 st 0"));
    CHECK(host.logContains("real 2.5"));  // Enter takes the default
    CHECK(host.logContains("str Hello world st 0"));
    CHECK(host.logContains("kw Fast"));
}

TEST_CASE("Enter takes defaults; clicks measure distance and angle from the base") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("EVERYTHING", {ScriptItem::click({3, 4}), ScriptItem::click({0, 2}), "", "", "x", ""});
    CHECK(host.logContains("dist 5"));
    CHECK(host.logContains("ang " + formatDouble(kPi / 2)));
    CHECK(host.logContains("n 4 st 0"));
    CHECK(host.logContains("kw Slow"));
}

TEST_CASE("selection: pre-selection, picks, entity filter") {
    Fixture f;
    EntityId l1, l2, c1;
    {
        Transaction tx = f.doc.begin("setup");
        l1 = tx.add(lineEntity({0, 0}, {1, 0})).value();
        l2 = tx.add(lineEntity({0, 1}, {1, 1})).value();
        c1 = tx.add(Entity{{}, CircleData{{0, 0}, 1}}).value();
        tx.commit();
    }
    ScriptHost host(f.doc, f.reg);

    // Pre-selection answers without prompting.
    host.setSelection({l1, l2});
    auto r = host.run("PICKCMD", {ScriptItem::pick(c1), ScriptItem::pick(l2)});
    CHECK(host.logContains("sel 2 st 0"));
    // c1 is not a line: rejected; l2 accepted.
    CHECK(r.rejected == 1);
    CHECK(host.logContains("entity " + std::to_string(l2.value) + " st 0"));

    // No pre-selection: picks accumulate until Enter.
    host.clearLog();
    host.setSelection({});
    host.run("PICKCMD", {ScriptItem::pick(l1), ScriptItem::select({l2, c1}), ScriptItem::enter(),
                         ScriptItem::esc()});
    CHECK(host.logContains("sel 3 st 0"));
    CHECK(host.logContains("st 1"));  // getEntity cancelled
}

TEST_CASE("transparent commands run inside a prompt and share the transaction") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("WAITER", {});
    REQUIRE(host.runner().active());
    host.feed({"'zoomt"});
    CHECK(host.setting("zoomed") == std::optional<std::string>("yes"));
    CHECK(host.runner().active());  // the parent still waits for its point
    host.feed({"3,3"});
    CHECK_FALSE(host.runner().active());
    CHECK(lines(f.doc).size() == 2);  // the transparent command's line is part of the parent step
    CHECK(host.undoStack().undoCount() == 1);
    host.undo();
    CHECK(f.doc.entityCount() == 0);

    // A non-transparent command cannot be nested.
    host.run("WAITER", {});
    auto r = host.feed({"'twoline"});
    CHECK(r.rejected == 1);
    host.feed({ScriptItem::esc()});
}

TEST_CASE("repeat last command on Enter, aliases at the idle prompt, unknown commands") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("tl", {"0,0", "1,0"});
    CHECK(host.runner().lastCommand() == "TWOLINE");

    CHECK(host.runner().submit(Input::enter()) == SubmitResult::CommandStarted);
    host.feed({"0,5", "1,5"});
    CHECK(lines(f.doc).size() == 2);

    CHECK(host.runner().submit(Input::typed("nosuchcommand")) == SubmitResult::UnknownCommand);
    CHECK(host.logContains("Unknown command"));
    // A command typed at the idle prompt.
    CHECK(host.runner().submit(Input::typed("TL")) == SubmitResult::CommandStarted);
    host.runner().submit(Input::cancel());
    CHECK_FALSE(host.runner().active());
}

TEST_CASE("UNDO/REDO commands and undo groups") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("TWOLINE", {"0,0", "1,0"});
    host.run("UNDOBEGIN", {});
    host.run("TWOLINE", {"0,1", "1,1"});
    host.run("TWOLINE", {"0,2", "1,2"});
    host.run("UNDOEND", {});
    CHECK(lines(f.doc).size() == 3);
    CHECK(host.undoStack().undoCount() == 2);  // the two grouped commands are one step

    host.run("U", {});
    CHECK(lines(f.doc).size() == 1);
    host.run("REDO", {});
    CHECK(lines(f.doc).size() == 3);
    host.run("UNDO", {});
    host.run("UNDO", {});
    host.run("UNDO", {});
    CHECK(host.logContains("Nothing to undo."));
    CHECK(f.doc.entityCount() == 0);
}

TEST_CASE("undo is refused while a command runs") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("TWOLINE", {"0,0", "1,0"});
    host.run("TWOLINE", {"0,0"});
    CHECK_FALSE(host.undo());
    host.feed({ScriptItem::esc()});
    CHECK(host.undo());
}

namespace {

// Snaps every point to a 10 mm grid.
struct GridSnap : PointFilter {
    std::optional<Vec2> filter(const PointQuery&, Vec2 raw, FilterFeedback& fb) override {
        fb.marker = "grid";
        return Vec2{std::round(raw.x / 10) * 10, std::round(raw.y / 10) * 10};
    }
};
struct SnapToOrigin : PointFilter {
    std::optional<Vec2> filter(const PointQuery&, Vec2 raw, FilterFeedback& fb) override {
        if (distance(raw, {0, 0}) > 3)
            return std::nullopt;
        fb.marker = "origin";
        return Vec2{0, 0};
    }
};
struct DirectDistance : CommandLineParser {
    std::optional<Vec2> parsePoint(std::string_view text, const PointParseContext& ctx) override {
        if (text == "dd" && ctx.base)
            return *ctx.base + Vec2{42, 0};
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("point filters: priority order, preview gets the filtered point") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    GridSnap grid;
    SnapToOrigin osnap;
    host.filters().add(&grid, 20);
    host.filters().add(&osnap, 10);  // lower order wins when it applies

    host.run("TWOLINE", {
                            ScriptItem::click({2, 1}),
                        });
    host.runner().pointerMoved(RawInput{{23, 18}, {}, 1});
    REQUIRE(host.preview().items.size() == 1);
    const auto& pl = std::get<DisplayPolyline>(host.preview().items[0]);
    CHECK(near(pl.pts[0], {0, 0}));    // clicked near the origin: osnap won over the grid
    CHECK(near(pl.pts[1], {20, 20}));  // far from the origin: grid
    CHECK(host.runner().feedback().marker == "grid");

    host.feed({ScriptItem::click({41, 39})});
    REQUIRE(lines(f.doc).size() == 1);
    CHECK(near(lines(f.doc)[0].b, {40, 40}));
}

TEST_CASE("a CommandLineParser hook extends typed points") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    DirectDistance dd;
    host.runner().setLineParser(&dd);
    host.run("TWOLINE", {"1,1", "dd"});
    REQUIRE(lines(f.doc).size() == 1);
    CHECK(near(lines(f.doc)[0].b, {43, 1}));
}

TEST_CASE("prompt text shows keywords and defaults") {
    Fixture f;
    ScriptHost host(f.doc, f.reg);
    host.run("CHAIN", {"0,0", "1,0"});
    REQUIRE(host.prompts().size() >= 3);
    CHECK(host.prompts()[0] == "First point: ");
    CHECK(host.prompts().back() == "Next point or [Undo]: ");
    host.feed({ScriptItem::esc()});
}

TEST_CASE("a hook that observes the document sees checkpoint steps and the final commit") {
    struct Obs : DocumentObserver {
        int calls = 0;
        void onChanged(const ChangeSet&) override { ++calls; }
    } obs;
    Fixture f;
    f.doc.addObserver(&obs);
    ScriptHost host(f.doc, f.reg);
    host.run("CHAIN", {"0,0", "1,0", "2,0", ScriptItem::enter()});
    CHECK(obs.calls >= 2);
    f.doc.removeObserver(&obs);
}

namespace {
class LastPointProbe final : public Command {
public:
    std::string_view name() const override { return "LASTPOINTPROBE"; }
    Task run(CommandContext& ctx) override {
        DistanceOptions o;
        o.base = Vec2{0, 0};
        auto d = co_await ctx.getDistance("Distance", o);
        if (!d.ok())
            co_return;
        ctx.message("last=" + std::to_string(ctx.lastPoint().x) + "," +
                    std::to_string(ctx.lastPoint().y));
    }
};
}  // namespace

TEST_CASE("CommandContext::lastPoint is the click that answered a distance prompt") {
    Document doc;
    Registry reg;
    (void)reg.add(std::make_unique<LastPointProbe>());
    ScriptHost host(doc, reg);
    host.run("LASTPOINTPROBE", {ScriptItem::click({3, 4})});
    CHECK(host.logContains("last=3.000000,4.000000"));
}

TEST_CASE("a prompt that spells out its keywords and default is not shown twice") {
    cmd::PromptInfo p;
    p.text = "Reference type [Attach/Overlay] <Attach>";
    p.keywords = {"Attach", "Overlay"};
    p.defaultText = "Attach";
    CHECK(p.display() == "Reference type [Attach/Overlay] <Attach>: ");

    cmd::PromptInfo q;
    q.text = "Reference type";
    q.keywords = {"Attach", "Overlay"};
    q.defaultText = "Attach";
    CHECK(q.display() == "Reference type or [Attach/Overlay] <Attach>: ");
}

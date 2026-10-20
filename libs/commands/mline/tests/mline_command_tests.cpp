#include <doctest/doctest.h>

#include <cmath>
#include <set>
#include <variant>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/geom/geom.h"
#include "tbcad/mline/mline.h"
#include "tbcad/mlinecmd/mlinecmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

struct Fx {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;
    Fx() {
        registerBuiltinCommands(reg);
        mlinecmd::registerCommands(reg);
        host = std::make_unique<ScriptHost>(doc, reg);
    }
    const MlineData* mline() const {
        const MlineData* m = nullptr;
        doc.forEachEntity([&](const Entity& e) {
            if (kind(e) == EntityKind::MLine)
                m = entityAs<MlineData>(e);
        });
        return m;
    }
};

}  // namespace

TEST_CASE("MLINE draws one multiline through the points and undoes in one step") {
    Fx f;
    CHECK(f.reg.find("ML") == f.reg.find("MLINE"));
    auto r = f.host->run("MLINE", {"0,0", "100,0", "100,50", ""});
    CHECK(r.finished);
    const MlineData* m = f.mline();
    REQUIRE(m);
    CHECK(m->v.size() == 3);
    CHECK_FALSE(m->closed);
    CHECK(m->style == "STANDARD");
    f.host->undo();
    CHECK(f.mline() == nullptr);
}

TEST_CASE("MLINE Close closes the figure") {
    Fx f;
    f.host->run("MLINE", {"0,0", "100,0", "100,50", "Close"});
    const MlineData* m = f.mline();
    REQUIRE(m);
    CHECK(m->closed);
}

TEST_CASE("MLINE options set justification and scale") {
    Fx f;
    f.host->run("MLINE", {"Justification", "Top", "Scale", "2", "0,0", "10,0", ""});
    const MlineData* m = f.mline();
    REQUIRE(m);
    CHECK(m->justification == 0);
    CHECK(m->scale == doctest::Approx(2));
}

TEST_CASE("MLSTYLE creates a style that MLINE can use, and deletes it again") {
    Fx f;
    f.host->run("-MLSTYLE", {"New", "WALL", "1 0 -1", "Both"});
    auto s = mline::findStyle(f.doc, "wall");
    REQUIRE(s);
    CHECK(s->offsets.size() == 3);
    CHECK(s->startCap);
    CHECK(s->endCap);
    f.host->run("-MLSTYLE", {"Current", "WALL"});
    f.host->run("MLINE", {"0,0", "10,0", ""});
    REQUIRE(f.mline());
    CHECK(f.mline()->style == "WALL");
    f.host->run("-MLSTYLE", {"Delete", "WALL"});
    CHECK(mline::findStyle(f.doc, "WALL").has_value());  // still used
}

TEST_CASE("Exploding a multiline gives polylines") {
    Fx f;
    f.host->run("MLINE", {"0,0", "100,0", ""});
    REQUIRE(f.mline());
    int n = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        auto r = mline::explodeMline(f.doc, e);
        REQUIRE(r);
        n = static_cast<int>(r.value().size());
    });
    CHECK(n == 2);
}

TEST_CASE("MLEDIT joins two walls at their corner and adds and deletes vertices") {
    Fx f;
    // Two walls that stop short of each other: one along X to (90,0), one along Y from (100,10).
    REQUIRE(f.host->run("MLINE", {"0,0", "90,0", ""}).finished);
    REQUIRE(f.host->run("MLINE", {"100,10", "100,100", ""}).finished);
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::MLine)
            ids.push_back(e.common.id);
    });
    REQUIRE(ids.size() == 2);
    // The first drawn is ids[0] (storage order). Pick each near the ends that should meet.
    EntityId wallX = ids[0], wallY = ids[1];
    f.host->run("MLEDIT",
                {"Corner", ScriptItem::pick(wallX, {85, 0}), ScriptItem::pick(wallY, {100, 15})});
    const auto* x = entityAs<MlineData>(*f.doc.find(wallX));
    const auto* y = entityAs<MlineData>(*f.doc.find(wallY));
    CHECK(x->v.back().x == doctest::Approx(100));
    CHECK(x->v.back().y == doctest::Approx(0));
    CHECK(y->v.front().x == doctest::Approx(100));
    CHECK(y->v.front().y == doctest::Approx(0));
    CHECK(f.host->undo());
    CHECK(entityAs<MlineData>(*f.doc.find(wallX))->v.back().x == doctest::Approx(90));

    f.host->run("MLEDIT", {"Add", ScriptItem::pick(wallY, {100, 50}), "100,50"});
    CHECK(entityAs<MlineData>(*f.doc.find(wallY))->v.size() == 3);
    f.host->run("MLEDIT", {"Delete", ScriptItem::pick(wallY, {100, 52})});
    CHECK(entityAs<MlineData>(*f.doc.find(wallY))->v.size() == 2);
    f.host->run("MLEDIT", {"Delete", ScriptItem::pick(wallY, {100, 52})});
    CHECK(entityAs<MlineData>(*f.doc.find(wallY))->v.size() == 2);  // at least two stay
}

namespace {
std::vector<EntityId> mlineIds(const Document& doc) {
    std::vector<EntityId> ids;
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::MLine)
            ids.push_back(e.common.id);
    });
    return ids;
}
}  // namespace

TEST_CASE("MLEDIT Cut opens a gap in a wall and Weld closes it again") {
    Fx f;
    REQUIRE(f.host->run("MLINE", {"0,0", "100,0", ""}).finished);
    const EntityId w = mlineIds(f.doc)[0];
    const auto strokesBefore =
        mline::strokes(*entityAs<MlineData>(*f.doc.find(w)), mline::standardStyle());
    f.host->run("MLEDIT", {"Cut", ScriptItem::pick(w, {40, 0}), "60,0", "All"});
    const auto* m = entityAs<MlineData>(*f.doc.find(w));
    REQUIRE(m->cuts.size() == 1);
    CHECK(m->cuts[0].t0 == doctest::Approx(0.4));
    CHECK(m->cuts[0].t1 == doctest::Approx(0.6));
    const auto cutStrokes = mline::strokes(*m, mline::standardStyle());
    CHECK(cutStrokes.lines.size() >= strokesBefore.lines.size());
    CHECK(f.host->undo());
    CHECK(entityAs<MlineData>(*f.doc.find(w))->cuts.empty());
    f.host->run("MLEDIT", {"Cut", ScriptItem::pick(w, {40, 0}), "60,0", "All"});
    f.host->run("MLEDIT", {"Weld", ScriptItem::pick(w, {45, 0}), "55,0"});
    CHECK(entityAs<MlineData>(*f.doc.find(w))->cuts.empty());
}

TEST_CASE("MLEDIT Cross and Tee cut the walls where they meet") {
    Fx f;
    REQUIRE(f.host->run("MLINE", {"0,0", "100,0", ""}).finished);
    REQUIRE(f.host->run("MLINE", {"50,-50", "50,50", ""}).finished);
    auto ids = mlineIds(f.doc);
    REQUIRE(ids.size() == 2);
    f.host->run("MLEDIT", {"Cross", "Closed", ScriptItem::pick(ids[0], {10, 0}),
                           ScriptItem::pick(ids[1], {50, 30})});
    CHECK(entityAs<MlineData>(*f.doc.find(ids[0]))->cuts.size() == 1);
    CHECK(entityAs<MlineData>(*f.doc.find(ids[1]))->cuts.empty());
    CHECK(f.host->undo());

    // Tee: a wall ending short of another one is brought to its near edge.
    REQUIRE(f.host->run("MLINE", {"20,40", "20,10", ""}).finished);
    ids = mlineIds(f.doc);
    REQUIRE(ids.size() == 3);
    f.host->run("MLEDIT", {"Tee", "Closed", ScriptItem::pick(ids[2], {20, 12}),
                           ScriptItem::pick(ids[0], {60, 0})});
    const auto* t = entityAs<MlineData>(*f.doc.find(ids[2]));
    CHECK(t->v.back().x == doctest::Approx(20));
    CHECK(t->v.back().y > 0);
    CHECK(t->v.back().y < 10);
}

TEST_CASE("MLSTYLE gives each line a colour, and the display and explode use them") {
    Fx f;
    f.host->run("-MLSTYLE", {"New", "TWO", "0.5 -0.5", "None"});
    f.host->run("-MLSTYLE", {"Colors", "TWO", "1 3"});
    f.host->run("-MLSTYLE", {"Fill", "TWO", "5"});
    f.host->run("-MLSTYLE", {"Joints", "TWO", "On"});
    auto s = mline::findStyle(f.doc, "TWO");
    REQUIRE(s);
    REQUIRE(s->colors.size() == 2);
    CHECK(s->colors[0] == 1);
    CHECK(s->colors[1] == 3);
    CHECK(s->fill == 5);
    CHECK(s->joints);
    // A wrong count changes nothing.
    f.host->run("-MLSTYLE", {"Colors", "TWO", "1"});
    CHECK(mline::findStyle(f.doc, "TWO")->colors.size() == 2);

    f.host->run("-MLSTYLE", {"Current", "TWO"});
    f.host->run("MLINE", {"0,0", "100,0", "100,50", ""});
    REQUIRE(f.mline());
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) != EntityKind::MLine)
            return;
        DisplayList dl;
        geom::buildDisplay(f.doc, e, geom::DisplayContext{}, dl);
        int fills = 0, lines = 0;
        std::set<std::uint32_t> colours;
        for (const DisplayItem& it : dl.items) {
            if (std::holds_alternative<DisplayFill>(it)) {
                ++fills;
            } else if (const auto* pl = std::get_if<DisplayPolyline>(&it)) {
                ++lines;
                colours.insert((pl->style.color.r << 16) | (pl->style.color.g << 8) |
                               pl->style.color.b);
            }
        }
        CHECK(fills == 1);
        CHECK(lines == 3);  // two lines and the joint at the corner
        CHECK(colours.size() >= 2);
        auto ex = mline::explodeMline(f.doc, e);
        REQUIRE(ex);
        REQUIRE(ex.value().size() == 3);
        CHECK(ex.value()[0].common.color == Color::index(1));
        CHECK(ex.value()[1].common.color == Color::index(3));
    });
}

TEST_CASE(
    "MLEDIT on random, parallel, overlapping and degenerate walls keeps every multiline sane") {
    std::uint32_t seed = 777;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
    };
    auto pt = [&] { return Vec2{std::floor(rnd() * 5) * 25, std::floor(rnd() * 5) * 25}; };
    auto txt = [](Vec2 p) { return std::to_string(p.x) + "," + std::to_string(p.y); };
    const char* keys[] = {"Corner", "Add", "Delete", "Close", "Open",
                          "Cross",  "Tee", "Cut",    "Weld"};
    for (int round = 0; round < 150; ++round) {
        Fx f;
        for (int w = 0; w < 2; ++w) {
            const int n = 2 + static_cast<int>(rnd() * 3);
            std::vector<ScriptItem> items;
            for (int i = 0; i < n; ++i)
                items.push_back(txt(pt()));
            items.push_back("");
            f.host->run("MLINE", items);
        }
        auto ids = mlineIds(f.doc);
        if (ids.size() < 2)
            continue;
        for (int step = 0; step < 6; ++step) {
            const char* key = keys[static_cast<int>(rnd() * 9)];
            std::vector<ScriptItem> items{ScriptItem(std::string(key))};
            for (int i = 0; i < 4; ++i) {
                const EntityId id = ids[static_cast<int>(rnd() * ids.size())];
                items.push_back(ScriptItem::pick(id, pt()));
                if (rnd() < 0.5)
                    items.push_back(txt(pt()));
            }
            items.push_back(ScriptItem::enter());
            f.host->run("MLEDIT", items);
            for (EntityId id : ids) {
                const Entity* e = f.doc.find(id);
                if (!e)
                    continue;
                const auto* m = entityAs<MlineData>(*e);
                REQUIRE(m);
                REQUIRE(m->v.size() >= 2);
                for (const Vec2& v : m->v) {
                    REQUIRE(std::isfinite(v.x));
                    REQUIRE(std::isfinite(v.y));
                }
                const mline::Strokes s = mline::strokes(f.doc, *m);
                for (const auto& line : s.lines)
                    for (const Vec2& p : line)
                        REQUIRE(std::isfinite(p.x));
            }
        }
    }
}

TEST_CASE("MLSTYLE opens the Multiline Style dialog") {
    Fx f;
    CHECK(f.host->run("MLSTYLE", {}).finished);
}

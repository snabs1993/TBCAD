#include <doctest/doctest.h>

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
    f.host->run("MLSTYLE", {"New", "WALL", "1 0 -1", "Both"});
    auto s = mline::findStyle(f.doc, "wall");
    REQUIRE(s);
    CHECK(s->offsets.size() == 3);
    CHECK(s->startCap);
    CHECK(s->endCap);
    f.host->run("MLSTYLE", {"Current", "WALL"});
    f.host->run("MLINE", {"0,0", "10,0", ""});
    REQUIRE(f.mline());
    CHECK(f.mline()->style == "WALL");
    f.host->run("MLSTYLE", {"Delete", "WALL"});
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

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

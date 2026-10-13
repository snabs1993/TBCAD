#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

TEST_CASE("EXPLODE turns an insert back into its geometry; undo restores the insert") {
    Fixture f;
    EntityId a = f.addLine(10, 10, 20, 10);
    EntityId b = f.addCircle(15, 15, 2);
    f.host->setSelection({a, b});
    f.host->run("BLOCK", {"Door", "10,10", "C"});
    f.host->run("INSERT", {"Door", "100,100"});
    EntityId ins = f.modelSpace().back();
    f.host->setSelection({ins});
    auto r = f.host->run("EXPLODE", {});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 3);  // the first insert (from BLOCK) plus the two parts
    CHECK_FALSE(f.doc.contains(ins));
    bool line = false, circle = false;
    for (EntityId id : f.modelSpace()) {
        if (const auto* l = entityAs<LineData>(*f.doc.find(id))) {
            line = true;
            CHECK(near(l->a, {100, 100}));
            CHECK(near(l->b, {110, 100}));
        }
        if (const auto* c = entityAs<CircleData>(*f.doc.find(id))) {
            circle = true;
            CHECK(near(c->center, {105, 105}));
        }
    }
    CHECK(line);
    CHECK(circle);
    CHECK(f.host->undo());
    CHECK(f.doc.contains(ins));
    CHECK(f.modelSpace().size() == 2);
}

TEST_CASE("EXPLODE a polyline and report dropped widths") {
    Fixture f;
    Entity pl;
    pl.data = PolylineData{{{{0, 0}, 0, 1, 1}, {{10, 0}, 0, 1, 1}, {{10, 10}, 0, 0, 0}}, false, 0};
    EntityId id = f.add(pl);
    f.host->setSelection({id});
    f.host->run("EXPLODE", {});
    CHECK(f.modelSpace().size() == 2);
    CHECK(f.host->logContains("widths were dropped"));
}

TEST_CASE("EXPLODE ignores what cannot be exploded") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    auto r = f.host->run("EXPLODE", {cmd::ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.doc.contains(c));
}

#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

TEST_CASE("ARRAY rectangular creates the grid of copies") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    // rows 2, columns 3, column spacing 5, row spacing 10, angle 0
    auto r = f.host->run("ARRAY", {"R", "2", "3", "5", "10", "0"});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 6);
    std::vector<Vec2> centers;
    for (EntityId id : f.modelSpace())
        centers.push_back(entityAs<CircleData>(*f.doc.find(id))->center);
    auto has = [&](Vec2 p) {
        for (Vec2 q : centers)
            if (near(p, q))
                return true;
        return false;
    };
    CHECK(has({0, 0}));
    CHECK(has({5, 0}));
    CHECK(has({10, 0}));
    CHECK(has({0, 10}));
    CHECK(has({10, 10}));
    CHECK(f.host->undoStack().undoLabel() == "Array 5 objects");
    CHECK(f.host->undo());
    CHECK(f.modelSpace().size() == 1);
}

TEST_CASE("ARRAY polar of 6 around a centre") {
    Fixture f;
    EntityId c = f.addCircle(10, 0, 1);
    f.host->setSelection({c});
    auto r = f.host->run("ARRAY", {"P", "0,0", "6", "360", "Y"});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 6);
    for (int k = 0; k < 6; ++k) {
        double a = kTwoPi * k / 6;
        bool found = false;
        for (EntityId id : f.modelSpace())
            if (near(entityAs<CircleData>(*f.doc.find(id))->center,
                     {10 * std::cos(a), 10 * std::sin(a)}, 1e-9))
                found = true;
        CHECK(found);
    }
}

TEST_CASE("ARRAY accepts the defaults with Enter") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"", "", "", "", "", ""});
    CHECK(f.modelSpace().size() == 12);  // 3 rows x 4 columns
}

TEST_CASE("ARRAY refuses a huge array and Esc changes nothing") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "5000", "5000", "5", "5", "0"});
    CHECK(f.modelSpace().size() == 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "2", cmd::ScriptItem::esc()});
    CHECK(f.modelSpace().size() == 1);
}

TEST_CASE("ARRAY previews the copies while the last distance is chosen") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "2", "2", "5"});
    f.host->runner().pointerMoved(cmd::RawInput{{0, 20}, {0, 20}, 1});
    CHECK_FALSE(f.host->preview().items.empty());
}

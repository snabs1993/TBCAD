#include "support.h"
#include "tbcad/drafting/snap_geometry.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

TEST_CASE("object snap sees the contents of an insert") {
    Fixture f;  // registers the Insert provider on the application registry
    EntityId a = f.addLine(0, 0, 10, 0);
    EntityId b = f.addCircle(5, 5, 2);
    f.host->setSelection({a, b});
    f.host->run("BLOCK", {"B", "0,0", "D"});
    f.host->run("INSERT", {"B", "S", "2", "100,100"});
    const Entity& ins = *f.doc.find(f.modelSpace()[0]);

    drafting::SnapGeometry g;
    REQUIRE(drafting::SnapGeometryRegistry::instance().collect(f.doc, ins, g));
    auto has = [](const std::vector<Vec2>& v, Vec2 p) {
        for (Vec2 q : v)
            if (near(p, q))
                return true;
        return false;
    };
    CHECK(has(g.endpoints, {100, 100}));
    CHECK(has(g.endpoints, {120, 100}));
    CHECK(has(g.midpoints, {110, 100}));
    CHECK(has(g.centers, {110, 110}));
    CHECK(g.curves.size() == 2);
}

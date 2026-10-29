// Associative hatches follow their boundary through the transform commands, and one undo takes
// the boundary and the hatch back together.
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, (registerAllCommands(reg), reg)) {}

    EntityId add(EntityData d) {
        Transaction tx = doc.begin("setup");
        auto id = tx.add(Entity{{}, std::move(d)});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    const HatchData* theHatch(EntityId* idOut = nullptr) const {
        const HatchData* found = nullptr;
        doc.forEachEntity([&](const Entity& e) {
            if (const HatchData* h = entityAs<HatchData>(e)) {
                found = h;
                if (idOut)
                    *idOut = e.common.id;
            }
        });
        return found;
    }
};

BBox2 boundaryBounds(const Rig& r, const HatchData& h) {
    BBox2 box;
    bool first = true;
    for (EntityId id : h.sourceBoundary) {
        const Entity* e = r.doc.find(id);
        REQUIRE(e != nullptr);
        BBox2 b = geom::bounds(*e, r.doc);
        if (first) {
            box = b;
            first = false;
        } else {
            box.min = {std::min(box.min.x, b.min.x), std::min(box.min.y, b.min.y)};
            box.max = {std::max(box.max.x, b.max.x), std::max(box.max.y, b.max.y)};
        }
    }
    return box;
}

void makeShape(Rig& r, int which) {
    switch (which) {
        case 0:
            r.add(CircleData{{50, 50}, 30});
            break;
        case 1: {
            PolylineData p;
            p.v = {{{0, 0}, 0.4, 0, 0},
                   {{100, 0}, 0, 0, 0},
                   {{100, 60}, -0.3, 0, 0},
                   {{0, 60}, 0, 0, 0}};
            p.closed = true;
            r.add(p);
            break;
        }
        case 2:
            r.add(LineData{{0, 0}, {100, 0}});
            r.add(LineData{{100, 0}, {100, 60}});
            r.add(LineData{{100, 60}, {0, 60}});
            r.add(LineData{{0, 60}, {0, 0}});
            break;
        default: {
            EllipseData e;
            e.center = {50, 30};
            e.majorAxis = {45, 10};
            e.ratio = 0.6;
            r.add(Indirect<EllipseData>(e));
        }
    }
}

}  // namespace

TEST_CASE("an associative hatch follows its boundary through MOVE, ROTATE, and SCALE") {
    struct Edit {
        const char* name;
        std::vector<ScriptItem> in;
    };
    const Edit edits[] = {
        {"MOVE", {"0,0", "37.5,-12"}},
        {"ROTATE", {"10,10", "33"}},
        {"SCALE", {"0,0", "1.7"}},
    };
    for (int shape = 0; shape < 4; ++shape) {
        for (const auto& edit : edits) {
            CAPTURE(shape);
            CAPTURE(edit.name);
            Rig r;
            makeShape(r, shape);
            r.host.run("HATCH", {"50,30", ScriptItem::enter()});
            EntityId hid;
            const HatchData* h = r.theHatch(&hid);
            REQUIRE(h != nullptr);
            REQUIRE_FALSE(h->sourceBoundary.empty());
            const std::vector<EntityId> sources = h->sourceBoundary;
            const std::string before = canonicalDump(r.doc);

            r.host.setSelection(sources);
            r.host.run(edit.name, edit.in);
            const std::string after = canonicalDump(r.doc);
            REQUIRE(after != before);
            h = r.theHatch();
            REQUIRE(h != nullptr);
            // The hatch is still tied to its boundary and fills the same place.
            CHECK(h->sourceBoundary == sources);
            const BBox2 want = boundaryBounds(r, *h);
            const BBox2 got = hatch::hatchBounds(*h);
            CHECK(got.min.x == doctest::Approx(want.min.x).epsilon(1e-6).scale(100));
            CHECK(got.min.y == doctest::Approx(want.min.y).epsilon(1e-6).scale(100));
            CHECK(got.max.x == doctest::Approx(want.max.x).epsilon(1e-6).scale(100));
            CHECK(got.max.y == doctest::Approx(want.max.y).epsilon(1e-6).scale(100));
            // One undo step takes both back.
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
            CHECK(r.host.redo());
            CHECK(canonicalDump(r.doc) == after);
        }
    }
}

TEST_CASE("erasing one edge of the boundary detaches the hatch and keeps its fill") {
    Rig r;
    makeShape(r, 2);
    r.host.run("HATCH", {"50,30", ScriptItem::enter()});
    const HatchData* h = r.theHatch();
    REQUIRE(h != nullptr);
    const BBox2 before = hatch::hatchBounds(*h);
    const EntityId victim = h->sourceBoundary.front();
    r.host.setSelection({victim});
    r.host.run("ERASE", {});
    h = r.theHatch();
    REQUIRE(h != nullptr);
    CHECK(h->sourceBoundary.empty());
    const BBox2 after = hatch::hatchBounds(*h);
    CHECK(after.max.x == doctest::Approx(before.max.x));
    CHECK(after.max.y == doctest::Approx(before.max.y));
    CHECK(r.host.undo());
    h = r.theHatch();
    REQUIRE(h != nullptr);
    CHECK_FALSE(h->sourceBoundary.empty());
}

TEST_CASE("dragging a corner grip or stretching the boundary updates the hatch in one step") {
    for (int how = 0; how < 2; ++how) {
        CAPTURE(how);
        Rig r;
        makeShape(r, 2);
        r.host.run("HATCH", {"50,30", ScriptItem::enter()});
        const HatchData* h = r.theHatch();
        REQUIRE(h != nullptr);
        const std::vector<EntityId> sources = h->sourceBoundary;
        const std::string before = canonicalDump(r.doc);
        if (how == 0) {
            r.host.setSelection(sources);
            r.host.run("GRIPEDIT", {"100,60", "130,80"});
        } else {
            r.host.run("STRETCH", {"90,50", "110,70", "100,60", "130,80"});
        }
        h = r.theHatch();
        REQUIRE(h != nullptr);
        const BBox2 got = hatch::hatchBounds(*h);
        CHECK(got.max.x == doctest::Approx(130));
        CHECK(got.max.y == doctest::Approx(80));
        CHECK(r.host.undo());
        CHECK(canonicalDump(r.doc) == before);
    }
}

TEST_CASE("trimming or extending a boundary edge keeps the hatch coherent and undoes in one step") {
    Rig r;
    makeShape(r, 2);
    r.host.run("HATCH", {"50,30", ScriptItem::enter()});
    const HatchData* h = r.theHatch();
    REQUIRE(h != nullptr);
    REQUIRE_FALSE(h->sourceBoundary.empty());
    const BBox2 before = hatch::hatchBounds(*h);
    const std::vector<EntityId> boundary = h->sourceBoundary;  // `h` is not kept past an edit
    // A cutting line across the right edge at y = 30; trimming the upper half opens the boundary.
    const EntityId cutter = r.add(LineData{{80, 30}, {120, 30}});
    EntityId right{};
    for (EntityId id : boundary)
        if (const auto* l = entityAs<LineData>(*r.doc.find(id));
            l && l->a.x == 100 && l->b.x == 100)
            right = id;
    REQUIRE(static_cast<bool>(right));
    r.host.run("TRIM", {ScriptItem::select({cutter}), ScriptItem::enter(),
                        ScriptItem::pick(right, {100, 50}), ScriptItem::enter()});
    h = r.theHatch();
    REQUIRE(h != nullptr);
    // Open boundary: the hatch keeps its fill, and is no longer tied to the edges.
    const BBox2 after = hatch::hatchBounds(*h);
    CHECK(after.max.x == doctest::Approx(before.max.x));
    CHECK(after.max.y == doctest::Approx(before.max.y));
    CHECK(h->sourceBoundary.empty());
    CHECK(r.host.undo());
    h = r.theHatch();
    REQUIRE(h != nullptr);
    CHECK_FALSE(h->sourceBoundary.empty());
    // The edge is whole again.
    int whole = 0;
    r.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e); l && std::abs(l->a.x - 100) < 1e-9 &&
                                                   std::abs(l->b.x - 100) < 1e-9 &&
                                                   std::abs(l->a.y - l->b.y) > 59)
            ++whole;
    });
    CHECK(whole == 1);
}

TEST_CASE("COPY and MIRROR that keep the source leave the original hatch and its boundary alone") {
    for (int shape = 0; shape < 4; ++shape) {
        CAPTURE(shape);
        Rig r;
        makeShape(r, shape);
        r.host.run("HATCH", {"50,30", ScriptItem::enter()});
        const HatchData* h = r.theHatch();
        REQUIRE(h != nullptr);
        const std::vector<EntityId> sources = h->sourceBoundary;
        const BBox2 hatchBefore = hatch::hatchBounds(*h);
        r.host.setSelection(sources);
        r.host.run("MIRROR", {"10,-5", "40,60", "N"});
        h = r.theHatch();
        REQUIRE(h != nullptr);
        CHECK(h->sourceBoundary == sources);
        const BBox2 hatchAfter = hatch::hatchBounds(*h);
        CHECK(hatchAfter.min.x == doctest::Approx(hatchBefore.min.x));
        CHECK(hatchAfter.max.y == doctest::Approx(hatchBefore.max.y));
        CHECK(r.host.undo());
        r.host.setSelection(sources);
        r.host.run("COPY", {"0,0", "200,0", ScriptItem::enter()});
        h = r.theHatch();
        REQUIRE(h != nullptr);
        CHECK(h->sourceBoundary == sources);
        const BBox2 hb = hatch::hatchBounds(*h);
        CHECK(hb.min.x == doctest::Approx(hatchBefore.min.x));
        CHECK(hb.max.x == doctest::Approx(hatchBefore.max.x));
    }
}

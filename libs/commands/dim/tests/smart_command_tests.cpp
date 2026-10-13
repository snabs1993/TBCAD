#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dimcmd/dimcmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

struct Fix {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;

    Fix() {
        registerBuiltinCommands(reg);
        dimcmd::registerCommands(reg);
        host = std::make_unique<ScriptHost>(doc, reg);
    }
    EntityId add(EntityData d) {
        Transaction tx = doc.begin("setup");
        Entity e;
        e.data = std::move(d);
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    }
    template <class T>
    std::vector<const T*> all() const {
        std::vector<const T*> out;
        doc.forEachEntity([&](const Entity& e) {
            if (const auto* d = entityAs<T>(e))
                out.push_back(d);
        });
        return out;
    }
};

}  // namespace

TEST_CASE("DIM picks linear for an axis-aligned line and aligned for a slanted one") {
    Fix f;
    EntityId h = f.add(LineData{{0, 0}, {100, 0}});
    EntityId s = f.add(LineData{{0, 50}, {30, 90}});
    auto r = f.host->run("DIM", {ScriptItem::pick(h, {50, 0}), "50,-20",
                                 ScriptItem::pick(s, {15, 70}), "-20,80", ScriptItem::enter()});
    CHECK(r.finished);
    auto dims = f.all<DimensionData>();
    REQUIRE(dims.size() == 2);
    int linear = 0, aligned = 0;
    for (const auto* d : dims) {
        if (d->kind == DimKind::Linear) {
            ++linear;
            CHECK(d->measurement == doctest::Approx(100));
        }
        if (d->kind == DimKind::Aligned) {
            ++aligned;
            CHECK(d->measurement == doctest::Approx(50));
        }
    }
    CHECK(linear == 1);
    CHECK(aligned == 1);
    CHECK(f.host->undo());
    CHECK(f.all<DimensionData>().empty());  // one command, one undo step
}

TEST_CASE("DIM: circle gives a diameter, arc a radius, and a keyword switches") {
    Fix f;
    EntityId c = f.add(CircleData{{0, 0}, 10});
    EntityId a = f.add(ArcData{{100, 0}, 10, 0, kPi / 2});
    auto r = f.host->run(
        "DIM", {ScriptItem::pick(c, {10, 0}), "30,30", ScriptItem::pick(a, {100, 10}), "Diameter",
                "120,20", ScriptItem::pick(c, {10, 0}), "Radius", "30,-30", ScriptItem::enter()});
    CHECK(r.finished);
    auto dims = f.all<DimensionData>();
    REQUIRE(dims.size() == 3);
    CHECK(dims[0]->kind == DimKind::Diameter);
    CHECK(dims[0]->measurement == doctest::Approx(20));
    CHECK(dims[1]->kind == DimKind::Diameter);  // the arc, switched
    CHECK(dims[2]->kind == DimKind::Radius);    // the circle, switched
    CHECK(dims[2]->measurement == doctest::Approx(10));
}

TEST_CASE("DIM: a line plus the Angular keyword and a second line gives an angle") {
    Fix f;
    EntityId l1 = f.add(LineData{{0, 0}, {100, 0}});
    EntityId l2 = f.add(LineData{{0, 0}, {0, 100}});
    auto r = f.host->run("DIM", {ScriptItem::pick(l1, {50, 0}), "Angular",
                                 ScriptItem::pick(l2, {0, 50}), "40,40", ScriptItem::enter()});
    CHECK(r.finished);
    auto dims = f.all<DimensionData>();
    REQUIRE(dims.size() == 1);
    CHECK(dims[0]->kind == DimKind::Angular2Line);
    CHECK(dims[0]->measurement == doctest::Approx(kPi / 2));
}

TEST_CASE("MLEADER: several arrowheads, one landing point and a note") {
    Fix f;
    auto r =
        f.host->run("MLEADER", {"0,0", "40,30", "0,60", ScriptItem::enter(), "Text", "Steel beam"});
    CHECK(r.finished);
    auto ls = f.all<LeaderData>();
    REQUIRE(ls.size() == 2);
    int withText = 0;
    for (const auto* l : ls) {
        CHECK(l->vertices.back().x == doctest::Approx(40));
        CHECK(l->vertices.back().y == doctest::Approx(30));
        if (!l->utf8.empty())
            ++withText;
    }
    CHECK(withText == 1);
    CHECK(f.host->undo());
    CHECK(f.all<LeaderData>().empty());
}

TEST_CASE("MLEADER: block content inserts the block at the landing point") {
    Fix f;
    {
        Transaction tx = f.doc.begin("block");
        BlockDef def;
        def.name = "Tag";
        REQUIRE(tx.addBlock(std::move(def)));
        tx.commit();
    }
    auto r = f.host->run("MLEADER", {"0,0", "40,30", ScriptItem::enter(), "Block", "Tag"});
    CHECK(r.finished);
    CHECK(f.all<LeaderData>().size() == 1);
    CHECK(f.all<InsertData>().size() == 1);

    auto bad = f.host->run("MLEADER", {"0,0", "40,30", ScriptItem::enter(), "Block", "Nope"});
    CHECK(bad.finished);
    CHECK(f.all<InsertData>().size() == 1);
}

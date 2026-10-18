// STRETCH, SCALE, MIRROR and ALIGN on the objects that depend on others: a hatch follows its
// boundary, a dimension follows its points, an insert with attributes moves as a whole. Every
// command must also undo and redo exactly.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/dim/dim.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, (registerAllCommands(reg), reg)) {}

    std::vector<EntityId> ids() const {
        std::vector<EntityId> v;
        doc.forEachEntity([&](const Entity& e) {
            if (!doc.ownerBlock(e.common.id))
                v.push_back(e.common.id);
        });
        std::sort(v.begin(), v.end());
        return v;
    }
    template <class T>
    const T* first() const {
        for (EntityId id : ids())
            if (const auto* d = entityAs<T>(*doc.find(id)))
                return d;
        return nullptr;
    }
    BBox2 hatchBox() const {
        BBox2 b;
        if (const auto* h = first<HatchData>())
            for (const HatchLoop& l : h->loops)
                for (const HatchEdge& e : l.edges)
                    if (const auto* le = std::get_if<HatchLineEdge>(&e)) {
                        b.expand(le->a);
                        b.expand(le->b);
                    }
        return b;
    }
    // Rectangle 0,0 - 100,50 with its associative hatch and a dimension along the top.
    void house() {
        host.run("RECTANGLE", {"0,0", "100,50"});
        host.run("HATCH", {"50,25", ""});
        host.run("DIMLINEAR", {"0,50", "100,50", "50,70"});
    }
};

}  // namespace

TEST_CASE("STRETCH moves the hatch edge and the dimension with the boundary") {
    Rig r;
    r.house();
    REQUIRE(r.first<HatchData>());
    REQUIRE(r.first<DimensionData>());
    const std::string before = canonicalDump(r.doc);
    r.host.run("STRETCH", {"90,-10", "110,60", "0,0", "30,0"});
    const auto* pl = r.first<PolylineData>();
    REQUIRE(pl);
    double maxX = -1e9;
    for (const auto& v : pl->v)
        maxX = std::max(maxX, v.p.x);
    CHECK(maxX == doctest::Approx(130));
    CHECK(r.hatchBox().max.x == doctest::Approx(130));
    CHECK(r.hatchBox().min.x == doctest::Approx(0));
    const auto* d = r.first<DimensionData>();
    REQUIRE(d);
    CHECK(d->defPts[1].x == doctest::Approx(130));
    CHECK(dim::measurement(*d) == doctest::Approx(130));
    CHECK(r.host.undoStack().undoCount() == 4);  // one step for the whole stretch
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
    CHECK(r.host.redo());
    CHECK(r.hatchBox().max.x == doctest::Approx(130));
}

TEST_CASE("SCALE and MIRROR keep hatch, boundary and dimension together") {
    Rig r;
    r.house();
    const std::string before = canonicalDump(r.doc);
    r.host.setSelection(r.ids());
    r.host.run("SCALE", {"0,0", "2"});
    CHECK(r.hatchBox().max.x == doctest::Approx(200));
    CHECK(r.hatchBox().max.y == doctest::Approx(100));
    CHECK(dim::measurement(*r.first<DimensionData>()) == doctest::Approx(200));
    r.host.setSelection(r.ids());
    r.host.run("MIRROR", {"0,0", "0,10", "Y"});  // erase the source
    CHECK(r.first<PolylineData>()->v[0].p.x <= 0);
    CHECK(r.hatchBox().max.x == doctest::Approx(0).epsilon(1e-6));
    CHECK(r.hatchBox().min.x == doctest::Approx(-200));
    CHECK(dim::measurement(*r.first<DimensionData>()) == doctest::Approx(200));
    CHECK(r.host.undo());
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("STRETCH, ALIGN and SCALE take an insert with attributes along") {
    Rig r;
    {
        auto tx = r.doc.begin("block");
        BlockId b = tx.addBlock({.name = "TAG"}).value();
        Entity l;
        l.data = LineData{{0, 0}, {10, 0}};
        (void)tx.add(std::move(l), b);
        Entity a;
        TextData t;
        t.position = {1, 2};
        t.height = 2;
        t.utf8 = "X";
        t.attdef = AttDefInfo{.tag = "NAME", .prompt = "Name", .defaultValue = "A"};
        a.data = t;
        (void)tx.add(std::move(a), b);
        tx.commit();
    }
    r.host.run("INSERT", {"TAG", "20,20", ScriptItem::enter(), ScriptItem::enter()});
    const auto* in = r.first<InsertData>();
    REQUIRE(in);
    const std::size_t attribCount = in->attribs.size();  // `in` dangles once the insert is edited
    const std::string before = canonicalDump(r.doc);
    CHECK_FALSE(r.host.runner().active());
    r.host.run("STRETCH", {"10,10", "30,30", "0,0", "5,5"});
    CHECK(r.first<InsertData>()->position.x == doctest::Approx(25));
    CHECK(r.first<InsertData>()->attribs.size() == attribCount);
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
    r.host.setSelection(r.ids());
    r.host.run("ALIGN", {"20,20", "40,20", "30,20", "40,30", "", "N"});
    REQUIRE(r.host.undoStack().undoCount() == 2);  // the insert and the align
    CHECK(r.first<InsertData>()->rotation == doctest::Approx(kPi / 2));
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
    r.host.setSelection(r.ids());
    r.host.run("SCALE", {"20,20", "3"});
    CHECK(r.first<InsertData>()->scale.x == doctest::Approx(3));
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
}

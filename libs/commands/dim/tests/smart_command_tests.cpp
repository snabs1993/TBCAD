#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dim/dim.h"
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
    auto r = f.host->run("MLEADER", {"0,0", "Add", "0,60", "40,30", "Steel beam"});
    CHECK(r.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 1);  // one object with both leader lines and the note
    REQUIRE(ms[0]->lines.size() == 2);
    for (const auto& line : ms[0]->lines) {
        CHECK(line.back().x == doctest::Approx(40));
        CHECK(line.back().y == doctest::Approx(30));
    }
    CHECK(ms[0]->landing.x == doctest::Approx(40));
    CHECK(ms[0]->utf8 == "Steel beam");
    CHECK(ms[0]->contentPos.x > ms[0]->landing.x);
    CHECK(f.host->undo());
    CHECK(f.all<MLeaderData>().empty());
}

TEST_CASE("MLEADER: text typed at the first prompt is the note") {
    Fix f;
    auto r = f.host->run("MLEADER", {"Door D1", "0,0", "40,30"});
    CHECK(r.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 1);
    CHECK(ms[0]->utf8 == "Door D1");
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
    auto r = f.host->run("MLEADER", {"Options", "Type", "Block", "eXit", "0,0", "40,30", "Tag"});
    CHECK(r.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 1);
    CHECK(ms[0]->content == MLeaderData::Content::Block);
    CHECK(f.doc.block(ms[0]->block)->name == "Tag");

    auto bad = f.host->run("MLEADER", {"Options", "Type", "Block", "eXit", "0,0", "40,30", "Nope"});
    CHECK(bad.finished);
    CHECK(f.all<MLeaderData>().size() == 1);
}

TEST_CASE("MLEADER: a block with attributes asks for their values") {
    Fix f;
    BlockId tag{};
    {
        Transaction tx = f.doc.begin("block");
        BlockDef def;
        def.name = "TagCircle";
        auto b = tx.addBlock(std::move(def));
        REQUIRE(b);
        tag = b.value();
        TextData t;
        t.utf8 = "NO";
        t.attdef = AttDefInfo{"NO", "Number", "1", false, false, false, false};
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        REQUIRE(tx.add(std::move(e), tag));
        tx.commit();
    }
    auto r = f.host->run("MLEADER",
                         {"Options", "Type", "Block", "eXit", "0,0", "40,30", "TagCircle", "A7"});
    CHECK(r.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 1);
    REQUIRE(ms[0]->attribs.size() == 1);
    CHECK(ms[0]->attribs[0].tag == "NO");
    CHECK(ms[0]->attribs[0].value == "A7");
}

TEST_CASE("MLEADER leader Landing first: landing, then arrowheads, then the note") {
    Fix f;
    auto r = f.host->run("MLEADER", {"Landing", "40,30", "0,0", "0,60", "", "Beam"});
    CHECK(r.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 1);
    CHECK(ms[0]->lines.size() == 2);
    CHECK(ms[0]->landing.x == doctest::Approx(40));
    CHECK(ms[0]->utf8 == "Beam");
}

TEST_CASE("MLEADER Content first: the note, where it goes, then the arrowhead") {
    Fix f;
    auto r = f.host->run("MLEADER", {"Content", "Pipe P1", "50,30", "0,0", ""});
    CHECK(r.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 1);
    CHECK(ms[0]->utf8 == "Pipe P1");
    CHECK(ms[0]->contentPos.x == doctest::Approx(50));
    CHECK(ms[0]->landing.x < 50);  // one dogleg towards the arrow
    CHECK(ms[0]->lines[0].front().x == doctest::Approx(0));
}

TEST_CASE("MLEADER Options: Maxpoints bends the leader line, Dogleg No leaves it out") {
    Fix f;
    auto r = f.host->run("MLEADER", {"Options", "Maxpoints", "3", "Dogleg", "No", "eXit", "0,0",
                                     "10,20", "30,20", "Note"});
    CHECK(r.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 1);
    REQUIRE(ms[0]->lines.size() == 1);
    CHECK(ms[0]->lines[0].size() == 3);  // arrowhead, bend, landing
    CHECK(ms[0]->landing.x == doctest::Approx(30));
    CHECK_FALSE(ms[0]->dogleg);
    CHECK(ms[0]->contentPos.x == doctest::Approx(30));
    // Enter after a bend makes it the landing
    auto r2 =
        f.host->run("MLEADER", {"Options", "Maxpoints", "5", "eXit", "0,0", "10,20", "", "Two"});
    CHECK(r2.finished);
    ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 2);
    CHECK(ms[1]->lines[0].size() == 2);
    CHECK(ms[1]->landing.y == doctest::Approx(20));
}

TEST_CASE("MLEADER: arrow, landing, then the note, text typed at the landing prompt is the note") {
    Fix f;
    auto r = f.host->run("MLEADER", {"0,0", "40,30", "Beam B1"});
    CHECK(r.finished);
    auto r2 = f.host->run("MLEADER", {"0,0", "Column C2", "40,30"});
    CHECK(r2.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 2);
    CHECK(ms[0]->utf8 == "Beam B1");
    CHECK(ms[1]->utf8 == "Column C2");
}

TEST_CASE("DIM Continue and Baseline chain from the dimension just made, then DIM goes on") {
    Fix f;
    EntityId h = f.add(LineData{{0, 0}, {100, 0}});
    auto r = f.host->run(
        "DIM", {ScriptItem::pick(h, {50, 0}), "50,-30", "Continue", "250,0", ScriptItem::enter(),
                "Baseline", "400,0", ScriptItem::enter(), ScriptItem::enter()});
    CHECK(r.finished);
    auto dims = f.all<DimensionData>();
    REQUIRE(dims.size() == 3);
    CHECK(dims[1]->defPts[0].x == doctest::Approx(100));
    CHECK(dims[1]->measurement == doctest::Approx(150));
    CHECK(dims[1]->defPts[2].y == doctest::Approx(-30));
    // Baseline from the continued dimension: its first extension line, one spacing further out.
    CHECK(dims[2]->defPts[0].x == doctest::Approx(100));
    CHECK(dims[2]->measurement == doctest::Approx(300));
    CHECK(f.host->undo());
    CHECK(f.all<DimensionData>().empty());  // one command, one undo step
}

TEST_CASE("MLEADER Options: First and Second angle snap the leader segments") {
    Fix f;
    auto r = f.host->run("MLEADER", {"Options", "Maxpoints", "3", "First", "45", "Second", "90",
                                     "eXit", "0,0", "10,3", "12,30", "Note"});
    CHECK(r.finished);
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 1);
    REQUIRE(ms[0]->lines[0].size() == 3);
    CHECK(ms[0]->lines[0][1].y == doctest::Approx(0).epsilon(1e-9));  // first segment horizontal
    CHECK(ms[0]->lines[0][1].x > 10);
    // the second segment is vertical from the bend
    CHECK(ms[0]->landing.x == doctest::Approx(ms[0]->lines[0][1].x));
}

TEST_CASE("MLEADER Options: Leader type Spline curves the leader line, None leaves it out") {
    Fix f;
    f.host->run("MLEADER", {"Options", "Leader", "Spline", "Maxpoints", "3", "eXit", "0,0", "10,20",
                            "30,20", "Note"});
    f.host->run("MLEADER", {"Options", "Leader", "None", "eXit", "0,40", "30,60", "Note"});
    auto ms = f.all<MLeaderData>();
    REQUIRE(ms.size() == 2);
    CHECK(ms[0]->lineType == MLeaderData::LineType::Spline);
    CHECK(ms[1]->lineType == MLeaderData::LineType::None);
    const tbcad::dim::Layout curved = tbcad::dim::layoutMLeader(f.doc, *ms[0], 0.01, 1);
    REQUIRE_FALSE(curved.lines.empty());
    CHECK(curved.lines[0].pts.size() > 3);  // sampled
    const tbcad::dim::Layout alone = tbcad::dim::layoutMLeader(f.doc, *ms[1], 0.01, 1);
    for (const auto& line : alone.lines)  // only the dogleg is left, no line from the arrowhead
        CHECK(line.pts.front() != ms[1]->lines[0].front());
}

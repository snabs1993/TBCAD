#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dimcmd/dimcmd.h"
#include "tbcad/geom/geom.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

struct Fixture {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;

    Fixture() {
        registerBuiltinCommands(reg);
        dimcmd::registerCommands(reg);
        host = std::make_unique<ScriptHost>(doc, reg);
    }

    EntityId addEntity(EntityData d) {
        Transaction tx = doc.begin("setup");
        Entity e;
        e.data = std::move(d);
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    }
    std::vector<const DimensionData*> dims() const {
        std::vector<const DimensionData*> out;
        doc.forEachEntity([&](const Entity& e) {
            if (const auto* d = entityAs<DimensionData>(e))
                out.push_back(d);
        });
        return out;
    }
    std::vector<const LeaderData*> leaders() const {
        std::vector<const LeaderData*> out;
        doc.forEachEntity([&](const Entity& e) {
            if (const auto* d = entityAs<LeaderData>(e))
                out.push_back(d);
        });
        return out;
    }
};

}  // namespace

TEST_CASE("the dimension commands are registered with their aliases") {
    Fixture f;
    CHECK(f.reg.find("DLI") == f.reg.find("DIMLINEAR"));
    CHECK(f.reg.find("DAL") == f.reg.find("DIMALIGNED"));
    CHECK(f.reg.find("DRA") == f.reg.find("DIMRADIUS"));
    CHECK(f.reg.find("DDI") == f.reg.find("DIMDIAMETER"));
    CHECK(f.reg.find("DAN") == f.reg.find("DIMANGULAR"));
    CHECK(f.reg.find("LE") == f.reg.find("LEADER"));
    CHECK(f.reg.find("D") == f.reg.find("DIMSTYLE"));
    CHECK(f.reg.find("DIMLINEAR") != nullptr);
    CHECK(geom::DisplayRegistry::has(EntityKind::Dimension));
    CHECK(geom::DisplayRegistry::has(EntityKind::Leader));
    CHECK(blocks::ExplodeRegistry::has(EntityKind::Dimension));
    CHECK(blocks::ExplodeRegistry::has(EntityKind::Leader));
}

TEST_CASE("DIMLINEAR: the cursor decides horizontal or vertical") {
    Fixture f;
    auto r = f.host->run("DIMLINEAR", {"0,0", "100,40", "50,70"});
    CHECK(r.finished);
    auto d = f.dims();
    REQUIRE(d.size() == 1);
    CHECK(d[0]->kind == DimKind::Linear);
    CHECK(d[0]->rotation == doctest::Approx(0));
    CHECK(d[0]->measurement == doctest::Approx(100));
    CHECK(d[0]->defPts[0] == Vec2{0, 0});
    CHECK(d[0]->defPts[1] == Vec2{100, 40});
    CHECK(d[0]->defPts[2] == Vec2{50, 70});
    CHECK(d[0]->style == kDimStyleStandard);

    Fixture g;
    g.host->run("DIMLINEAR", {"0,0", "100,40", "140,20"});
    auto v = g.dims();
    REQUIRE(v.size() == 1);
    CHECK(v[0]->rotation == doctest::Approx(kPi / 2));
    CHECK(v[0]->measurement == doctest::Approx(40));
}

TEST_CASE("DIMLINEAR: keywords Horizontal, Vertical, Rotated and Text") {
    Fixture f;
    f.host->run("DIMLINEAR", {"0,0", "100,40", "Vertical", "140,20"});
    REQUIRE(f.dims().size() == 1);
    CHECK(f.dims()[0]->rotation == doctest::Approx(kPi / 2));

    Fixture g;
    g.host->run("DLI", {"0,0", "100,40", "Text", "<> typ.", "50,70"});
    REQUIRE(g.dims().size() == 1);
    CHECK(g.dims()[0]->textOverride == "<> typ.");

    Fixture h;
    h.host->run("DLI", {"0,0", "100,0", "Rotated", "45", "50,70"});
    REQUIRE(h.dims().size() == 1);
    CHECK(h.dims()[0]->rotation == doctest::Approx(kPi / 4));
    CHECK(h.dims()[0]->measurement == doctest::Approx(100 * std::cos(kPi / 4)));
}

TEST_CASE("DIMLINEAR: Enter selects an object - Esc leaves nothing behind") {
    Fixture f;
    EntityId line = f.addEntity(LineData{{0, 0}, {60, 0}});
    f.host->run("DIMLINEAR", {"", ScriptItem::pick(line, {30, 0}), "30,12"});
    REQUIRE(f.dims().size() == 1);
    CHECK(f.dims()[0]->measurement == doctest::Approx(60));

    Fixture g;
    auto r = g.host->run("DIMLINEAR", {"0,0", "100,0", ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(g.dims().empty());
}

TEST_CASE("DIMALIGNED measures along the points") {
    Fixture f;
    f.host->run("DIMALIGNED", {"0,0", "30,40", "-10,20"});
    auto d = f.dims();
    REQUIRE(d.size() == 1);
    CHECK(d[0]->kind == DimKind::Aligned);
    CHECK(d[0]->measurement == doctest::Approx(50));
}

TEST_CASE("DIMRADIUS and DIMDIAMETER on a circle and an arc") {
    Fixture f;
    EntityId c = f.addEntity(CircleData{{50, 50}, 20});
    f.host->run("DIMRADIUS", {ScriptItem::pick(c, {70, 50}), "90,50"});
    auto d = f.dims();
    REQUIRE(d.size() == 1);
    CHECK(d[0]->kind == DimKind::Radius);
    CHECK(d[0]->measurement == doctest::Approx(20));
    CHECK(d[0]->defPts[0] == Vec2{50, 50});
    CHECK(distance(d[0]->defPts[1], Vec2{70, 50}) < 1e-9);

    f.host->run("DIMDIAMETER", {ScriptItem::pick(c, {70, 50}), "50,50"});
    d = f.dims();
    REQUIRE(d.size() == 2);
    const DimensionData* dia = nullptr;
    for (auto* x : d)
        if (x->kind == DimKind::Diameter)
            dia = x;
    REQUIRE(dia);
    CHECK(dia->measurement == doctest::Approx(40));

    EntityId arc = f.addEntity(ArcData{{0, 0}, 10, 0, kPi / 2});
    f.host->run("DRA", {ScriptItem::pick(arc, {7, 7}), "20,20"});
    CHECK(f.dims().size() == 3);

    Fixture g;
    EntityId ln = g.addEntity(LineData{{0, 0}, {1, 1}});
    g.host->run("DIMRADIUS", {ScriptItem::pick(ln)});
    CHECK(g.dims().empty());
}

TEST_CASE("DIMANGULAR: two lines, perpendicular reads 90 degrees - three points") {
    Fixture f;
    EntityId a = f.addEntity(LineData{{0, 0}, {50, 0}});
    EntityId b = f.addEntity(LineData{{0, 0}, {0, 50}});
    f.host->run("DIMANGULAR", {ScriptItem::pick(a), ScriptItem::pick(b), "30,30"});
    auto d = f.dims();
    REQUIRE(d.size() == 1);
    CHECK(d[0]->kind == DimKind::Angular2Line);
    CHECK(d[0]->measurement == doctest::Approx(kPi / 2));
    CHECK(dim::dimensionText(DimStyle{}, *d[0]).rfind("90", 0) == 0);

    Fixture g;
    g.host->run("DAN", {"", "0,0", "50,0", "0,50", "30,30"});
    auto e = g.dims();
    REQUIRE(e.size() == 1);
    CHECK(e[0]->kind == DimKind::Angular3Point);
    CHECK(e[0]->measurement == doctest::Approx(kPi / 2));

    Fixture h;
    EntityId p = h.addEntity(LineData{{0, 0}, {50, 0}});
    EntityId q = h.addEntity(LineData{{0, 10}, {50, 10}});
    h.host->run("DAN", {ScriptItem::pick(p), ScriptItem::pick(q)});
    CHECK(h.dims().empty());
    CHECK(h.host->logContains("parallel"));
}

TEST_CASE("LEADER: points, Enter, annotation") {
    Fixture f;
    auto r = f.host->run("LEADER", {"0,0", "20,20", "40,20", "", "Check this"});
    CHECK(r.finished);
    auto l = f.leaders();
    REQUIRE(l.size() == 1);
    CHECK(l[0]->vertices.size() == 3);
    CHECK(l[0]->utf8 == "Check this");
    CHECK(l[0]->textPos.x > 40);
    CHECK(l[0]->style == kDimStyleStandard);

    Fixture g;
    g.host->run("LE", {"0,0", ""});
    CHECK(g.leaders().empty());
}

TEST_CASE("one undo removes the whole dimension") {
    Fixture f;
    f.host->run("DIMLINEAR", {"0,0", "100,0", "50,10"});
    REQUIRE(f.dims().size() == 1);
    CHECK(f.host->undo());
    CHECK(f.dims().empty());
}

TEST_CASE("EXPLODE turns a dimension into plain objects") {
    Fixture f;
    f.host->run("DIMLINEAR", {"0,0", "100,0", "50,10"});
    EntityId id;
    f.doc.forEachEntity([&](const Entity& e) { id = e.common.id; });
    auto r = blocks::explode(f.doc, *f.doc.find(id));
    REQUIRE(r.ok());
    CHECK(r.value().entities.size() >= 6);
}

TEST_CASE("DIMARC measures the length of an arc") {
    Fixture f;
    ArcData a;
    a.center = {0, 0};
    a.radius = 50;
    a.startAngle = 0;
    a.endAngle = kPi / 2;
    EntityId id = f.addEntity(a);
    REQUIRE(f.host->run("DIMARC", {ScriptItem::pick(id, {35, 35}), "70,70"}).finished);
    const DimensionData* d = nullptr;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* x = entityAs<DimensionData>(e))
            d = x;
    });
    REQUIRE(d);
    CHECK(d->kind == DimKind::ArcLength);
    CHECK(d->measurement == doctest::Approx(kPi / 2 * 50));
    auto L = dim::layoutDimension(f.doc, *d);
    CHECK(L.arcs.size() >= 1);
    CHECK(L.text.valid());
    CHECK(L.text.utf8.find("78.54") != std::string::npos);
}

TEST_CASE("DIMORDINATE measures the distance from the origin along one axis") {
    Fixture f;
    REQUIRE(f.host->run("DIMORDINATE", {"120,45", "120,80"}).finished);
    REQUIRE(f.dims().size() == 1);
    const DimensionData* d = f.dims()[0];
    CHECK(d->kind == DimKind::Ordinate);
    CHECK(d->measurement == doctest::Approx(120));  // leader runs up: X is measured
    auto L = dim::layoutDimension(f.doc, *d);
    CHECK(!L.lines.empty());
    CHECK(L.text.utf8 == "120.00");

    REQUIRE(f.host->run("DIMORDINATE", {"120,45", "170,45"}).finished);
    REQUIRE(f.dims().size() == 2);
    const DimensionData* e = nullptr;
    for (const DimensionData* x : f.dims())
        if (x != d)
            e = x;
    REQUIRE(e);
    CHECK(e->measurement == doctest::Approx(45));  // leader runs across: Y is measured
    REQUIRE(f.host->run("DIMORDINATE", {"10,10", "Xdatum", "60,15"}).finished);
    CHECK(f.dims().size() == 3);
}

TEST_CASE("DIMJOGGED dimensions the radius with a jog in the line") {
    Fixture f;
    CircleData c;
    c.center = {0, 0};
    c.radius = 40;
    EntityId id = f.addEntity(c);
    REQUIRE(f.host
                ->run("DIMJOGGED",
                      {ScriptItem::pick(id, {40, 0}), "100,0", "70,20", ScriptItem::enter()})
                .finished);
    REQUIRE(f.dims().size() == 1);
    const DimensionData* d = f.dims()[0];
    CHECK(d->kind == DimKind::Jogged);
    CHECK(d->measurement == doctest::Approx(40));
    auto L = dim::layoutDimension(f.doc, *d);
    REQUIRE(!L.lines.empty());
    CHECK(L.lines[0].pts.size() == 6);  // line, four points of the jog, arc end
    CHECK(L.text.utf8 == "R40.00");
}
